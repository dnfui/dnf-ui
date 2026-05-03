// -----------------------------------------------------------------------------
// src/transaction_service_client.cpp
// GUI-side D-Bus client for the transaction service
// Starts transaction requests, waits for preview and apply state changes, reads
// structured preview data, forwards service progress lines, and releases
// finished requests when the GUI no longer needs them.
// -----------------------------------------------------------------------------
#include "transaction_service_client.hpp"

#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "service/transaction_service_dbus.hpp"
#include "transaction_request.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <mutex>
#include <string>

namespace {

// -----------------------------------------------------------------------------
// Transaction service result state returned by GetResult
// -----------------------------------------------------------------------------
struct TransactionServiceResult {
  std::string stage;
  bool finished = false;
  bool success = false;
  std::string details;
};

struct TransactionServiceProgressForwarder {
  const std::function<void(const std::string &)> *progress_callback = nullptr;
};

struct TransactionServiceConnectionCache {
  std::mutex mutex;
  GDBusConnection *connection = nullptr;
  GBusType bus_type = G_BUS_TYPE_SYSTEM;
  std::string bus_address;
};

// -----------------------------------------------------------------------------
// Select the D-Bus bus used by the transaction service client
// -----------------------------------------------------------------------------
static GBusType
get_transaction_service_bus_type()
{
  // Docker GUI testing uses the session bus path while native Polkit uses the system bus.
  const char *bus_mode = g_getenv("DNFUI_TRANSACTION_BUS");
  if (bus_mode && g_strcmp0(bus_mode, "session") == 0) {
    return G_BUS_TYPE_SESSION;
  }

  return G_BUS_TYPE_SYSTEM;
}

// -----------------------------------------------------------------------------
// Return the process-local cache that keeps the GUI service connection alive
// -----------------------------------------------------------------------------
static TransactionServiceConnectionCache &
get_transaction_service_connection_cache()
{
  static TransactionServiceConnectionCache cache;
  return cache;
}

// -----------------------------------------------------------------------------
// Build the StartTransaction argument tuple for the D-Bus service call
// -----------------------------------------------------------------------------
static GVariant *
build_start_transaction_parameters(const TransactionRequest &request)
{
  GVariantBuilder install_builder;
  GVariantBuilder remove_builder;
  GVariantBuilder reinstall_builder;
  g_variant_builder_init(&install_builder, G_VARIANT_TYPE("as"));
  g_variant_builder_init(&remove_builder, G_VARIANT_TYPE("as"));
  g_variant_builder_init(&reinstall_builder, G_VARIANT_TYPE("as"));

  for (const auto &spec : request.install) {
    g_variant_builder_add(&install_builder, "s", spec.c_str());
  }

  for (const auto &spec : request.remove) {
    g_variant_builder_add(&remove_builder, "s", spec.c_str());
  }

  for (const auto &spec : request.reinstall) {
    g_variant_builder_add(&reinstall_builder, "s", spec.c_str());
  }

  // Pack the request arrays in install, remove, and reinstall order.
  return g_variant_new("(asasas)", &install_builder, &remove_builder, &reinstall_builder);
}

// -----------------------------------------------------------------------------
// Connect to the D-Bus transaction service used by the GUI client
// -----------------------------------------------------------------------------
static GDBusConnection *
connect_transaction_service(std::string &error_out)
{
  error_out.clear();

  GBusType bus_type = get_transaction_service_bus_type();
  const char *bus_address = bus_type == G_BUS_TYPE_SESSION ? g_getenv("DBUS_SESSION_BUS_ADDRESS") : nullptr;
  std::string bus_address_key = bus_address ? bus_address : "";
  TransactionServiceConnectionCache &cache = get_transaction_service_connection_cache();

  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.connection && cache.bus_type == bus_type && cache.bus_address == bus_address_key &&
        !g_dbus_connection_is_closed(cache.connection)) {
      DNFUI_TRACE("Transaction service client connect bus=%s cached",
                  bus_type == G_BUS_TYPE_SESSION ? "session" : "system");
      return G_DBUS_CONNECTION(g_object_ref(cache.connection));
    }

