// -----------------------------------------------------------------------------
// transaction_service_client_dbus.cpp
// Raw D-Bus calls used by the GUI-side transaction client.
// Talks to DNF5 dnf5daemon while keeping the GTK-facing transaction API small.
// -----------------------------------------------------------------------------
#include "transaction_service_client_internal.hpp"

#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "transaction_request.hpp"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// D-Bus names from Fedora dnf5daemon.
// Keep these in sync with the external API assumptions document.
// -----------------------------------------------------------------------------
constexpr const char *kDnfDaemonName = "org.rpm.dnf.v0";
constexpr const char *kDnfDaemonManagerPath = "/org/rpm/dnf/v0";
constexpr const char *kDnfDaemonSessionManagerInterface = "org.rpm.dnf.v0.SessionManager";
constexpr const char *kDnfDaemonBaseInterface = "org.rpm.dnf.v0.Base";
constexpr const char *kDnfDaemonRpmInterface = "org.rpm.dnf.v0.rpm.Rpm";
constexpr const char *kDnfDaemonRpmRepoInterface = "org.rpm.dnf.v0.rpm.Repo";
constexpr const char *kDnfDaemonGoalInterface = "org.rpm.dnf.v0.Goal";

// -----------------------------------------------------------------------------
// DNF UI needs this package for future transaction previews and applies.
// -----------------------------------------------------------------------------
constexpr const char *kRequiredDaemonServerPackage = "dnf5daemon-server";

#ifdef DNFUI_DEBUG_TRACE
static long long
elapsed_ms_since(gint64 started_at_us)
{
  return static_cast<long long>((g_get_monotonic_time() - started_at_us) / 1000);
}
#endif

struct TransactionServiceConnectionCache {
  std::mutex mutex;
  GDBusConnection *connection = nullptr;
  std::set<std::string> allow_erasing_sessions;
};

// -----------------------------------------------------------------------------
// Return the process-local cache that keeps daemon sessions alive.
// dnf5daemon sessions are tied to the D-Bus connection that created them.
// -----------------------------------------------------------------------------
TransactionServiceConnectionCache &
get_transaction_service_connection_cache()
{
  static TransactionServiceConnectionCache cache;
  return cache;
}

// -----------------------------------------------------------------------------
// Return options for daemon calls that may need user authentication.
// -----------------------------------------------------------------------------
GVariant *
interactive_options()
{
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "interactive", g_variant_new_boolean(TRUE));
  return g_variant_new("a{sv}", &options);
}

// -----------------------------------------------------------------------------
// Return an empty options map for dnf5daemon methods.
// -----------------------------------------------------------------------------
GVariant *
empty_options()
{
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  return g_variant_new("a{sv}", &options);
}

// -----------------------------------------------------------------------------
// Return session options for manual repository refresh.
// The refresh code expires metadata itself before loading repos again.
// Do not let open_session load old repository data first.
// -----------------------------------------------------------------------------
GVariant *
refresh_session_options()
{
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "load_available_repos", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&options, "{sv}", "load_system_repo", g_variant_new_boolean(FALSE));
  return g_variant_new("a{sv}", &options);
}

// -----------------------------------------------------------------------------
// Return resolve options for one daemon session.
// Remove requests need allow_erasing so dependency removals match normal DNF behavior.
// -----------------------------------------------------------------------------
GVariant *
resolve_options(const std::string &transaction_path)
{
  bool allow_erasing = false;
  TransactionServiceConnectionCache &cache = get_transaction_service_connection_cache();
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    allow_erasing = cache.allow_erasing_sessions.count(transaction_path) > 0;
  }

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "interactive", g_variant_new_boolean(TRUE));
  if (allow_erasing) {
    g_variant_builder_add(&options, "{sv}", "allow_erasing", g_variant_new_boolean(TRUE));
  }
  return g_variant_new("a{sv}", &options);
}

// -----------------------------------------------------------------------------
// Remember whether a daemon session needs allow_erasing during resolve.
// -----------------------------------------------------------------------------
void
remember_allow_erasing_session(const std::string &transaction_path, bool allow_erasing)
{
  if (transaction_path.empty() || !allow_erasing) {
    return;
  }

  TransactionServiceConnectionCache &cache = get_transaction_service_connection_cache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  cache.allow_erasing_sessions.insert(transaction_path);
}

// -----------------------------------------------------------------------------
// Forget local state for a daemon session after it is closed.
// -----------------------------------------------------------------------------
void
forget_daemon_session(const std::string &transaction_path)
{
  TransactionServiceConnectionCache &cache = get_transaction_service_connection_cache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  cache.allow_erasing_sessions.erase(transaction_path);
}

// -----------------------------------------------------------------------------
// Return dnf5daemon method parameters for package spec methods.
// -----------------------------------------------------------------------------
GVariant *
package_specs_parameters(const std::vector<std::string> &specs)
{
  GVariantBuilder specs_builder;
  GVariantBuilder options;
  g_variant_builder_init(&specs_builder, G_VARIANT_TYPE("as"));
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));

  for (const auto &spec : specs) {
    g_variant_builder_add(&specs_builder, "s", spec.c_str());
  }

  return g_variant_new("(asa{sv})", &specs_builder, &options);
}

