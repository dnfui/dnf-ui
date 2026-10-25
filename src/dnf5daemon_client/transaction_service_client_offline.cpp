// -----------------------------------------------------------------------------
// transaction_service_client_offline.cpp
// DNF offline status, preflight checks, and cleanup through dnf5daemon
// -----------------------------------------------------------------------------
#include "transaction_service_client_offline.hpp"
#include "transaction_service_client_internal.hpp"

#include "i18n.hpp"

#include <cstring>

namespace {

constexpr const char *kDnfDaemonName = "org.rpm.dnf.v0";
constexpr const char *kDnfDaemonOfflineInterface = "org.rpm.dnf.v0.Offline";

// -----------------------------------------------------------------------------
// Return options for daemon calls that may need user authentication.
// -----------------------------------------------------------------------------
GVariant *
offline_interactive_options()
{
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "interactive", g_variant_new_boolean(TRUE));
  return g_variant_new("a{sv}", &options);
}

// -----------------------------------------------------------------------------
// Check the input and output arguments of one daemon cleanup method.
// -----------------------------------------------------------------------------
bool
offline_cleanup_method_has_options(GDBusMethodInfo *method)
{
  if (!method || !method->in_args || !method->out_args) {
    return false;
  }
  if (!method->in_args[0] || method->in_args[1] || !method->out_args[0] || !method->out_args[1] ||
      method->out_args[2]) {
    return false;
  }

  return std::strcmp(method->in_args[0]->signature, "a{sv}") == 0 &&
      std::strcmp(method->out_args[0]->signature, "b") == 0 && std::strcmp(method->out_args[1]->signature, "s") == 0;
}

// -----------------------------------------------------------------------------
// Select the cleanup method with an options dictionary on this daemon session.
// -----------------------------------------------------------------------------
const char *
offline_cleanup_method_name(GDBusConnection *connection,
                            const std::string &session_path,
                            std::string &error_out,
                            GCancellable *cancellable)
{
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                session_path.c_str(),
                                                "org.freedesktop.DBus.Introspectable",
                                                "Introspect",
                                                nullptr,
                                                G_VARIANT_TYPE("(s)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                cancellable,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Could not inspect dnf5daemon offline cleanup methods.");
    g_clear_error(&error);
    return nullptr;
  }

  const gchar *xml = nullptr;
  g_variant_get(reply, "(&s)", &xml);
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, &error);
  g_variant_unref(reply);
  if (!node) {
    error_out = error ? error->message : _("Could not inspect dnf5daemon offline cleanup methods.");
    g_clear_error(&error);
    return nullptr;
  }

  GDBusInterfaceInfo *offline_interface = g_dbus_node_info_lookup_interface(node, kDnfDaemonOfflineInterface);
  const char *method_name = nullptr;
  if (offline_interface &&
      offline_cleanup_method_has_options(
          g_dbus_interface_info_lookup_method(offline_interface, "clean_with_options"))) {
    method_name = "clean_with_options";
  } else if (offline_interface &&
             offline_cleanup_method_has_options(g_dbus_interface_info_lookup_method(offline_interface, "clean"))) {
    method_name = "clean";
  }

  g_dbus_node_info_unref(node);
  if (!method_name) {
    error_out = _("dnf5daemon does not expose a supported offline cleanup method.");
  }
  return method_name;
}

} // namespace

