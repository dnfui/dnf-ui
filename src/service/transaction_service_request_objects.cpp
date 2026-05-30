// -----------------------------------------------------------------------------
// transaction_service_request_objects.cpp
// Handles methods on each transaction request object.
// Request objects expose preview state, apply, cancel, release, and result reads.
// -----------------------------------------------------------------------------
#include "transaction_service_internal.hpp"

#include "debug_trace.hpp"
#include "i18n.hpp"

// -----------------------------------------------------------------------------
// Per transaction object handling
// -----------------------------------------------------------------------------
static bool
disable_auto_release_for_tests_requested()
{
#ifdef DNFUI_BUILD_TESTS
  // The Docker system bus smoke tests use one gdbus process per method.
  // The test-only service binary allows those scripts to keep one request
  // reachable after the StartTransaction caller exits.
  const char *disable_auto_release = g_getenv("SERVICE_TEST_DISABLE_AUTO_RELEASE");
  return disable_auto_release && g_strcmp0(disable_auto_release, "1") == 0;
#else
  return false;
#endif
}

// -----------------------------------------------------------------------------
// Check whether a request object method call comes from the client that created
// that request object.
// -----------------------------------------------------------------------------
static bool
request_call_is_from_owner(TransactionSession *session, GDBusMethodInvocation *invocation, std::string &error_out)
{
  error_out.clear();

  if (!session || !session->service) {
    error_out = _("Transaction session is not available.");
    return false;
  }

  // The installed system bus service gives each request object to the client
  // that created it. Other clients may know the object path, but they must not
  // be allowed to read, cancel, apply, or release that request.
  //
  // The session bus path is only used for local service tests. The test-only
  // service binary also has one system bus smoke path that opts out so short
  // lived gdbus calls can inspect the request object after StartTransaction.
  if (session->service->bus_type != G_BUS_TYPE_SYSTEM || disable_auto_release_for_tests_requested()) {
    return true;
  }

  std::string sender;
  if (!get_invocation_sender(invocation, sender, error_out)) {
    return false;
  }

  if (sender != session->owner_name) {
    error_out = _("Transaction request belongs to another client.");
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Handle one D-Bus method call for a live transaction request object.
// -----------------------------------------------------------------------------
static void
on_transaction_method_call(GDBusConnection *,
                           const gchar *,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *method_name,
                           GVariant *,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data)
{
  (void)object_path;

  TransactionSession *session = static_cast<TransactionSession *>(user_data);
  if (!session) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Transaction session is not available."));
    return;
  }

  if (g_strcmp0(interface_name, kTransactionInterface) != 0) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", _("Unknown method."));
    return;
  }

  // Check ownership before any request state can be read or changed.
  std::string owner_error;
  if (!request_call_is_from_owner(session, invocation, owner_error)) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "%s", owner_error.c_str());
    return;
  }

  if (g_strcmp0(method_name, "Cancel") == 0) {
    DNFUI_TRACE("Transaction service cancel path=%s", object_path);

    bool has_pending_apply = false;
    TransactionStage stage = TransactionStage::PREVIEW_RUNNING;
    bool finished = false;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      has_pending_apply = session->pending_apply_invocation != nullptr;
      stage = session->stage;
      finished = session->finished.load();
    }

    if (has_pending_apply) {
      g_dbus_method_invocation_return_error(
          invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Cannot cancel while authorization is pending."));
      return;
    }

    if (stage == TransactionStage::APPLY_RUNNING) {
      g_dbus_method_invocation_return_error(invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_NOT_SUPPORTED,
                                            "%s",
                                            _("Cancellation is not supported while apply is running."));
      return;
    }

    // A transaction that has already reached a final state other than PREVIEW_READY
    // (e.g. PREVIEW_FAILED, CANCELLED, APPLY_SUCCEEDED, APPLY_FAILED) has nothing left to cancel.
    // Return success without re-emitting signals to avoid confusing the client.
    if (finished && stage != TransactionStage::PREVIEW_READY) {
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }

    session->cancelled = true;

    // If the preview worker already finished (PREVIEW_READY), clear the flag.
    // That lets emit_transaction_progress send the cancellation line before the Finished signal.
    // The reset is safe here: PREVIEW_READY is only reached after the worker exits,
    // so no thread is concurrently modifying session->finished.
    if (finished) {
      session->finished = false;
    }

    emit_transaction_progress(session, _("Transaction preview was cancelled."));
    emit_transaction_finished(session, TransactionStage::CANCELLED, false, _("Transaction preview was cancelled."));
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }

  if (g_strcmp0(method_name, "Apply") == 0) {
    bool preview_ready = false;
    bool preview_empty = false;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      preview_ready = session->stage == TransactionStage::PREVIEW_READY && session->finished.load() && session->success;
      preview_empty = session->preview.empty();
    }

    if (!preview_ready) {
      g_dbus_method_invocation_return_error(invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "%s",
                                            _("Transaction preview must succeed before apply can start."));
      return;
    }

    if (preview_empty) {
      g_dbus_method_invocation_return_error(
          invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("No package changes are available."));
      return;
    }

    std::string error_out;
    if (!start_authorize_apply_request(session, invocation, error_out)) {
      DNFUI_TRACE(
          "Transaction service apply authorization start failed path=%s error=%s", object_path, error_out.c_str());
      g_dbus_method_invocation_return_error(
          invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "%s", error_out.c_str());
      return;
    }

    // Session bus: authorization succeeded immediately, proceed with apply.
    // System bus: async authorization in progress, callback will complete the request.
    if (session->service->bus_type != G_BUS_TYPE_SYSTEM) {
      complete_apply_request(session, invocation);
    }

    return;
  }

  if (g_strcmp0(method_name, "Release") == 0) {
    bool has_pending_apply = false;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      has_pending_apply = session->pending_apply_invocation != nullptr;
    }

    if (has_pending_apply) {
      g_dbus_method_invocation_return_error(invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "%s",
                                            _("Transaction request cannot be released while authorization is "
                                              "pending."));
      return;
    }

    if (!session->finished.load()) {
      g_dbus_method_invocation_return_error(invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "%s",
                                            _("Transaction request cannot be released while work is still running."));
      return;
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
    queue_transaction_release(session);
    return;
  }

  if (g_strcmp0(method_name, "GetPreview") == 0) {
    TransactionPreview preview_copy;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);

      if (session->stage != TransactionStage::PREVIEW_READY || !session->finished.load() || !session->success) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Transaction preview is not available."));
        return;
      }

      preview_copy = session->preview;
    }

    GVariantBuilder install_builder;
    GVariantBuilder upgrade_builder;
    GVariantBuilder downgrade_builder;
    GVariantBuilder reinstall_builder;
    GVariantBuilder remove_builder;

    g_variant_builder_init(&install_builder, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&upgrade_builder, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&downgrade_builder, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&reinstall_builder, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&remove_builder, G_VARIANT_TYPE("as"));

    append_transaction_preview_array(install_builder, preview_copy.install);
    append_transaction_preview_array(upgrade_builder, preview_copy.upgrade);
    append_transaction_preview_array(downgrade_builder, preview_copy.downgrade);
    append_transaction_preview_array(reinstall_builder, preview_copy.reinstall);
    append_transaction_preview_array(remove_builder, preview_copy.remove);
    const gint64 disk_space_delta = static_cast<gint64>(preview_copy.disk_space_delta);

    // Return the preview arrays in the same order as the GetPreview D-Bus signature.
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(asasasasasx)",
                                                        &install_builder,
                                                        &upgrade_builder,
                                                        &downgrade_builder,
                                                        &reinstall_builder,
                                                        &remove_builder,
                                                        disk_space_delta));
    return;
  }

  if (g_strcmp0(method_name, "GetResult") == 0) {
    const char *stage_name = nullptr;
    bool finished = false;
    bool success = false;
    std::string details;

    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      stage_name = transaction_stage_name(session->stage);
      finished = session->finished.load();
      success = session->success;
      details = session->details;
    }

    // Return stage, finished, success, and details in the GetResult reply order.
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(sbbs)", stage_name, finished, success, details.c_str()));
    return;
  }

  g_dbus_method_invocation_return_error(
      invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", _("Unknown method."));
}