// -----------------------------------------------------------------------------
// Return dnf5daemon transaction options for an interactive apply.
// -----------------------------------------------------------------------------
GVariant *
apply_options_parameters()
{
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "interactive", g_variant_new_boolean(TRUE));
  return g_variant_new("(a{sv})", &options);
}

// -----------------------------------------------------------------------------
// Return true when D-Bus reports that dnf5daemon is missing or cannot start.
// Other daemon errors are kept unchanged so useful details are not hidden.
// -----------------------------------------------------------------------------
bool
daemon_is_unavailable_error(GError *error)
{
  if (!error) {
    return false;
  }

  return g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_EXEC_FAILED) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_FAILED) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND);
}

// -----------------------------------------------------------------------------
// Return true when D-Bus rejects access to the daemon service.
// This can happen if the daemon package or its policy is broken.
// -----------------------------------------------------------------------------
bool
daemon_is_access_denied_error(GError *error)
{
  return error && g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED);
}

// -----------------------------------------------------------------------------
// Convert one string to lower case for daemon enum matching.
// -----------------------------------------------------------------------------
std::string
ascii_lower(std::string value)
{
  std::transform(
      value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// -----------------------------------------------------------------------------
// Read one string field from a daemon object map.
// -----------------------------------------------------------------------------
std::string
map_lookup_string(GVariant *map, const char *key)
{
  const gchar *value = nullptr;
  if (map && g_variant_lookup(map, key, "&s", &value)) {
    return value ? value : "";
  }
  return "";
}

// -----------------------------------------------------------------------------
// Read one signed size field from a daemon object map.
// -----------------------------------------------------------------------------
long long
map_lookup_int64(GVariant *map, const char *key)
{
  if (!map) {
    return 0;
  }

  gint64 signed64 = 0;
  if (g_variant_lookup(map, key, "x", &signed64)) {
    return static_cast<long long>(signed64);
  }

  guint64 unsigned64 = 0;
  if (g_variant_lookup(map, key, "t", &unsigned64)) {
    return static_cast<long long>(unsigned64);
  }

  gint32 signed32 = 0;
  if (g_variant_lookup(map, key, "i", &signed32)) {
    return signed32;
  }

  guint32 unsigned32 = 0;
  if (g_variant_lookup(map, key, "u", &unsigned32)) {
    return unsigned32;
  }

  return 0;
}

// -----------------------------------------------------------------------------
// Build the user-facing error shown when dnf5daemon cannot apply a preview.
// The daemon message is kept because it often contains the real failure reason.
// -----------------------------------------------------------------------------
std::string
daemon_apply_error_message(GError *error)
{
  std::string message = _("The approved transaction was not applied. Prepare the preview again before retrying.");

  if (error && error->message && *error->message) {
    message += "\n\n";
    message += _("Details: ");
    message += error->message;
  }

  return message;
}

// -----------------------------------------------------------------------------
// Build the package label used by the existing preview dialog.
// A daemon package item must contain enough fields to identify one package.
// -----------------------------------------------------------------------------
bool
package_label_from_daemon_object(GVariant *object, std::string &label_out, std::string &error_out)
{
  label_out.clear();

  const std::string name = map_lookup_string(object, "name");
  const std::string epoch = map_lookup_string(object, "epoch");
  const std::string version = map_lookup_string(object, "version");
  const std::string release = map_lookup_string(object, "release");
  const std::string arch = map_lookup_string(object, "arch");

  if (name.empty() || version.empty() || release.empty() || arch.empty()) {
    error_out = _("dnf5daemon returned an incomplete package item.");
    return false;
  }

  std::ostringstream label;
  label << name << "-";
  if (!epoch.empty() && epoch != "0") {
    label << epoch << ":";
  }
  label << version << "-" << release << "." << arch;
  label_out = label.str();
  return true;
}

// -----------------------------------------------------------------------------
// Build the package key used when comparing dnf5daemon upgrade candidates with
// UI table rows. The exact NEVRA can differ between daemon list output and UI
// candidate rows, but name and architecture identify the package row.
// -----------------------------------------------------------------------------
bool
package_upgrade_key_from_daemon_object(GVariant *object, std::string &key_out, std::string &error_out)
{
  key_out.clear();

  const std::string name = map_lookup_string(object, "name");
  const std::string arch = map_lookup_string(object, "arch");

  if (name.empty() || arch.empty()) {
    error_out = _("dnf5daemon returned an incomplete package item.");
    return false;
  }

  key_out = name + "." + arch;
  return true;
}

// -----------------------------------------------------------------------------
// Add one daemon transaction item to the preview model.
// Unknown actions fail the whole preview so the dialog never hides work.
// -----------------------------------------------------------------------------
bool
append_daemon_preview_item(TransactionPreview &preview,
                           const std::string &object_type,
                           const std::string &action,
                           GVariant *object,
                           std::string &error_out)
{
  const std::string lower_object_type = ascii_lower(object_type);
  if (lower_object_type != "package") {
    error_out = "Unsupported dnf5daemon transaction item type: " + object_type + ".";
    return false;
  }

  const std::string name = map_lookup_string(object, "name");
  const std::string lower_action = ascii_lower(action);

  // NOTE: Without dnf5daemon-server, DNF UI cannot apply future package changes.
  if ((lower_action == "remove" || lower_action == "replaced") && name == kRequiredDaemonServerPackage) {
    error_out = _("This transaction would remove or replace dnf5daemon-server, "
                  "which DNF UI needs to apply package changes.");
    return false;
  }

  std::string label;
  if (!package_label_from_daemon_object(object, label, error_out)) {
    return false;
  }

  const long long install_size = map_lookup_int64(object, "install_size");

  if (lower_action == "install") {
    preview.install.push_back(label);
    preview.disk_space_delta += install_size;
    return true;
  }
  if (lower_action == "upgrade") {
    preview.upgrade.push_back(label);
    preview.disk_space_delta += install_size;
    return true;
  }
  if (lower_action == "downgrade") {
    preview.downgrade.push_back(label);
    preview.disk_space_delta += install_size;
    return true;
  }
  if (lower_action == "reinstall") {
    preview.reinstall.push_back(label);
    return true;
  }
  if (lower_action == "remove") {
    preview.remove.push_back(label);
    preview.disk_space_delta -= install_size;
    return true;
  }
  if (lower_action == "replaced") {
    preview.replaced.push_back(label);
    preview.disk_space_delta -= install_size;
    return true;
  }

  error_out = "Unsupported dnf5daemon transaction action in preview: " + action + ".";
  return false;
}

// -----------------------------------------------------------------------------
// Ask dnf5daemon for human-readable resolver problems.
// -----------------------------------------------------------------------------
std::string
daemon_transaction_problems(GDBusConnection *connection, const std::string &transaction_path)
{
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                transaction_path.c_str(),
                                                kDnfDaemonGoalInterface,
                                                "get_transaction_problems_string",
                                                nullptr,
                                                G_VARIANT_TYPE("(as)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    std::string message = error ? error->message : "";
    g_clear_error(&error);
    return message;
  }

  gchar **problems = nullptr;
  g_variant_get(reply, "(^as)", &problems);

  std::ostringstream out;
  if (problems) {
    for (gchar **it = problems; *it; ++it) {
      if (out.tellp() > 0) {
        out << "\n";
      }
      out << *it;
    }
  }

  g_strfreev(problems);
  g_variant_unref(reply);
  return out.str();
}

// -----------------------------------------------------------------------------
// Return false when the caller cancelled the repository refresh.
// -----------------------------------------------------------------------------
bool
refresh_cancelled(GCancellable *cancellable, std::string &error_out)
{
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    error_out = _("Repository refresh was cancelled.");
    return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
// dnf5daemon reports a missing cache directory as a clean failure.
// For Refresh Repositories, a missing cache is already clean.
// -----------------------------------------------------------------------------
bool
daemon_clean_missing_cache_error(const std::string &error)
{
  return error.find("Cannot iterate the cache directory") != std::string::npos;
}

// -----------------------------------------------------------------------------
// Call one dnf5daemon Base method that returns success and an error string.
// -----------------------------------------------------------------------------
bool
call_daemon_base_status_method(GDBusConnection *connection,
                               const std::string &transaction_path,
                               const char *method,
                               GVariant *parameters,
                               GCancellable *cancellable,
                               std::string &error_out)
{
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                transaction_path.c_str(),
                                                kDnfDaemonBaseInterface,
                                                method,
                                                parameters,
                                                G_VARIANT_TYPE("(bs)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                G_MAXINT,
                                                cancellable,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("dnf5daemon repository refresh failed.");
    g_clear_error(&error);
    return false;
  }

  gboolean success = FALSE;
  const gchar *message = nullptr;
  g_variant_get(reply, "(b&s)", &success, &message);
  if (!success) {
    error_out = message && *message ? message : _("dnf5daemon repository refresh failed.");
    g_variant_unref(reply);
    return false;
  }

  g_variant_unref(reply);
  return true;
}

// -----------------------------------------------------------------------------
// Clean one daemon cache type, accepting a missing cache directory as empty cache.
// -----------------------------------------------------------------------------
bool
call_daemon_base_clean_method(GDBusConnection *connection,
                              const std::string &transaction_path,
                              const char *cache_type,
                              GCancellable *cancellable,
                              std::string &error_out)
{
  std::string clean_error;
  bool ok = call_daemon_base_status_method(connection,
                                           transaction_path,
                                           "clean_with_options",
                                           g_variant_new("(s@a{sv})", cache_type, interactive_options()),
                                           cancellable,
                                           clean_error);
  if (ok) {
    return true;
  }

  if (daemon_clean_missing_cache_error(clean_error)) {
    DNFUI_TRACE("dnf5daemon cache clean skipped missing cache directory type=%s", cache_type);
    error_out.clear();
    return true;
  }

  error_out = clean_error;
  return false;
}

// -----------------------------------------------------------------------------
// Call one dnf5daemon Base method that returns only success.
// -----------------------------------------------------------------------------
bool
call_daemon_base_success_method(GDBusConnection *connection,
                                const std::string &transaction_path,
                                const char *method,
                                GCancellable *cancellable,
                                std::string &error_out)
{
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                transaction_path.c_str(),
                                                kDnfDaemonBaseInterface,
                                                method,
                                                nullptr,
                                                G_VARIANT_TYPE("(b)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                G_MAXINT,
                                                cancellable,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("dnf5daemon repository refresh failed.");
    g_clear_error(&error);
    return false;
  }

  gboolean success = FALSE;
  g_variant_get(reply, "(b)", &success);
  if (!success) {
    error_out = _("dnf5daemon repository refresh failed.");
    g_variant_unref(reply);
    return false;
  }

  g_variant_unref(reply);
  return true;
}

// -----------------------------------------------------------------------------
// Call one dnf5daemon method that marks package specs for the session.
// -----------------------------------------------------------------------------
bool
mark_package_specs(GDBusConnection *connection,
                   const std::string &transaction_path,
                   const char *method,
                   const std::vector<std::string> &specs,
                   std::string &error_out)
{
  if (specs.empty()) {
    DNFUI_TRACE("dnf5daemon mark skipped method=%s specs=0 path=%s", method, transaction_path.c_str());
    return true;
  }

  DNFUI_TRACE("dnf5daemon mark start method=%s specs=%zu path=%s", method, specs.size(), transaction_path.c_str());
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                transaction_path.c_str(),
                                                kDnfDaemonRpmInterface,
                                                method,
                                                package_specs_parameters(specs),
                                                nullptr,
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Could not mark packages in dnf5daemon.");
    DNFUI_TRACE(
        "dnf5daemon mark failed method=%s path=%s error=%s", method, transaction_path.c_str(), error_out.c_str());
    g_clear_error(&error);
    return false;
  }

  g_variant_unref(reply);
  DNFUI_TRACE("dnf5daemon mark done method=%s specs=%zu path=%s", method, specs.size(), transaction_path.c_str());
  return true;
}

// -----------------------------------------------------------------------------
// Open one dnf5daemon session with explicit options and return its object path.
// -----------------------------------------------------------------------------
bool
open_daemon_session_with_options(GDBusConnection *connection,
                                 GVariant *options,
                                 std::string &transaction_path_out,
                                 std::string &error_out)
{
  transaction_path_out.clear();
  error_out.clear();

#ifdef DNFUI_DEBUG_TRACE
  const gint64 started_at_us = g_get_monotonic_time();
#endif
  DNFUI_TRACE("dnf5daemon session open start");
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                kDnfDaemonManagerPath,
                                                kDnfDaemonSessionManagerInterface,
                                                "open_session",
                                                g_variant_new("(@a{sv})", options),
                                                G_VARIANT_TYPE("(o)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    if (daemon_is_unavailable_error(error)) {
      error_out = _("dnf5daemon is not available. Make sure dnf5daemon-server is installed and running.");
    } else if (daemon_is_access_denied_error(error)) {
      error_out = _("dnf5daemon is installed, but DNF UI is not allowed to talk to it. "
                    "Reinstall dnf5daemon-server or check the D-Bus policy.");
    } else {
      error_out = error ? error->message : _("Could not open a dnf5daemon session.");
    }
    DNFUI_TRACE("dnf5daemon session open failed error=%s", error_out.c_str());
    g_clear_error(&error);
    return false;
  }

  const gchar *path = nullptr;
  g_variant_get(reply, "(&o)", &path);
  transaction_path_out = path ? path : "";
  g_variant_unref(reply);

  if (transaction_path_out.empty()) {
    error_out = _("dnf5daemon returned an empty session path.");
    DNFUI_TRACE("dnf5daemon session open failed error=%s", error_out.c_str());
    return false;
  }

  DNFUI_TRACE("dnf5daemon session opened path=%s elapsed_ms=%lld",
              transaction_path_out.c_str(),
              elapsed_ms_since(started_at_us));
  return true;
}

// -----------------------------------------------------------------------------
// Open one normal dnf5daemon session and return its object path.
// -----------------------------------------------------------------------------
bool
open_daemon_session(GDBusConnection *connection, std::string &transaction_path_out, std::string &error_out)
{
  return open_daemon_session_with_options(connection, empty_options(), transaction_path_out, error_out);
}

} // namespace

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Test-only hook for daemon preview parser coverage.
// -----------------------------------------------------------------------------
bool
transaction_service_client_testonly_build_preview_from_item(const std::string &object_type,
                                                            const std::string &action,
                                                            const std::string &name,
                                                            TransactionPreview &preview,
                                                            std::string &error_out)
{
  TransactionPreview built_preview;

  GVariantBuilder object_builder;
  g_variant_builder_init(&object_builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&object_builder, "{sv}", "name", g_variant_new_string(name.c_str()));
  g_variant_builder_add(&object_builder, "{sv}", "epoch", g_variant_new_string("0"));
  g_variant_builder_add(&object_builder, "{sv}", "version", g_variant_new_string("2.0"));
  g_variant_builder_add(&object_builder, "{sv}", "release", g_variant_new_string("3"));
  g_variant_builder_add(&object_builder, "{sv}", "arch", g_variant_new_string("x86_64"));
  g_variant_builder_add(&object_builder, "{sv}", "install_size", g_variant_new_int64(4096));

  GVariant *object = g_variant_ref_sink(g_variant_builder_end(&object_builder));
  bool ok = append_daemon_preview_item(built_preview, object_type, action, object, error_out);
  g_variant_unref(object);

  if (!ok) {
    return false;
  }

  preview = std::move(built_preview);
  return true;
}
#endif