namespace dnfui_offline {

// -----------------------------------------------------------------------------
// Read daemon state and the systemd boot trigger before preparing more changes.
// The daemon pending flag alone omits incomplete, invalid, and unscheduled data.
// -----------------------------------------------------------------------------
bool
get_offline_status(GDBusConnection *connection,
                   const std::string &session_path,
                   OfflineTransactionStatus &status_out,
                   std::string &error_out,
                   GCancellable *cancellable)
{
  status_out = {};
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                session_path.c_str(),
                                                kDnfDaemonOfflineInterface,
                                                "get_status",
                                                nullptr,
                                                G_VARIANT_TYPE("(ba{sv})"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                cancellable,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Could not check updates prepared for reboot.");
    g_clear_error(&error);
    return false;
  }

  gboolean scheduled = FALSE;
  GVariant *fields = nullptr;
  g_variant_get(reply, "(b@a{sv})", &scheduled, &fields);
  const gchar *state = nullptr;
  bool valid =
      g_variant_n_children(fields) == 0 || (g_variant_lookup(fields, "status", "&s", &state) && state && *state);
  if (valid) {
    status_out.scheduled = scheduled;
    status_out.state = state ? state : "";
    // systemd also accepts a boot trigger from another update tool.
    status_out.boot_trigger_present =
        g_file_test("/system-update", static_cast<GFileTest>(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK)) ||
        g_file_test("/etc/system-update", static_cast<GFileTest>(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK));
  } else {
    error_out = _("dnf5daemon returned an unsupported offline transaction status.");
  }
  g_variant_unref(fields);
  g_variant_unref(reply);
  return valid;
}

bool
verify_no_offline_transaction(GDBusConnection *connection,
                              const std::string &session_path,
                              std::string &error_out,
                              GCancellable *cancellable)
{
  OfflineTransactionStatus status;
  if (!get_offline_status(connection, session_path, status, error_out, cancellable)) {
    return false;
  }
  if (status.has_transaction()) {
    if (status.scheduled && status.state == "ready") {
      error_out = _("Updates are already prepared for reboot. Restart to install them, or use "
                    "Package > Discard Prepared Updates to remove them before applying other package changes.");
    } else {
      error_out =
          _("DNF has stored offline update data. Use Package > Discard Prepared Updates before applying more changes. "
            "For manual recovery, use 'dnf5 offline status' and 'dnf5 offline clean'.");
    }
    return false;
  }
  return true;
}

} // namespace dnfui_offline

// -----------------------------------------------------------------------------
// Report only the normal ready-and-scheduled state used by the main-window cue.
// Other stored states still block transactions, but are recovery cases rather
// than a successfully prepared update.
// -----------------------------------------------------------------------------
bool
transaction_service_client_has_prepared_offline_updates_request(GDBusConnection *connection,
                                                                const std::string &session_path,
                                                                bool &prepared_out,
                                                                std::string &error_out,
                                                                GCancellable *cancellable)
{
  prepared_out = false;
  error_out.clear();

  dnfui_offline::OfflineTransactionStatus status;
  if (!dnfui_offline::get_offline_status(connection, session_path, status, error_out, cancellable)) {
    return false;
  }

  prepared_out = status.scheduled && status.state == "ready";
  return true;
}

// -----------------------------------------------------------------------------
// Clean stored changes through DNF and verify that new transactions are unblocked.
// -----------------------------------------------------------------------------
bool
transaction_service_client_discard_offline_request(GDBusConnection *connection,
                                                   const std::string &session_path,
                                                   std::string &error_out,
                                                   GCancellable *cancellable)
{
  error_out.clear();

  const char *method_name = offline_cleanup_method_name(connection, session_path, error_out, cancellable);
  if (!method_name) {
    return false;
  }

  // Empty daemon status can also mean unreadable stored data. Always request cleanup.
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                session_path.c_str(),
                                                kDnfDaemonOfflineInterface,
                                                method_name,
                                                g_variant_new("(@a{sv})", offline_interactive_options()),
                                                G_VARIANT_TYPE("(bs)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                G_MAXINT,
                                                cancellable,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Could not discard the prepared updates.");
    g_clear_error(&error);
    return false;
  }

  gboolean success = FALSE;
  const gchar *message = nullptr;
  g_variant_get(reply, "(b&s)", &success, &message);
  if (!success) {
    error_out = message && *message ? message : _("Could not discard the prepared updates.");
  }
  g_variant_unref(reply);
  if (!success) {
    return false;
  }

  // A successful method reply alone does not prove that the stored update was removed.
  dnfui_offline::OfflineTransactionStatus status;
  if (!dnfui_offline::get_offline_status(connection, session_path, status, error_out, cancellable)) {
    return false;
  }
  if (status.has_transaction()) {
    if (status.boot_trigger_present && !status.scheduled && status.state.empty()) {
      error_out = _("Another application still has an update scheduled for reboot. Use that application to manage it.");
    } else {
      error_out = _("Prepared update data still exists. Use 'dnf5 offline status' to inspect it and "
                    "'dnf5 offline clean' for manual recovery.");
    }
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