    if (cache.connection) {
      g_object_unref(cache.connection);
      cache.connection = nullptr;
    }
  }

  DNFUI_TRACE("Transaction service client connect bus=%s", bus_type == G_BUS_TYPE_SESSION ? "session" : "system");

  GError *error = nullptr;
  GDBusConnection *connection = nullptr;
  if (bus_type == G_BUS_TYPE_SESSION && bus_address && *bus_address) {
    connection = g_dbus_connection_new_for_address_sync(
        bus_address,
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr,
        nullptr,
        &error);
  } else {
    connection = g_bus_get_sync(bus_type, nullptr, &error);
  }
  if (!connection) {
    error_out = error ? error->message : _("Could not connect to the transaction service bus.");
    g_clear_error(&error);
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (!cache.connection) {
      cache.connection = G_DBUS_CONNECTION(g_object_ref(connection));
      cache.bus_type = bus_type;
      cache.bus_address = bus_address_key;
    } else if (cache.bus_type == bus_type && cache.bus_address == bus_address_key &&
               !g_dbus_connection_is_closed(cache.connection)) {
      g_object_unref(connection);
      return G_DBUS_CONNECTION(g_object_ref(cache.connection));
    } else {
      g_object_unref(cache.connection);
      cache.connection = G_DBUS_CONNECTION(g_object_ref(connection));
      cache.bus_type = bus_type;
      cache.bus_address = bus_address_key;
    }
  }

  return connection;
}

// -----------------------------------------------------------------------------
// Start a new transaction request and return its service object path
// -----------------------------------------------------------------------------
static bool
start_transaction_request(GDBusConnection *connection,
                          const TransactionRequest &request,
                          std::string &transaction_path_out,
                          std::string &error_out)
{
  transaction_path_out.clear();
  error_out.clear();

  if (!connection) {
    error_out = _("Transaction service connection is not available.");
    return false;
  }

  GError *error = nullptr;
  GVariant *start_reply = g_dbus_connection_call_sync(connection,
                                                      kTransactionServiceName,
                                                      kTransactionServiceManagerPath,
                                                      kTransactionServiceManagerInterface,
                                                      "StartTransaction",
                                                      build_start_transaction_parameters(request),
                                                      G_VARIANT_TYPE("(o)"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      -1,
                                                      nullptr,
                                                      &error);
  if (!start_reply) {
    error_out = error ? error->message : _("Could not start the transaction service request.");
    g_clear_error(&error);
    return false;
  }

  const gchar *path = nullptr;
  g_variant_get(start_reply, "(&o)", &path);
  transaction_path_out = path ? path : "";
  g_variant_unref(start_reply);

  if (transaction_path_out.empty()) {
    error_out = _("Transaction service returned an empty request path.");
    return false;
  }

  DNFUI_TRACE("Transaction service client start path=%s", transaction_path_out.c_str());

  return true;
}

// -----------------------------------------------------------------------------
// Start a new upgrade-all transaction request and return its service object path
// -----------------------------------------------------------------------------
static bool
start_upgrade_all_transaction_request(GDBusConnection *connection,
                                      std::string &transaction_path_out,
                                      std::string &error_out)
{
  transaction_path_out.clear();
  error_out.clear();

  if (!connection) {
    error_out = _("Transaction service connection is not available.");
    return false;
  }

  GError *error = nullptr;
  GVariant *start_reply = g_dbus_connection_call_sync(connection,
                                                      kTransactionServiceName,
                                                      kTransactionServiceManagerPath,
                                                      kTransactionServiceManagerInterface,
                                                      "StartUpgradeAllTransaction",
                                                      nullptr,
                                                      G_VARIANT_TYPE("(o)"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      -1,
                                                      nullptr,
                                                      &error);
  if (!start_reply) {
    error_out = error ? error->message : _("Could not start the upgrade-all transaction service request.");
    g_clear_error(&error);
    return false;
  }

  const gchar *path = nullptr;
  g_variant_get(start_reply, "(&o)", &path);
  transaction_path_out = path ? path : "";
  g_variant_unref(start_reply);

  if (transaction_path_out.empty()) {
    error_out = _("Transaction service returned an empty request path.");
    return false;
  }

  DNFUI_TRACE("Transaction service client start upgrade-all path=%s", transaction_path_out.c_str());

  return true;
}

// -----------------------------------------------------------------------------
// Read the current state of a service transaction request
// -----------------------------------------------------------------------------
static bool
get_transaction_result(GDBusConnection *connection,
                       const std::string &transaction_path,
                       TransactionServiceResult &result_out,
                       std::string &error_out)
{
  result_out = {};
  error_out.clear();

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kTransactionServiceName,
                                                transaction_path.c_str(),
                                                kTransactionServiceRequestInterface,
                                                "GetResult",
                                                nullptr,
                                                G_VARIANT_TYPE("(sbbs)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Failed to read transaction service result.");
    g_clear_error(&error);
    return false;
  }

  // Read stage, finished, success, and details from the GetResult reply.
  const gchar *stage = nullptr;
  gboolean finished = FALSE;
  gboolean success = FALSE;
  const gchar *details = nullptr;
  g_variant_get(reply, "(&sbbs)", &stage, &finished, &success, &details);
  result_out.stage = stage ? stage : "";
  result_out.finished = finished;
  result_out.success = success;
  result_out.details = details ? details : "";
  g_variant_unref(reply);

  return true;
}

// -----------------------------------------------------------------------------
// Read the structured preview data from a prepared service request
// -----------------------------------------------------------------------------
static bool
get_transaction_preview(GDBusConnection *connection,
                        const std::string &transaction_path,
                        TransactionPreview &preview_out,
                        std::string &error_out)
{
  preview_out = {};
  error_out.clear();

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kTransactionServiceName,
                                                transaction_path.c_str(),
                                                kTransactionServiceRequestInterface,
                                                "GetPreview",
                                                nullptr,
                                                G_VARIANT_TYPE("(asasasasasx)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Failed to read transaction service preview.");
    g_clear_error(&error);
    return false;
  }

  // Unpack the preview reply into owned string arrays and the disk space delta.
  gchar **install = nullptr;
  gchar **upgrade = nullptr;
  gchar **downgrade = nullptr;
  gchar **reinstall = nullptr;
  gchar **remove = nullptr;
  gint64 disk_space_delta = 0;
  g_variant_get(reply, "(^as^as^as^as^asx)", &install, &upgrade, &downgrade, &reinstall, &remove, &disk_space_delta);

  auto append_specs = [](std::vector<std::string> &target, gchar **specs) {
    if (!specs) {
      return;
    }

    for (gchar **it = specs; *it; ++it) {
      target.emplace_back(*it);
    }
  };

  append_specs(preview_out.install, install);
  append_specs(preview_out.upgrade, upgrade);
  append_specs(preview_out.downgrade, downgrade);
  append_specs(preview_out.reinstall, reinstall);
  append_specs(preview_out.remove, remove);
  preview_out.disk_space_delta = static_cast<long long>(disk_space_delta);

  g_strfreev(install);
  g_strfreev(upgrade);
  g_strfreev(downgrade);
  g_strfreev(reinstall);
  g_strfreev(remove);
  g_variant_unref(reply);

  return true;
}