// -----------------------------------------------------------------------------
// Connect to the system D-Bus used by dnf5daemon.
// -----------------------------------------------------------------------------
GDBusConnection *
transaction_service_client_connect(std::string &error_out)
{
  error_out.clear();
  TransactionServiceConnectionCache &cache = get_transaction_service_connection_cache();

  std::lock_guard<std::mutex> lock(cache.mutex);
  if (cache.connection && !g_dbus_connection_is_closed(cache.connection)) {
    DNFUI_TRACE("dnf5daemon connection reuse");
    return G_DBUS_CONNECTION(g_object_ref(cache.connection));
  }

  if (cache.connection) {
    DNFUI_TRACE("dnf5daemon connection was closed, reconnecting");
    g_object_unref(cache.connection);
    cache.connection = nullptr;
    // Remembered daemon session options belong to the connection that created them.
    cache.allow_erasing_sessions.clear();
  }

  DNFUI_TRACE("dnf5daemon connection open start");
  GError *error = nullptr;
  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (!connection) {
    error_out = error ? error->message : _("Could not connect to the system D-Bus.");
    DNFUI_TRACE("dnf5daemon connection open failed error=%s", error_out.c_str());
    g_clear_error(&error);
    return nullptr;
  }

  cache.connection = G_DBUS_CONNECTION(g_object_ref(connection));
  DNFUI_TRACE("dnf5daemon connection open done");

  return connection;
}

