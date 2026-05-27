// -----------------------------------------------------------------------------
// transaction_service_preview_authorization.cpp
// Handles preview-start authorization on the transaction service manager object.
// Session bus keeps the local development path unchanged. System bus allows only
// the active local desktop user to start preview work without prompting.
// -----------------------------------------------------------------------------
#include "transaction_service_internal.hpp"

#include "debug_trace.hpp"
#include "i18n.hpp"

#include <polkit/polkit.h>

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Return true when the test-only service should skip preview authorization.
// Headless Docker system bus smoke tests do not run under an active desktop
// session, so they cannot satisfy the real system-bus preview policy.
// -----------------------------------------------------------------------------
static bool
disable_preview_authorization_for_tests_requested()
{
  const char *disable_preview_authorization = g_getenv("SERVICE_TEST_DISABLE_PREVIEW_AUTHORIZATION");
  return disable_preview_authorization && g_strcmp0(disable_preview_authorization, "1") == 0;
}
#endif

// -----------------------------------------------------------------------------
// Authorize a preview start on the manager object.
// -----------------------------------------------------------------------------
bool
authorize_preview_start(TransactionService *service, const char *sender, std::string &error_out)
{
  error_out.clear();

  if (!service) {
    error_out = _("Transaction service authorization state is not available.");
    return false;
  }

#ifdef DNFUI_BUILD_TESTS
  if (disable_preview_authorization_for_tests_requested()) {
    DNFUI_TRACE("Transaction service preview authorization skipped (test-only service)");
    return true;
  }
#endif

  if (service->bus_type != G_BUS_TYPE_SYSTEM) {
    DNFUI_TRACE("Transaction service preview authorization skipped (session bus)");
    return true;
  }

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
  PolkitAuthorizationResult *result = polkit_authority_check_authorization_sync(
      authority, subject, kPreviewActionId, nullptr, POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE, nullptr, &error);
  g_object_unref(subject);
  g_object_unref(authority);

  if (!result) {
    error_out = error ? error->message : _("Authorization check failed.");
    DNFUI_TRACE("Transaction service preview authorization failed sender=%s error=%s", sender, error_out.c_str());
    g_clear_error(&error);
    return false;
  }

  bool authorized = polkit_authorization_result_get_is_authorized(result);
  g_object_unref(result);

  if (!authorized) {
    error_out = _("Not authorized to prepare package transaction previews.");
    DNFUI_TRACE("Transaction service preview authorization denied sender=%s", sender);
    return false;
  }

  DNFUI_TRACE("Transaction service preview authorization granted sender=%s", sender);
  return true;
}