static const GDBusInterfaceVTable kTransactionVTable = {
  on_transaction_method_call,
  nullptr,
  nullptr,
  nullptr,
};

// -----------------------------------------------------------------------------
// Client disconnect handling
// -----------------------------------------------------------------------------
// Release transaction requests when their owning client disconnects.
// -----------------------------------------------------------------------------
static void
on_client_name_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  (void)connection;

  TransactionService *service = static_cast<TransactionService *>(user_data);
  if (!service || !name) {
    return;
  }

  DNFUI_TRACE("Transaction service client disconnected name=%s", name);

  // Collect matching paths before releasing sessions from the map.
  std::vector<std::string> orphaned_paths;
  for (const auto &[path, session] : service->transactions) {
    if (session->owner_name == name) {
      orphaned_paths.push_back(path);
    }
  }

  for (const auto &path : orphaned_paths) {
    DNFUI_TRACE("Transaction service auto-releasing orphaned session path=%s owner=%s", path.c_str(), name);
    queue_transaction_release(service->transactions[path].get());
  }
}

// -----------------------------------------------------------------------------
// Read the unique bus name of the caller that invoked StartTransaction.
// -----------------------------------------------------------------------------
bool
get_invocation_sender(GDBusMethodInvocation *invocation, std::string &sender_out, std::string &error_out)
{
  sender_out.clear();
  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!sender || !*sender) {
    error_out = _("Could not determine the client bus name.");
    return false;
  }

  sender_out = sender;
  return true;
}