// -----------------------------------------------------------------------------
// Start a dnf5daemon transaction session and mark selected package actions.
// -----------------------------------------------------------------------------
bool
transaction_service_client_start_transaction_request(GDBusConnection *connection,
                                                     const TransactionRequest &request,
                                                     std::string &transaction_path_out,
                                                     std::string &error_out)
{
  transaction_path_out.clear();
  error_out.clear();
  DNFUI_TRACE("dnf5daemon selected transaction start install=%zu upgrade=%zu remove=%zu reinstall=%zu",
              request.install.size(),
              request.upgrade.size(),
              request.remove.size(),
              request.reinstall.size());

  if (!connection) {
    error_out = _("dnf5daemon connection is not available.");
    return false;
  }

  if (!open_daemon_session(connection, transaction_path_out, error_out)) {
    DNFUI_TRACE("dnf5daemon selected transaction failed before mark error=%s", error_out.c_str());
    return false;
  }

  if (!mark_package_specs(connection, transaction_path_out, "install", request.install, error_out) ||
      !mark_package_specs(connection, transaction_path_out, "upgrade", request.upgrade, error_out) ||
      !mark_package_specs(connection, transaction_path_out, "remove", request.remove, error_out) ||
      !mark_package_specs(connection, transaction_path_out, "reinstall", request.reinstall, error_out)) {
    DNFUI_TRACE("dnf5daemon selected transaction mark failed path=%s error=%s",
                transaction_path_out.c_str(),
                error_out.c_str());
    std::string release_error;
    transaction_service_client_release_transaction_request(connection, transaction_path_out, release_error);
    transaction_path_out.clear();
    return false;
  }

  remember_allow_erasing_session(transaction_path_out, !request.remove.empty());
  DNFUI_TRACE("dnf5daemon selected transaction ready path=%s", transaction_path_out.c_str());
  return true;
}

