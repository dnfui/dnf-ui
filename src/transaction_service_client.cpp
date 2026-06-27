// -----------------------------------------------------------------------------
// transaction_service_client.cpp
// GUI-side D-Bus client for dnf5daemon transactions.
// Owns the public preview, apply, and release flow used by the GTK frontend.
// Raw D-Bus calls and signal waiting live in the private client helper files.
// -----------------------------------------------------------------------------
#include "transaction_service_client.hpp"

#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "transaction_request.hpp"
#include "transaction_service_client_internal.hpp"

namespace {

// -----------------------------------------------------------------------------
// Free data owned by one queued release task.
// -----------------------------------------------------------------------------
void
release_request_task_data_free(gpointer p)
{
  delete static_cast<std::string *>(p);
}

// -----------------------------------------------------------------------------
// Run the blocking Release call away from the GTK thread.
// -----------------------------------------------------------------------------
void
release_request_task(GTask *task, gpointer, gpointer task_data, GCancellable *)
{
  std::string *transaction_path = static_cast<std::string *>(task_data);
  if (transaction_path) {
    transaction_service_client_release_request(*transaction_path);
  }

  g_task_return_boolean(task, TRUE);
}

// -----------------------------------------------------------------------------
// Return true if a resolved daemon preview would remove or replace the running DNF UI package.
// Normal upgrades are allowed; destructive actions are rejected after dnf5daemon
// has resolved dependency, obsolete, and replacement actions.
// -----------------------------------------------------------------------------
bool
transaction_preview_removes_or_replaces_self_protected_package(const TransactionPreview &preview)
{
  std::vector<std::string> labels;
  labels.reserve(preview.remove.size() + preview.replaced.size());
  labels.insert(labels.end(), preview.remove.begin(), preview.remove.end());
  labels.insert(labels.end(), preview.replaced.begin(), preview.replaced.end());

  return dnf_backend_any_self_protected_package_label(labels);
}

// -----------------------------------------------------------------------------
// Reject daemon previews that would remove or replace DNF UI itself.
// -----------------------------------------------------------------------------
bool
verify_preview_keeps_running_app_package(const TransactionPreview &preview,
                                         std::string &error_out,
                                         const std::string &unsafe_message,
                                         const std::string &verify_failed_message)
{
  try {
    // A fresh app start may not have published the installed snapshot yet.
    // Refresh from the local rpmdb before checking whether the preview removes or replaces DNF UI itself.
    dnf_backend_refresh_installed_nevras();
    if (transaction_preview_removes_or_replaces_self_protected_package(preview)) {
      error_out = unsafe_message;
      return false;
    }
  } catch (const std::exception &e) {
    error_out = e.what();
    return false;
  } catch (...) {
    error_out = verify_failed_message;
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Resolve a daemon preview while the worker can answer daemon key prompts.
// -----------------------------------------------------------------------------
bool
resolve_transaction_preview(GDBusConnection *connection,
                            const std::string &transaction_path,
                            const TransactionKeyImportCallback &key_import_callback,
                            GCancellable *cancellable,
                            TransactionPreview &preview_out,
                            std::string &error_out,
                            std::vector<std::string> *upgrade_keys_out = nullptr)
{
  GMainContext *signal_context = g_main_context_new();
  g_main_context_push_thread_default(signal_context);

  TransactionServiceProgressForwarder progress_forwarder;
  progress_forwarder.key_import_callback = &key_import_callback;
  guint progress_subscription_id =
      transaction_service_client_subscribe_progress(connection, transaction_path, &progress_forwarder);

  bool ok = transaction_service_client_wait_for_started_transaction_preview(
      connection, transaction_path, &progress_forwarder, cancellable, preview_out, error_out, upgrade_keys_out);

  if (progress_subscription_id != 0) {
    g_dbus_connection_signal_unsubscribe(connection, progress_subscription_id);
  }
  g_main_context_pop_thread_default(signal_context);
  g_main_context_unref(signal_context);

  return ok;
}

} // namespace

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Test-only access to the resolved-preview self-protection check.
// -----------------------------------------------------------------------------
bool
transaction_service_client_testonly_verify_preview_keeps_running_app_package(const TransactionPreview &preview,
                                                                             std::string &error_out)
{
  return verify_preview_keeps_running_app_package(
      preview,
      error_out,
      "This transaction would remove or replace DNF UI itself.",
      "Could not verify whether this transaction would remove or replace DNF UI.");
}
#endif

// -----------------------------------------------------------------------------
// Resolve a service-backed transaction preview and return its request path.
// -----------------------------------------------------------------------------
bool
transaction_service_client_preview_request(const TransactionRequest &request,
                                           TransactionPreview &preview_out,
                                           std::string &transaction_path_out,
                                           std::string &error_out,
                                           const TransactionKeyImportCallback &key_import_callback,
                                           GCancellable *cancellable)
{
  preview_out = {};
  transaction_path_out.clear();
  error_out.clear();

  if (!request.validate(error_out)) {
    return false;
  }

  if (request.upgrade_all) {
    error_out = _("Use the upgrade-all preview helper for upgrade-all requests.");
    return false;
  }

  std::string connect_error;
  GDBusConnection *connection = transaction_service_client_connect(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  if (!transaction_service_client_start_transaction_request(connection, request, transaction_path_out, error_out)) {
    g_object_unref(connection);
    return false;
  }

  if (!resolve_transaction_preview(
          connection, transaction_path_out, key_import_callback, cancellable, preview_out, error_out)) {
    std::string release_error;
    transaction_service_client_release_transaction_request(connection, transaction_path_out, release_error);
    transaction_path_out.clear();
    g_object_unref(connection);
    return false;
  }

  if (!verify_preview_keeps_running_app_package(
          preview_out,
          error_out,
          _("This transaction would remove or replace DNF UI itself."),
          _("Could not verify whether this transaction would remove or replace DNF UI."))) {
    std::string release_error;
    transaction_service_client_release_transaction_request(connection, transaction_path_out, release_error);
    transaction_path_out.clear();
    g_object_unref(connection);
    return false;
  }

  g_object_unref(connection);

  return true;
}

// -----------------------------------------------------------------------------
// Resolve an upgrade-all service-backed transaction preview.
// -----------------------------------------------------------------------------
bool
transaction_service_client_preview_upgrade_all_request(TransactionPreview &preview_out,
                                                       std::string &transaction_path_out,
                                                       std::string &error_out,
                                                       const TransactionKeyImportCallback &key_import_callback,
                                                       GCancellable *cancellable)
{
  preview_out = {};
  transaction_path_out.clear();
  error_out.clear();

  std::string connect_error;
  GDBusConnection *connection = transaction_service_client_connect(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  if (!transaction_service_client_start_upgrade_all_transaction_request(connection, transaction_path_out, error_out)) {
    g_object_unref(connection);
    return false;
  }

  if (!resolve_transaction_preview(
          connection, transaction_path_out, key_import_callback, cancellable, preview_out, error_out)) {
    std::string release_error;
    transaction_service_client_release_transaction_request(connection, transaction_path_out, release_error);
    transaction_path_out.clear();
    g_object_unref(connection);
    return false;
  }

  auto release_preview_session = [&]() {
    std::string release_error;
    transaction_service_client_release_transaction_request(connection, transaction_path_out, release_error);
    transaction_path_out.clear();
  };

  if (!verify_preview_keeps_running_app_package(
          preview_out,
          error_out,
          _("Upgrade All would remove or replace DNF UI itself."),
          _("Could not verify whether Upgrade All would remove or replace DNF UI."))) {
    release_preview_session();
    g_object_unref(connection);
    return false;
  }

  g_object_unref(connection);

  return true;
}

// -----------------------------------------------------------------------------
// List upgrade package keys from the resolved daemon Upgrade All preview.
// The daemon package-list upgrades scope is only a candidate query. The resolver
// is the same source used by the actual Upgrade All preview.
// -----------------------------------------------------------------------------
bool
transaction_service_client_list_upgrade_keys(std::vector<std::string> &keys_out,
                                             std::string &error_out,
                                             GCancellable *cancellable)
{
  keys_out.clear();
  error_out.clear();

  std::string connect_error;
  GDBusConnection *connection = transaction_service_client_connect(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  std::string transaction_path;
  if (!transaction_service_client_start_upgrade_all_transaction_request(connection, transaction_path, error_out)) {
    g_object_unref(connection);
    return false;
  }

  TransactionPreview preview;
  bool ok = resolve_transaction_preview(connection, transaction_path, {}, cancellable, preview, error_out, &keys_out);

  std::string release_error;
  transaction_service_client_release_transaction_request(connection, transaction_path, release_error);
  g_object_unref(connection);

  if (!ok) {
    keys_out.clear();
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Apply a previously previewed transaction request through dnf5daemon.
// The apply call waits until the daemon finishes. Progress arrives as D-Bus
// signals while the call is running.
// -----------------------------------------------------------------------------
bool
transaction_service_client_apply_started_request(const std::string &transaction_path,
                                                 const std::function<void(const std::string &)> &progress_callback,
                                                 const TransactionKeyImportCallback &key_import_callback,
                                                 std::string &error_out)
{
  error_out.clear();

  if (transaction_path.empty()) {
    error_out = _("dnf5daemon session path is empty.");
    return false;
  }

  auto append_progress = [&](const std::string &message) {
    if (progress_callback && !message.empty()) {
      progress_callback(message);
    }
  };

  append_progress(_("Connecting to dnf5daemon..."));

  std::string connect_error;
  GDBusConnection *connection = transaction_service_client_connect(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  bool ok = false;
  GMainContext *signal_context = g_main_context_new();
  g_main_context_push_thread_default(signal_context);
  TransactionServiceProgressForwarder progress_forwarder;
  progress_forwarder.progress_callback = &progress_callback;
  progress_forwarder.key_import_callback = &key_import_callback;
  guint progress_subscription_id = 0;

  do {
    DNFUI_TRACE("Transaction service client start path=%s", transaction_path.c_str());
    // Listen before calling Apply so early service messages are not missed.
    progress_subscription_id =
        transaction_service_client_subscribe_progress(connection, transaction_path, &progress_forwarder);

    append_progress(_("Privileged transaction preview ready."));
    append_progress(_("Requesting authorization and starting apply..."));

    if (!transaction_service_client_start_apply_request(connection, transaction_path, &progress_forwarder, error_out)) {
      break;
    }

    append_progress(_("Transaction applied successfully."));
    DNFUI_TRACE("Transaction service client apply done path=%s", transaction_path.c_str());
    ok = true;
  } while (false);

  if (!ok) {
    DNFUI_TRACE(
        "Transaction service client apply failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
  }

  if (progress_subscription_id != 0) {
    g_dbus_connection_signal_unsubscribe(connection, progress_subscription_id);
  }
  g_main_context_pop_thread_default(signal_context);
  g_main_context_unref(signal_context);
  g_object_unref(connection);

  return ok;
}

// -----------------------------------------------------------------------------
// Release a finished daemon session after it has been applied or discarded.
// -----------------------------------------------------------------------------
void
transaction_service_client_release_request(const std::string &transaction_path)
{
  if (transaction_path.empty()) {
    return;
  }

  std::string connect_error;
  GDBusConnection *connection = transaction_service_client_connect(connect_error);
  if (!connection) {
    DNFUI_TRACE("Transaction service client release connect failed path=%s error=%s",
                transaction_path.c_str(),
                connect_error.c_str());
    return;
  }

  std::string error_out;
  if (!transaction_service_client_release_transaction_request(connection, transaction_path, error_out)) {
    DNFUI_TRACE(
        "Transaction service client release failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
  } else {
    DNFUI_TRACE("Transaction service client release done path=%s", transaction_path.c_str());
  }

  g_object_unref(connection);
}

// -----------------------------------------------------------------------------
// Queue request release on a worker thread so GTK cleanup does not wait on D-Bus.
// -----------------------------------------------------------------------------
void
transaction_service_client_release_request_async(const std::string &transaction_path)
{
  if (transaction_path.empty()) {
    return;
  }

  GTask *task = g_task_new(nullptr, nullptr, nullptr, nullptr);
  g_task_set_task_data(task, new std::string(transaction_path), release_request_task_data_free);
  g_task_run_in_thread(task, release_request_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