// -----------------------------------------------------------------------------
// Release a finished service request that is no longer needed by the GUI
// -----------------------------------------------------------------------------
static bool
release_transaction_request(GDBusConnection *connection, const std::string &transaction_path, std::string &error_out)
{
  error_out.clear();

  if (!connection || transaction_path.empty()) {
    return true;
  }

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kTransactionServiceName,
                                                transaction_path.c_str(),
                                                kTransactionServiceRequestInterface,
                                                "Release",
                                                nullptr,
                                                nullptr,
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Failed to release transaction service request.");
    g_clear_error(&error);
    return false;
  }

  g_variant_unref(reply);
  return true;
}

// -----------------------------------------------------------------------------
// State shared between wait_for_transaction_stage and its Finished signal handler.
// -----------------------------------------------------------------------------
struct FinishedWaitState {
  bool received = false;
  bool service_disappeared = false;
  std::string stage;
  bool success = false;
  std::string details;
};

// -----------------------------------------------------------------------------
// Wait until the service transaction request leaves the given running stage.
// Subscribes to the Finished D-Bus signal so the wait wakes up immediately
// when the service reports a final state.
// context must already be the thread-default context of the calling thread so
// that signal callbacks are dispatched on the same context that is iterated.
// -----------------------------------------------------------------------------
static bool
wait_for_transaction_stage(GDBusConnection *connection,
                           const std::string &transaction_path,
                           const char *running_stage,
                           GMainContext *context,
                           TransactionServiceResult &result_out,
                           std::string &error_out)
{
  error_out.clear();

  // Subscribe to Finished before the first state poll.
  // This covers the short gap where the service could finish after the request
  // starts but before the client begins waiting for the result.
  FinishedWaitState wait_state;
  guint finished_id = g_dbus_connection_signal_subscribe(
      connection,
      kTransactionServiceName,
      kTransactionServiceRequestInterface,
      "Finished",
      transaction_path.c_str(),
      nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE,
      +[](GDBusConnection *,
          const gchar *,
          const gchar *,
          const gchar *,
          const gchar *,
          GVariant *parameters,
          gpointer user_data) {
        auto *state = static_cast<FinishedWaitState *>(user_data);
        const gchar *stage = nullptr;
        gboolean success = FALSE;
        const gchar *details = nullptr;
        g_variant_get(parameters, "(&sbs)", &stage, &success, &details);
        state->stage = stage ? stage : "";
        state->success = success;
        state->details = details ? details : "";
        state->received = true;
      },
      &wait_state,
      nullptr);

  guint name_owner_changed_id = g_dbus_connection_signal_subscribe(
      connection,
      "org.freedesktop.DBus",
      "org.freedesktop.DBus",
      "NameOwnerChanged",
      "/org/freedesktop/DBus",
      kTransactionServiceName,
      G_DBUS_SIGNAL_FLAGS_NONE,
      +[](GDBusConnection *,
          const gchar *,
          const gchar *,
          const gchar *,
          const gchar *,
          GVariant *parameters,
          gpointer user_data) {
        auto *state = static_cast<FinishedWaitState *>(user_data);
        const gchar *name = nullptr;
        const gchar *old_owner = nullptr;
        const gchar *new_owner = nullptr;
        g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
        if (name && g_strcmp0(name, kTransactionServiceName) == 0 && old_owner && *old_owner &&
            (!new_owner || !*new_owner)) {
          state->service_disappeared = true;
        }
      },
      &wait_state,
      nullptr);

  // One initial GetResult call handles the case where the service already
  // reached a final state before our Finished subscription was registered.
  if (!get_transaction_result(connection, transaction_path, result_out, error_out)) {
    g_dbus_connection_signal_unsubscribe(connection, name_owner_changed_id);
    g_dbus_connection_signal_unsubscribe(connection, finished_id);
    return false;
  }

  // Block on context until the Finished signal fires or the initial poll
  // already shows a final state. Any other signals pending on context
  // (such as Progress callbacks in the apply path) are also dispatched here.
  while (!wait_state.received && !wait_state.service_disappeared && result_out.stage == running_stage &&
         !result_out.finished) {
    g_main_context_iteration(context, TRUE);
  }

  g_dbus_connection_signal_unsubscribe(connection, name_owner_changed_id);
  g_dbus_connection_signal_unsubscribe(connection, finished_id);

  // Return a normal client error if the service disappears while the client
  // is still waiting for the final state signal.
  if (wait_state.service_disappeared && !wait_state.received) {
    error_out = _("Transaction service disappeared while waiting for the result.");
    return false;
  }

  // Fill the result from the final state signal and avoid a second GetResult call.
  if (wait_state.received) {
    result_out.stage = wait_state.stage;
    result_out.finished = true;
    result_out.success = wait_state.success;
    result_out.details = wait_state.details;
  }

  DNFUI_TRACE("Transaction service client stage path=%s stage=%s finished=%d success=%d",
              transaction_path.c_str(),
              result_out.stage.c_str(),
              result_out.finished ? 1 : 0,
              result_out.success ? 1 : 0);

  return true;
}