// -----------------------------------------------------------------------------
// Start a dnf5daemon upgrade-all session.
// -----------------------------------------------------------------------------
bool
transaction_service_client_start_upgrade_all_transaction_request(GDBusConnection *connection,
                                                                 std::string &transaction_path_out,
                                                                 std::string &error_out)
{
  transaction_path_out.clear();
  error_out.clear();
#ifdef DNFUI_DEBUG_TRACE
  const gint64 started_at_us = g_get_monotonic_time();
#endif
  DNFUI_TRACE("dnf5daemon upgrade-all transaction start");

  if (!connection) {
    error_out = _("dnf5daemon connection is not available.");
    return false;
  }

  if (!open_daemon_session(connection, transaction_path_out, error_out)) {
    DNFUI_TRACE("dnf5daemon upgrade-all transaction failed before mark error=%s", error_out.c_str());
    return false;
  }

  // dnf5daemon treats an empty upgrade list as native Upgrade All.
  std::vector<std::string> upgrade_specs;
  DNFUI_TRACE("dnf5daemon upgrade-all mark start path=%s", transaction_path_out.c_str());
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                transaction_path_out.c_str(),
                                                kDnfDaemonRpmInterface,
                                                "upgrade",
                                                package_specs_parameters(upgrade_specs),
                                                nullptr,
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Could not mark upgrade-all in dnf5daemon.");
    DNFUI_TRACE("dnf5daemon upgrade-all mark failed path=%s error=%s", transaction_path_out.c_str(), error_out.c_str());
    g_clear_error(&error);
    std::string release_error;
    transaction_service_client_release_transaction_request(connection, transaction_path_out, release_error);
    transaction_path_out.clear();
    return false;
  }

  g_variant_unref(reply);
  DNFUI_TRACE("dnf5daemon upgrade-all transaction ready path=%s elapsed_ms=%lld",
              transaction_path_out.c_str(),
              elapsed_ms_since(started_at_us));
  return true;
}

