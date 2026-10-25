// -----------------------------------------------------------------------------
// transaction_service_client_offline.cpp
// DNF offline status, preflight checks, and cleanup through dnf5daemon
// -----------------------------------------------------------------------------
#include "transaction_service_client_offline.hpp"
#include "transaction_service_client_internal.hpp"

#include "i18n.hpp"

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

  // Empty daemon status can also mean unreadable stored data. Always request cleanup.
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                session_path.c_str(),
                                                kDnfDaemonOfflineInterface,
                                                "clean_with_options",
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