// -----------------------------------------------------------------------------
// Forward transaction progress signals from the service to the GUI callback
// -----------------------------------------------------------------------------
static void
on_transaction_progress_signal(GDBusConnection *,
                               const gchar *,
                               const gchar *,
                               const gchar *,
                               const gchar *,
                               GVariant *parameters,
                               gpointer user_data)
{
  auto *forwarder = static_cast<TransactionServiceProgressForwarder *>(user_data);
  if (!forwarder || !forwarder->progress_callback || !*forwarder->progress_callback) {
    return;
  }

  const gchar *line = nullptr;
  g_variant_get(parameters, "(&s)", &line);
  if (!line || !*line) {
    return;
  }

  DNFUI_TRACE("Transaction service client progress line=%s", line);
  (*forwarder->progress_callback)(line);
}

// -----------------------------------------------------------------------------
// Wait for a started preview request and read its structured preview
// -----------------------------------------------------------------------------
static bool
wait_for_started_transaction_preview(GDBusConnection *connection,
                                     const std::string &transaction_path,
                                     TransactionPreview &preview_out,
                                     std::string &error_out)
{
  TransactionServiceResult result;

  // Create a dedicated main context for the duration of the preview wait.
  // The Finished signal subscription inside wait_for_transaction_stage is
  // dispatched on the thread-default context, so it and the iterated context
  // must be the same object.
  GMainContext *wait_context = g_main_context_new();
  g_main_context_push_thread_default(wait_context);
  bool stage_ok =
      wait_for_transaction_stage(connection, transaction_path, "preview-running", wait_context, result, error_out);
  g_main_context_pop_thread_default(wait_context);
  g_main_context_unref(wait_context);

  if (!stage_ok) {
    std::string release_error;
    release_transaction_request(connection, transaction_path, release_error);
    return false;
  }

  if (result.stage != "preview-ready" || !result.finished || !result.success) {
    error_out = result.details.empty() ? _("Privileged transaction preview failed.") : result.details;
    DNFUI_TRACE(
        "Transaction service client preview failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
    std::string release_error;
    release_transaction_request(connection, transaction_path, release_error);
    return false;
  }

  if (!get_transaction_preview(connection, transaction_path, preview_out, error_out)) {
    DNFUI_TRACE(
        "Transaction service client get preview failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
    std::string release_error;
    release_transaction_request(connection, transaction_path, release_error);
    return false;
  }

  return true;
}

} // namespace

// -----------------------------------------------------------------------------
// Resolve a service-backed transaction preview and return its request path
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
  GDBusConnection *connection = connect_transaction_service(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  if (!start_transaction_request(connection, request, transaction_path_out, error_out)) {
    g_object_unref(connection);
    return false;
  }

  if (!wait_for_started_transaction_preview(connection, transaction_path_out, preview_out, error_out)) {
    transaction_path_out.clear();
    g_object_unref(connection);
    return false;
  }

  g_object_unref(connection);

  return true;
}

// -----------------------------------------------------------------------------
// Resolve an upgrade-all service-backed transaction preview
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
  GDBusConnection *connection = connect_transaction_service(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  if (!start_upgrade_all_transaction_request(connection, transaction_path_out, error_out)) {
    g_object_unref(connection);
    return false;
  }

  if (!wait_for_started_transaction_preview(connection, transaction_path_out, preview_out, error_out)) {
    transaction_path_out.clear();
    g_object_unref(connection);
    return false;
  }

  g_object_unref(connection);

  return true;
}

// -----------------------------------------------------------------------------
// Apply a previously previewed transaction request through the service
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
  GDBusConnection *connection = connect_transaction_service(connect_error);
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
    progress_subscription_id = g_dbus_connection_signal_subscribe(connection,
                                                                  kTransactionServiceName,
                                                                  kTransactionServiceRequestInterface,
                                                                  "Progress",
                                                                  transaction_path.c_str(),
                                                                  nullptr,
                                                                  G_DBUS_SIGNAL_FLAGS_NONE,
                                                                  on_transaction_progress_signal,
                                                                  &progress_forwarder,
                                                                  nullptr);

    append_progress(_("Privileged transaction preview ready."));
    append_progress(_("Requesting authorization and starting apply..."));

    GError *error = nullptr;
    GVariant *apply_reply = g_dbus_connection_call_sync(connection,
                                                        kTransactionServiceName,
                                                        transaction_path.c_str(),
                                                        kTransactionServiceRequestInterface,
                                                        "Apply",
                                                        nullptr,
                                                        nullptr,
                                                        G_DBUS_CALL_FLAGS_NONE,
                                                        -1,
                                                        nullptr,
                                                        &error);
    if (!apply_reply) {
      error_out = error ? error->message : _("Could not start the privileged apply request.");
      g_clear_error(&error);
      break;
    }
    g_variant_unref(apply_reply);

    append_progress(_("Waiting for privileged apply to finish..."));
    if (!wait_for_transaction_stage(connection, transaction_path, "apply-running", signal_context, result, error_out)) {
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
// Release a finished service request after it has been applied or discarded
// -----------------------------------------------------------------------------
void
transaction_service_client_release_request(const std::string &transaction_path)
{
  if (transaction_path.empty()) {
    return;
  }

  std::string connect_error;
  GDBusConnection *connection = connect_transaction_service(connect_error);
  if (!connection) {
    DNFUI_TRACE("Transaction service client release connect failed path=%s error=%s",
                transaction_path.c_str(),
                connect_error.c_str());
    return;
  }

  std::string error_out;
  if (!release_transaction_request(connection, transaction_path, error_out)) {
    DNFUI_TRACE(
        "Transaction service client release failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
  } else {
    DNFUI_TRACE("Transaction service client release done path=%s", transaction_path.c_str());
  }

  g_object_unref(connection);
}

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Close the cached service connection between tests.
// -----------------------------------------------------------------------------
void
transaction_service_client_reset_for_tests()
{
  TransactionServiceConnectionCache &cache = get_transaction_service_connection_cache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  if (!cache.connection) {
    return;
  }

  g_object_unref(cache.connection);
  cache.connection = nullptr;
  cache.bus_address.clear();
}
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