// -----------------------------------------------------------------------------
// Refresh dnf5daemon repository metadata for the manual Refresh Repositories action.
// -----------------------------------------------------------------------------
bool
transaction_service_client_refresh_repositories(std::string &error_out, GCancellable *cancellable)
{
  error_out.clear();

  std::string connect_error;
  GDBusConnection *connection = transaction_service_client_connect(connect_error);
  if (!connection) {
    error_out = connect_error;
    return false;
  }

  std::string transaction_path;
  if (!open_daemon_session_with_options(connection, refresh_session_options(), transaction_path, error_out)) {
    g_object_unref(connection);
    return false;
  }

  bool ok = true;
  if (refresh_cancelled(cancellable, error_out)) {
    ok = false;
  }
  if (ok) {
    DNFUI_TRACE("dnf5daemon repository refresh expire cache start path=%s", transaction_path.c_str());
    ok = call_daemon_base_clean_method(connection, transaction_path, "expire-cache", cancellable, error_out);
  }
  if (ok && refresh_cancelled(cancellable, error_out)) {
    ok = false;
  }
  if (ok) {
    DNFUI_TRACE("dnf5daemon repository refresh reset start path=%s", transaction_path.c_str());
    ok = call_daemon_base_status_method(connection, transaction_path, "reset", nullptr, cancellable, error_out);
  }
  if (ok && refresh_cancelled(cancellable, error_out)) {
    ok = false;
  }
  if (ok) {
    DNFUI_TRACE("dnf5daemon repository refresh read repos start path=%s", transaction_path.c_str());
    ok = call_daemon_base_success_method(connection, transaction_path, "read_all_repos", cancellable, error_out);
  }

  std::string release_error;
  transaction_service_client_release_transaction_request(connection, transaction_path, release_error);
  g_object_unref(connection);

  if (!ok) {
    DNFUI_TRACE("dnf5daemon repository refresh failed error=%s", error_out.c_str());
    return false;
  }

  DNFUI_TRACE("dnf5daemon repository refresh done path=%s", transaction_path.c_str());
  return true;
}

