// -----------------------------------------------------------------------------
// transaction_service_authorization.cpp
// Handles authorization for Apply calls.
// Session bus runs without Polkit for local tests. System bus asks Polkit before
// starting package changes.
// -----------------------------------------------------------------------------
#include "transaction_service_internal.hpp"

#include "debug_trace.hpp"
#include "i18n.hpp"

#include <polkit/polkit.h>

#include <memory>

// -----------------------------------------------------------------------------
// Complete the Apply request after authorization succeeds.
// Called either immediately on session bus or from async callback on system bus.
// -----------------------------------------------------------------------------
void
complete_apply_request(TransactionSession *session, GDBusMethodInvocation *invocation)
{
  if (!session || !invocation) {
    return;
  }

  bool expected = false;
  if (!session->service->apply_running.compare_exchange_strong(expected, true)) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Another transaction apply is already running."));
    return;
  }

  session->cancelled = false;
  set_transaction_running(session, TransactionStage::APPLY_RUNNING);
  g_dbus_method_invocation_return_value(invocation, nullptr);
  g_idle_add_full(G_PRIORITY_DEFAULT, start_transaction_apply, session, nullptr);
}

// -----------------------------------------------------------------------------
// Context passed to the polkit authorization callback.
// Stores the object path and service pointer so the callback can look up the
// session after authorization finishes or ignore the result during shutdown.
// -----------------------------------------------------------------------------
struct AuthorizationCallbackContext {
  TransactionService *service = nullptr;
  std::string object_path;
};

// -----------------------------------------------------------------------------
// Finish the polkit authorization check for Apply.
// The service and session are looked up again because the client may have
// released the request or the service may have started shutdown while
// authorization was still running.
// -----------------------------------------------------------------------------
static void
on_apply_authorization_result(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  std::unique_ptr<AuthorizationCallbackContext> context(static_cast<AuthorizationCallbackContext *>(user_data));
  if (!context || !context->service) {
    return;
  }

  // Ignore the result after service shutdown has started.
  if (context->service->shutting_down.load()) {
    DNFUI_TRACE("Transaction service authorization callback ignored during shutdown path=%s",
                context->object_path.c_str());
    return;
  }

  // The request may have been released while authorization was running.
  auto it = context->service->transactions.find(context->object_path);
  if (it == context->service->transactions.end()) {
    DNFUI_TRACE("Transaction service authorization callback session not found path=%s", context->object_path.c_str());
    return;
  }

  TransactionSession *session = it->second.get();
  GDBusMethodInvocation *invocation = nullptr;
  bool preview_ready = false;
  bool preview_empty = false;
  {
    std::lock_guard<std::mutex> lock(session->state_mutex);
    if (!session->pending_apply_invocation) {
      return;
    }
    invocation = session->pending_apply_invocation;
    session->pending_apply_invocation = nullptr;
    preview_ready = session->stage == TransactionStage::PREVIEW_READY && session->finished.load() && session->success;
    preview_empty = session->preview.empty();
  }

  // The request must still be ready for apply after authorization completes.
  if (!preview_ready) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Transaction state changed during authorization."));
    g_object_unref(invocation);
    return;
  }

  if (preview_empty) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("No package changes are available."));
    g_object_unref(invocation);
    return;
  }

  GError *error = nullptr;
  PolkitAuthority *authority = POLKIT_AUTHORITY(source_object);
  PolkitAuthorizationResult *result = polkit_authority_check_authorization_finish(authority, res, &error);

  if (!result) {
    std::string error_msg = error ? error->message : _("Authorization check failed.");
    DNFUI_TRACE("Transaction service apply authorization failed path=%s error=%s",
                session->object_path.c_str(),
                error_msg.c_str());
    g_clear_error(&error);
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "%s", error_msg.c_str());
    g_object_unref(invocation);
    return;
  }

  bool authorized = polkit_authorization_result_get_is_authorized(result);
  g_object_unref(result);

  if (!authorized) {
    DNFUI_TRACE("Transaction service apply authorization denied path=%s", session->object_path.c_str());
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "%s", _("Not authorized to apply package transactions."));
    g_object_unref(invocation);
    return;
  }

  DNFUI_TRACE("Transaction service apply authorization granted path=%s", session->object_path.c_str());
  complete_apply_request(session, invocation);
  g_object_unref(invocation);
}

// -----------------------------------------------------------------------------
// Start authorization for an Apply request.
// The session bus skips polkit for development and tests. The system bus starts
// a polkit check and lets the callback reply to the D-Bus method call.
// -----------------------------------------------------------------------------
bool
start_authorize_apply_request(TransactionSession *session, GDBusMethodInvocation *invocation, std::string &error_out)
{
  error_out.clear();

  if (!session || !session->service || !invocation) {
    error_out = _("Transaction service authorization state is not available.");
    return false;
  }

  // Session bus mode remains available for local development and Docker tests.
  if (session->service->bus_type != G_BUS_TYPE_SYSTEM) {
    DNFUI_TRACE("Transaction service apply authorization skipped (session bus) path=%s", session->object_path.c_str());
    return true;
  }

  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!sender || !*sender) {
    error_out = _("Could not determine the caller identity.");
    return false;
  }

  GError *error = nullptr;
  PolkitAuthority *authority = polkit_authority_get_sync(nullptr, &error);
  if (!authority) {
    error_out = error ? error->message : _("Failed to contact the authorization service.");
    g_clear_error(&error);
    return false;
  }

  PolkitSubject *subject = polkit_system_bus_name_new(sender);

  // Reserve the pending authorization slot before starting the async request.
  // This keeps concurrent Apply calls from starting two authorization checks for
  // the same transaction request object.
  {
    std::lock_guard<std::mutex> lock(session->state_mutex);
    if (session->pending_apply_invocation) {
      error_out = _("Apply authorization is already in progress.");
      g_object_unref(subject);
      g_object_unref(authority);
      return false;
    }
    session->pending_apply_invocation = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
  }

  // Pass enough data for the callback to find the request again.
  auto *callback_context = new AuthorizationCallbackContext();
  callback_context->service = session->service;
  callback_context->object_path = session->object_path;

  DNFUI_TRACE("Transaction service apply authorization start (async) path=%s", session->object_path.c_str());
  polkit_authority_check_authorization(authority,
                                       subject,
                                       kApplyActionId,
                                       nullptr,
                                       POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                       nullptr,
                                       on_apply_authorization_result,
                                       callback_context);

  g_object_unref(subject);
  g_object_unref(authority);

  // Authorization started successfully. The callback will reply to the caller.
  return true;
}
