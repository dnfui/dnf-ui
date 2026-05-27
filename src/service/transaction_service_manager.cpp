// -----------------------------------------------------------------------------
// transaction_service_manager.cpp
// Handles the transaction service manager object.
// Creates one request object for StartTransaction or StartUpgradeAllTransaction.
// -----------------------------------------------------------------------------
#include "transaction_service_internal.hpp"

#include "debug_trace.hpp"
#include "i18n.hpp"
#include "service/transaction_service_request_parser.hpp"

// -----------------------------------------------------------------------------
// Manager object handling
// -----------------------------------------------------------------------------
// Handle StartTransaction calls on the transaction service manager object.
// -----------------------------------------------------------------------------
static void
on_manager_method_call(GDBusConnection *,
                       const gchar *,
                       const gchar *,
                       const gchar *interface_name,
                       const gchar *method_name,
                       GVariant *parameters,
                       GDBusMethodInvocation *invocation,
                       gpointer user_data)
{
  TransactionService *service = static_cast<TransactionService *>(user_data);
  if (!service) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Service is not available."));
    return;
  }

  if (g_strcmp0(interface_name, kManagerInterface) != 0 ||
      (g_strcmp0(method_name, "StartTransaction") != 0 && g_strcmp0(method_name, "StartUpgradeAllTransaction") != 0)) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", _("Unknown method."));
    return;
  }

  TransactionRequest request;
  if (g_strcmp0(method_name, "StartUpgradeAllTransaction") == 0) {
    request.upgrade_all = true;
  } else {
    request = transaction_service_request_from_variant(parameters);
  }
  std::string error_out;

  std::string owner_name;
  if (!get_invocation_sender(invocation, owner_name, error_out)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", error_out.c_str());
    return;
  }

  if (!request.validate(error_out)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "%s", error_out.c_str());
    return;
  }

  // On the system bus, allow only the active local desktop user to start
  // preview work. Deny the request before creating a request object or doing
  // backend transaction resolution.
  if (!authorize_preview_start(service, owner_name.c_str(), error_out)) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "%s", error_out.c_str());
    return;
  }

  if (service_request_limit_reached(service, owner_name, error_out)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", error_out.c_str());
    return;
  }

  if (!validate_transaction_request_for_service(request, error_out)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "%s", error_out.c_str());
    return;
  }

  DNFUI_TRACE("Transaction service start install=%zu remove=%zu reinstall=%zu upgrade_all=%d",
              request.install.size(),
              request.remove.size(),
              request.reinstall.size(),
              request.upgrade_all ? 1 : 0);

  TransactionSession *session = create_transaction_session(service, request, owner_name, error_out);
  if (!session) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", error_out.c_str());
    return;
  }

  g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", session->object_path.c_str()));
  // Queue the preview start after the request object is returned to the caller.
  g_idle_add_full(G_PRIORITY_DEFAULT, start_transaction_preview, session, nullptr);
}

const GDBusInterfaceVTable kManagerVTable = {
  on_manager_method_call,
  nullptr,
  nullptr,
  nullptr,
};