// -----------------------------------------------------------------------------
// Resolve one prepared dnf5daemon session into the existing preview model.
// -----------------------------------------------------------------------------
bool
transaction_service_client_get_transaction_preview(GDBusConnection *connection,
                                                   const std::string &transaction_path,
                                                   TransactionServiceProgressForwarder *progress_forwarder,
                                                   GCancellable *cancellable,
                                                   TransactionPreview &preview_out,
                                                   std::string &error_out,
                                                   std::vector<std::string> *upgrade_keys_out)
{
  preview_out = {};
  error_out.clear();
  if (upgrade_keys_out) {
    upgrade_keys_out->clear();
  }

  if (!connection || transaction_path.empty()) {
    error_out = _("dnf5daemon transaction session is not available.");
    return false;
  }

  struct PreviewWaitState {
    bool done = false;
    GVariant *reply = nullptr;
    GError *error = nullptr;
  } state;

  GMainContext *context = g_main_context_get_thread_default();
  if (!context) {
    context = g_main_context_default();
  }

#ifdef DNFUI_DEBUG_TRACE
  const gint64 started_at_us = g_get_monotonic_time();
#endif
  DNFUI_TRACE("dnf5daemon resolve start path=%s", transaction_path.c_str());
  GCancellable *call_cancellable = g_cancellable_new();
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    error_out = _("Transaction preview was cancelled.");
    g_object_unref(call_cancellable);
    return false;
  }

  gulong cancel_handler_id = 0;
  if (cancellable) {
    // Stop for List Upgradable should cancel the daemon resolve call too.
    cancel_handler_id = g_cancellable_connect(
        cancellable,
        G_CALLBACK(+[](GCancellable *, gpointer user_data) { g_cancellable_cancel(G_CANCELLABLE(user_data)); }),
        g_object_ref(call_cancellable),
        [](gpointer data) { g_object_unref(data); });
  }

  g_dbus_connection_call(
      connection,
      kDnfDaemonName,
      transaction_path.c_str(),
      kDnfDaemonGoalInterface,
      "resolve",
      g_variant_new("(@a{sv})", resolve_options(transaction_path)),
      G_VARIANT_TYPE("(a(sssa{sv}a{sv})u)"),
      G_DBUS_CALL_FLAGS_NONE,
      G_MAXINT,
      call_cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer user_data) {
        auto *wait_state = static_cast<PreviewWaitState *>(user_data);
        wait_state->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &wait_state->error);
        wait_state->done = true;
      },
      &state);

  bool cancel_requested = false;
  while (!state.done) {
    g_main_context_iteration(context, TRUE);
    if (!cancel_requested && progress_forwarder && !progress_forwarder->key_confirm_error.empty()) {
      // The key answer failed, so stop waiting on resolve and report that error.
      g_cancellable_cancel(call_cancellable);
      cancel_requested = true;
    }
  }
  if (cancellable && cancel_handler_id != 0) {
    g_cancellable_disconnect(cancellable, cancel_handler_id);
  }
  g_object_unref(call_cancellable);

  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    error_out = _("Transaction preview was cancelled.");
    if (state.error) {
      g_clear_error(&state.error);
    }
    if (state.reply) {
      g_variant_unref(state.reply);
    }
    return false;
  }

  if (progress_forwarder && !progress_forwarder->key_confirm_error.empty()) {
    error_out = progress_forwarder->key_confirm_error;
    if (state.error) {
      g_clear_error(&state.error);
    }
    if (state.reply) {
      g_variant_unref(state.reply);
    }
    return false;
  }

  if (!state.reply) {
    error_out = state.error ? state.error->message : _("dnf5daemon failed to resolve the transaction.");
    DNFUI_TRACE("dnf5daemon resolve call failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
    g_clear_error(&state.error);
    return false;
  }

  GVariant *items = nullptr;
  guint32 result = 0;
  g_variant_get(state.reply, "(@a(sssa{sv}a{sv})u)", &items, &result);
  DNFUI_TRACE("dnf5daemon resolve returned path=%s result=%u items=%zu elapsed_ms=%lld",
              transaction_path.c_str(),
              result,
              items ? static_cast<size_t>(g_variant_n_children(items)) : 0,
              elapsed_ms_since(started_at_us));

  switch (result) {
  case 0:
    break;
  case 1: {
    std::string warning = daemon_transaction_problems(connection, transaction_path);
    if (!warning.empty()) {
      DNFUI_TRACE("dnf5daemon resolve warning path=%s warning=%s", transaction_path.c_str(), warning.c_str());
    }
    break;
  }
  case 2:
    error_out = daemon_transaction_problems(connection, transaction_path);
    if (error_out.empty()) {
      error_out = _("dnf5daemon could not resolve the transaction.");
    }
    DNFUI_TRACE("dnf5daemon resolve failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
    g_variant_unref(items);
    g_variant_unref(state.reply);
    return false;
  default:
    error_out = _("dnf5daemon returned an unsupported transaction resolve result.");
    DNFUI_TRACE("dnf5daemon resolve returned unsupported result path=%s result=%u", transaction_path.c_str(), result);
    g_variant_unref(items);
    g_variant_unref(state.reply);
    return false;
  }

  TransactionPreview built_preview;
  GVariantIter iter;
  g_variant_iter_init(&iter, items);

  GVariant *item = nullptr;
  while ((item = g_variant_iter_next_value(&iter)) != nullptr) {
    const gchar *object_type = nullptr;
    const gchar *action = nullptr;
    const gchar *reason = nullptr;
    GVariant *attributes = nullptr;
    GVariant *object = nullptr;

    g_variant_get(item, "(&s&s&s@a{sv}@a{sv})", &object_type, &action, &reason, &attributes, &object);
    bool ok = append_daemon_preview_item(
        built_preview, object_type ? object_type : "", action ? action : "", object, error_out);
    if (ok && upgrade_keys_out && ascii_lower(object_type ? object_type : "") == "package" &&
        ascii_lower(action ? action : "") == "upgrade") {
      std::string key;
      ok = package_upgrade_key_from_daemon_object(object, key, error_out);
      if (ok) {
        upgrade_keys_out->push_back(std::move(key));
      }
    }

    g_variant_unref(attributes);
    g_variant_unref(object);
    g_variant_unref(item);

    if (!ok) {
      DNFUI_TRACE("dnf5daemon preview item rejected path=%s error=%s", transaction_path.c_str(), error_out.c_str());
      g_variant_unref(items);
      g_variant_unref(state.reply);
      return false;
    }
  }

  preview_out = std::move(built_preview);
  DNFUI_TRACE("dnf5daemon preview built path=%s install=%zu upgrade=%zu downgrade=%zu reinstall=%zu remove=%zu "
              "replaced=%zu total_ms=%lld",
              transaction_path.c_str(),
              preview_out.install.size(),
              preview_out.upgrade.size(),
              preview_out.downgrade.size(),
              preview_out.reinstall.size(),
              preview_out.remove.size(),
              preview_out.replaced.size(),
              elapsed_ms_since(started_at_us));
  g_variant_unref(items);
  g_variant_unref(state.reply);
  return true;
}

// -----------------------------------------------------------------------------
// Apply the resolved dnf5daemon transaction.
// The caller must have subscribed to progress signals before this is called.
// -----------------------------------------------------------------------------
bool
transaction_service_client_start_apply_request(GDBusConnection *connection,
                                               const std::string &transaction_path,
                                               TransactionServiceProgressForwarder *progress_forwarder,
                                               GCancellable *cancellable,
                                               std::string &error_out)
{
  error_out.clear();

  if (!connection || transaction_path.empty()) {
    error_out = _("dnf5daemon transaction session is not available.");
    return false;
  }

  struct ApplyWaitState {
    bool done = false;
    GError *error = nullptr;
  } state;

  GMainContext *context = g_main_context_get_thread_default();
  if (!context) {
    context = g_main_context_default();
  }

  GCancellable *call_cancellable = g_cancellable_new();
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    error_out = _("Transaction apply was cancelled.");
    g_object_unref(call_cancellable);
    return false;
  }

  gulong cancel_handler_id = 0;
  if (cancellable) {
    cancel_handler_id = g_cancellable_connect(
        cancellable,
        G_CALLBACK(+[](GCancellable *, gpointer user_data) { g_cancellable_cancel(G_CANCELLABLE(user_data)); }),
        g_object_ref(call_cancellable),
        [](gpointer data) { g_object_unref(data); });
  }
  g_dbus_connection_call(
      connection,
      kDnfDaemonName,
      transaction_path.c_str(),
      kDnfDaemonGoalInterface,
      "do_transaction",
      apply_options_parameters(),
      nullptr,
      G_DBUS_CALL_FLAGS_NONE,
      G_MAXINT,
      call_cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer user_data) {
        auto *wait_state = static_cast<ApplyWaitState *>(user_data);
        GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &wait_state->error);
        if (reply) {
          g_variant_unref(reply);
        }
        wait_state->done = true;
      },
      &state);

  bool cancel_requested = false;
  while (!state.done) {
    g_main_context_iteration(context, TRUE);
    if (!cancel_requested && progress_forwarder && !progress_forwarder->key_confirm_error.empty()) {
      // The key answer failed, so stop waiting on do_transaction and report that error.
      g_cancellable_cancel(call_cancellable);
      cancel_requested = true;
    }
  }
  if (cancellable && cancel_handler_id != 0) {
    g_cancellable_disconnect(cancellable, cancel_handler_id);
  }
  g_object_unref(call_cancellable);

  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    error_out = _("Transaction apply was cancelled.");
    if (state.error) {
      g_clear_error(&state.error);
    }
    return false;
  }

  if (progress_forwarder && !progress_forwarder->key_confirm_error.empty()) {
    error_out = progress_forwarder->key_confirm_error;
    if (state.error) {
      g_clear_error(&state.error);
    }
    return false;
  }

  if (state.error) {
    error_out = daemon_apply_error_message(state.error);
    g_clear_error(&state.error);
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Close a dnf5daemon session that is no longer needed by the GUI.
// -----------------------------------------------------------------------------
bool
transaction_service_client_release_transaction_request(GDBusConnection *connection,
                                                       const std::string &transaction_path,
                                                       std::string &error_out)
{
  error_out.clear();

  if (!connection || transaction_path.empty()) {
    return true;
  }

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                kDnfDaemonManagerPath,
                                                kDnfDaemonSessionManagerInterface,
                                                "close_session",
                                                g_variant_new("(o)", transaction_path.c_str()),
                                                G_VARIANT_TYPE("(b)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Failed to close the dnf5daemon session.");
    g_clear_error(&error);
    forget_daemon_session(transaction_path);
    return false;
  }

  gboolean success = FALSE;
  g_variant_get(reply, "(b)", &success);
  g_variant_unref(reply);
  forget_daemon_session(transaction_path);
  return success;
}