// -----------------------------------------------------------------------------
// Create and register one new transaction request object on the bus.
// Watches the client's unique bus name to auto-release the session if the client disconnects without calling Release.
// -----------------------------------------------------------------------------
TransactionSession *
create_transaction_session(TransactionService *service,
                           const TransactionRequest &request,
                           const std::string &owner_name,
                           std::string &error_out)
{
  error_out.clear();
  if (!service || !service->connection || !service->transaction_node_info) {
    error_out = _("Transaction service is not ready.");
    return nullptr;
  }

  auto session = std::make_unique<TransactionSession>();
  session->service = service;
  session->request = request;
  session->object_path =
      std::string(kManagerObjectPath) + "/requests/" + std::to_string(service->next_transaction_id++);
  session->owner_name = owner_name;

  if (service_request_limit_reached(service, session->owner_name, error_out)) {
    return nullptr;
  }

  GError *error = nullptr;
  session->registration_id = g_dbus_connection_register_object(service->connection,
                                                               session->object_path.c_str(),
                                                               service->transaction_node_info->interfaces[0],
                                                               &kTransactionVTable,
                                                               session.get(),
                                                               nullptr,
                                                               &error);
  if (session->registration_id == 0) {
    error_out = error ? error->message : _("Failed to register transaction object.");
    g_clear_error(&error);
    return nullptr;
  }

  // Watch for client disconnects on the system bus.
  // Session bus tests often use a fresh connection per call, so they opt out.
  // The test-only service binary has one system bus smoke path that also opts
  // out so the request object survives the first gdbus process exit.
  if (service->bus_type == G_BUS_TYPE_SYSTEM && !disable_auto_release_for_tests_requested()) {
    session->owner_watch_id = g_bus_watch_name_on_connection(service->connection,
                                                             session->owner_name.c_str(),
                                                             G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                             nullptr,
                                                             on_client_name_vanished,
                                                             service,
                                                             nullptr);
  }

  TransactionSession *raw = session.get();
  service->transactions.emplace(session->object_path, std::move(session));
  return raw;
}
