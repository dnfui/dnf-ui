// -----------------------------------------------------------------------------
// transaction_service_client.cpp
// GUI-side D-Bus client for the transaction service.
// Owns the public preview, apply, and release flow used by the GTK frontend.
// Raw D-Bus calls and signal waiting live in the private client helper files.
// -----------------------------------------------------------------------------
#include "transaction_service_client.hpp"

#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "transaction_request.hpp"
#include "transaction_service_client_internal.hpp"

// -----------------------------------------------------------------------------
// Resolve a service-backed transaction preview and return its request path.
// -----------------------------------------------------------------------------
bool
transaction_service_client_preview_request(const TransactionRequest &request,
                                           TransactionPreview &preview_out,
                                           std::string &transaction_path_out,
                                           std::string &error_out)
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

  if (!transaction_service_client_wait_for_started_transaction_preview(
          connection, transaction_path_out, preview_out, error_out)) {
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
                                                       std::string &error_out)
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

  if (!transaction_service_client_wait_for_started_transaction_preview(
          connection, transaction_path_out, preview_out, error_out)) {
    transaction_path_out.clear();
    g_object_unref(connection);
    return false;
  }

  g_object_unref(connection);

  return true;
}

// -----------------------------------------------------------------------------
// Apply a previously previewed transaction request through the service.
// Progress is not returned by Apply itself. Progress arrives as D-Bus signals
// on the request object while this function waits for the final result.
// -----------------------------------------------------------------------------
bool
transaction_service_client_apply_started_request(const std::string &transaction_path,
                                                 const std::function<void(const std::string &)> &progress_callback,
                                                 std::string &error_out)
{
  error_out.clear();

  if (transaction_path.empty()) {
    error_out = _("Transaction service request path is empty.");
    return false;
  }

  auto append_progress = [&](const std::string &message) {
    if (progress_callback && !message.empty()) {
      progress_callback(message);
    }
  };

  append_progress(_("Connecting to transaction service..."));

  std::string connect_error;
  GDBusConnection *connection = transaction_service_client_connect(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  bool ok = false;
  TransactionServiceResult result;
  GMainContext *signal_context = g_main_context_new();
  g_main_context_push_thread_default(signal_context);
  TransactionServiceProgressForwarder progress_forwarder { &progress_callback };
  guint progress_subscription_id = 0;

  do {
    DNFUI_TRACE("Transaction service client start path=%s", transaction_path.c_str());
    // Listen before calling Apply so early service messages are not missed.
    progress_subscription_id =
        transaction_service_client_subscribe_progress(connection, transaction_path, &progress_forwarder);

    append_progress(_("Privileged transaction preview ready."));
    append_progress(_("Requesting authorization and starting apply..."));

    if (!transaction_service_client_start_apply_request(connection, transaction_path, error_out)) {
      break;
    }

    append_progress(_("Waiting for privileged apply to finish..."));
    if (!transaction_service_client_wait_for_transaction_stage(
            connection, transaction_path, "apply-running", signal_context, result, error_out)) {
      break;
    }

    if (result.stage != "apply-succeeded" || !result.finished || !result.success) {
      error_out = result.details.empty() ? _("Privileged apply failed.") : result.details;
      break;
    }

    append_progress(result.details.empty() ? _("Transaction applied successfully.") : result.details);
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
// Release a finished service request after it has been applied or discarded.
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
// EOF
// -----------------------------------------------------------------------------