// -----------------------------------------------------------------------------
// Confirm or reject a repository signing key requested by dnf5daemon.
// -----------------------------------------------------------------------------
bool
transaction_service_client_confirm_key(GDBusConnection *connection,
                                       const std::string &transaction_path,
                                       const std::string &key_id,
                                       bool confirmed,
                                       std::string &error_out)
{
  error_out.clear();

  if (!connection || transaction_path.empty() || key_id.empty()) {
    error_out = _("dnf5daemon key import request is incomplete.");
    return false;
  }

  DNFUI_TRACE("dnf5daemon confirm key start path=%s key=%s confirmed=%d",
              transaction_path.c_str(),
              key_id.c_str(),
              confirmed ? 1 : 0);
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(
      connection,
      kDnfDaemonName,
      transaction_path.c_str(),
      kDnfDaemonRpmRepoInterface,
      "confirm_key_with_options",
      g_variant_new("(sb@a{sv})", key_id.c_str(), confirmed ? TRUE : FALSE, interactive_options()),
      nullptr,
      G_DBUS_CALL_FLAGS_NONE,
      G_MAXINT,
      nullptr,
      &error);

  if (!reply) {
    if (error) {
      g_dbus_error_strip_remote_error(error);
      error_out = error->message ? error->message : "";
      if (error_out.empty()) {
        error_out = _("Could not answer the dnf5daemon key import request.");
      }
    } else {
      error_out = _("Could not answer the dnf5daemon key import request.");
    }
    DNFUI_TRACE("dnf5daemon confirm key failed path=%s key=%s error=%s",
                transaction_path.c_str(),
                key_id.c_str(),
                error_out.c_str());
    g_clear_error(&error);
    return false;
  }

  g_variant_unref(reply);
  DNFUI_TRACE("dnf5daemon confirm key done path=%s key=%s", transaction_path.c_str(), key_id.c_str());
  return true;
}

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Close the cached daemon connection between tests.
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
  cache.allow_erasing_sessions.clear();
}

// -----------------------------------------------------------------------------
// Return true when dnf5daemon still exposes one session path.
// Tests use this to verify that release code does not leave old sessions behind.
// -----------------------------------------------------------------------------
bool
transaction_service_client_session_exists_for_tests(const std::string &transaction_path)
{
  if (transaction_path.empty()) {
    return false;
  }

  GDBusConnection *connection = nullptr;
  TransactionServiceConnectionCache &cache = get_transaction_service_connection_cache();
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.connection && !g_dbus_connection_is_closed(cache.connection)) {
      connection = G_DBUS_CONNECTION(g_object_ref(cache.connection));
    }
  }

  if (!connection) {
    return false;
  }

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                transaction_path.c_str(),
                                                "org.freedesktop.DBus.Introspectable",
                                                "Introspect",
                                                nullptr,
                                                G_VARIANT_TYPE("(s)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  g_clear_error(&error);
  g_object_unref(connection);

  if (!reply) {
    return false;
  }

  g_variant_unref(reply);
  return true;
}
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
