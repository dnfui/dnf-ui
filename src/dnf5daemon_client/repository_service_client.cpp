// -----------------------------------------------------------------------------
// src/dnf5daemon_client/repository_service_client.cpp
// D-Bus client for dnf5daemon repository configuration
// Lists and changes repository enabled state without exposing D-Bus types to the UI.
// -----------------------------------------------------------------------------
#include "dnf5daemon_client/repository_service_client.hpp"

#include "i18n.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <set>
#include <utility>

namespace {

// -----------------------------------------------------------------------------
// D-Bus names from Fedora dnf5daemon.
// Keep these in sync with the external API assumptions document.
// -----------------------------------------------------------------------------
constexpr const char *kDnfDaemonName = "org.rpm.dnf.v0";
constexpr const char *kDnfDaemonManagerPath = "/org/rpm/dnf/v0";
constexpr const char *kDnfDaemonSessionManagerInterface = "org.rpm.dnf.v0.SessionManager";
constexpr const char *kDnfDaemonRpmRepoInterface = "org.rpm.dnf.v0.rpm.Repo";
constexpr int kRepositoryWriteTimeoutMs = 3 * 60 * 1000;

// -----------------------------------------------------------------------------
// Return session options for repository configuration work.
// Repository state is configuration data and does not need package metadata.
// -----------------------------------------------------------------------------
GVariant *
repository_session_options()
{
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "load_system_repo", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&options, "{sv}", "load_available_repos", g_variant_new_boolean(FALSE));
  return g_variant_new("a{sv}", &options);
}

// -----------------------------------------------------------------------------
// Return options for repository list calls.
// -----------------------------------------------------------------------------
GVariant *
repository_list_options()
{
  const char *attrs[] = { "id", "name", "enabled", nullptr };

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "repo_attrs", g_variant_new_strv(attrs, -1));
  g_variant_builder_add(&options, "{sv}", "enable_disable", g_variant_new_string("all"));
  g_variant_builder_add(&options, "{sv}", "interactive", g_variant_new_boolean(FALSE));
  return g_variant_new("a{sv}", &options);
}

// -----------------------------------------------------------------------------
// Return options for repository write calls.
// -----------------------------------------------------------------------------
GVariant *
repository_write_options()
{
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "interactive", g_variant_new_boolean(TRUE));
  return g_variant_new("a{sv}", &options);
}

// -----------------------------------------------------------------------------
// Return method parameters for repository ID lists.
// -----------------------------------------------------------------------------
GVariant *
repository_id_parameters(const std::vector<std::string> &repo_ids)
{
  GVariantBuilder repo_ids_builder;
  g_variant_builder_init(&repo_ids_builder, G_VARIANT_TYPE("as"));
  for (const auto &repo_id : repo_ids) {
    g_variant_builder_add(&repo_ids_builder, "s", repo_id.c_str());
  }

  return g_variant_new("(as@a{sv})", &repo_ids_builder, repository_write_options());
}

// -----------------------------------------------------------------------------
// Return true when D-Bus reports that dnf5daemon is missing or cannot start.
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
// -----------------------------------------------------------------------------
bool
daemon_is_access_denied_error(GError *error)
{
  return error && g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED);
}

// -----------------------------------------------------------------------------
// Connect to the system D-Bus used by dnf5daemon.
// -----------------------------------------------------------------------------
GDBusConnection *
repository_service_client_connect(std::string &error_out)
{
  error_out.clear();

  GError *error = nullptr;
  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (!connection) {
    error_out = error ? error->message : _("Could not connect to the system D-Bus.");
    g_clear_error(&error);
    return nullptr;
  }

  return connection;
}

// -----------------------------------------------------------------------------
// Open one dnf5daemon session and return its object path.
// -----------------------------------------------------------------------------
bool
open_repository_session(GDBusConnection *connection,
                        GCancellable *cancellable,
                        std::string &session_path_out,
                        std::string &error_out)
{
  session_path_out.clear();
  error_out.clear();

  if (!connection) {
    error_out = _("dnf5daemon connection is not available.");
    return false;
  }

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                kDnfDaemonManagerPath,
                                                kDnfDaemonSessionManagerInterface,
                                                "open_session",
                                                g_variant_new("(@a{sv})", repository_session_options()),
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
    g_clear_error(&error);
    return false;
  }

  const gchar *path = nullptr;
  g_variant_get(reply, "(&o)", &path);
  session_path_out = path ? path : "";
  g_variant_unref(reply);

  if (session_path_out.empty()) {
    error_out = _("dnf5daemon returned an empty session path.");
    return false;
  }

  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    error_out = _("Operation cancelled.");
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Close one dnf5daemon session.
// -----------------------------------------------------------------------------
bool
close_repository_session(GDBusConnection *connection, const std::string &session_path, std::string &error_out)
{
  error_out.clear();

  if (!connection || session_path.empty()) {
    return true;
  }

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                kDnfDaemonManagerPath,
                                                kDnfDaemonSessionManagerInterface,
                                                "close_session",
                                                g_variant_new("(o)", session_path.c_str()),
                                                G_VARIANT_TYPE("(b)"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Failed to close the dnf5daemon session.");
    g_clear_error(&error);
    return false;
  }

  gboolean success = FALSE;
  g_variant_get(reply, "(b)", &success);
  g_variant_unref(reply);
  return success;
}

struct RepositoryDaemonSession {
  GDBusConnection *connection = nullptr;
  std::string session_path;

  ~RepositoryDaemonSession()
  {
    if (connection) {
      std::string close_error;
      close_repository_session(connection, session_path, close_error);
      g_object_unref(connection);
    }
  }

  RepositoryDaemonSession(const RepositoryDaemonSession &) = delete;
  RepositoryDaemonSession &operator=(const RepositoryDaemonSession &) = delete;

  RepositoryDaemonSession() = default;

  bool open(GCancellable *cancellable, std::string &error_out)
  {
    connection = repository_service_client_connect(error_out);
    if (!connection) {
      return false;
    }

    if (!open_repository_session(connection, cancellable, session_path, error_out)) {
      if (!session_path.empty()) {
        std::string close_error;
        close_repository_session(connection, session_path, close_error);
        session_path.clear();
      }
      return false;
    }

    return true;
  }
};

// -----------------------------------------------------------------------------
// Read one required string field from a repository map.
// -----------------------------------------------------------------------------
bool
map_lookup_required_string(GVariant *map, const char *key, std::string &value_out)
{
  const gchar *value = nullptr;
  if (!map || !g_variant_lookup(map, key, "&s", &value) || !value || !*value) {
    value_out.clear();
    return false;
  }

  value_out = value;
  return true;
}

// -----------------------------------------------------------------------------
// Read one optional string field from a repository map.
// -----------------------------------------------------------------------------
std::string
map_lookup_optional_string(GVariant *map, const char *key)
{
  const gchar *value = nullptr;
  if (map && g_variant_lookup(map, key, "&s", &value) && value) {
    return value;
  }
  return "";
}

// -----------------------------------------------------------------------------
// Read one required boolean field from a repository map.
// -----------------------------------------------------------------------------
bool
map_lookup_required_bool(GVariant *map, const char *key, bool &value_out)
{
  if (!map) {
    return false;
  }

  GVariant *value = g_variant_lookup_value(map, key, G_VARIANT_TYPE_BOOLEAN);
  if (!value) {
    return false;
  }

  value_out = g_variant_get_boolean(value);
  g_variant_unref(value);
  return true;
}

// -----------------------------------------------------------------------------
// Build one repository entry from a daemon object map.
// -----------------------------------------------------------------------------
bool
repository_from_daemon_object(GVariant *object, RepositoryInfo &repository_out, std::string &error_out)
{
  RepositoryInfo repository;
  if (!map_lookup_required_string(object, "id", repository.id)) {
    error_out = _("dnf5daemon returned a repository without an ID.");
    return false;
  }

  if (!map_lookup_required_bool(object, "enabled", repository.enabled)) {
    error_out = _("dnf5daemon returned a repository without a valid enabled state.");
    return false;
  }

  repository.name = map_lookup_optional_string(object, "name");
  if (repository.name.empty()) {
    repository.name = repository.id;
  }

  repository_out = std::move(repository);
  return true;
}

// -----------------------------------------------------------------------------
// Parse the complete repository list returned by dnf5daemon.
// -----------------------------------------------------------------------------
bool
parse_repository_list(GVariant *repositories, std::vector<RepositoryInfo> &repositories_out, std::string &error_out)
{
  repositories_out.clear();
  error_out.clear();

  if (!repositories || !g_variant_is_of_type(repositories, G_VARIANT_TYPE("aa{sv}"))) {
    error_out = _("dnf5daemon returned an invalid repository list.");
    return false;
  }

  std::set<std::string> seen_ids;
  const gsize count = g_variant_n_children(repositories);
  for (gsize i = 0; i < count; ++i) {
    GVariant *object = g_variant_get_child_value(repositories, i);

    RepositoryInfo repository;
    const bool ok = repository_from_daemon_object(object, repository, error_out);
    g_variant_unref(object);
    if (!ok) {
      repositories_out.clear();
      return false;
    }

    if (!seen_ids.insert(repository.id).second) {
      error_out = _("dnf5daemon returned duplicate repository IDs.");
      repositories_out.clear();
      return false;
    }

    repositories_out.push_back(std::move(repository));
  }

  std::sort(repositories_out.begin(),
            repositories_out.end(),
            [](const RepositoryInfo &left, const RepositoryInfo &right) { return left.id < right.id; });
  return true;
}

// -----------------------------------------------------------------------------
// Call one dnf5daemon repository write method.
// -----------------------------------------------------------------------------
bool
call_repository_write_method(GDBusConnection *connection,
                             const std::string &session_path,
                             const char *method,
                             const std::vector<std::string> &repo_ids,
                             std::string &error_out)
{
  error_out.clear();

  if (repo_ids.empty()) {
    return true;
  }

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kDnfDaemonName,
                                                session_path.c_str(),
                                                kDnfDaemonRpmRepoInterface,
                                                method,
                                                repository_id_parameters(repo_ids),
                                                nullptr,
                                                G_DBUS_CALL_FLAGS_NONE,
                                                kRepositoryWriteTimeoutMs,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("dnf5daemon repository change failed.");
    g_clear_error(&error);
    return false;
  }

  g_variant_unref(reply);
  return true;
}

} // namespace

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Test-only hook for daemon repository-list parser coverage.
// -----------------------------------------------------------------------------
bool
repository_service_client_testonly_parse_repository_list(GVariant *repositories,
                                                         std::vector<RepositoryInfo> &repositories_out,
                                                         std::string &error_out)
{
  return parse_repository_list(repositories, repositories_out, error_out);
}
#endif

// -----------------------------------------------------------------------------
// List configured repositories through dnf5daemon.
// -----------------------------------------------------------------------------
bool
repository_service_client_list(std::vector<RepositoryInfo> &repositories_out,
                               std::string &error_out,
                               GCancellable *cancellable)
{
  repositories_out.clear();
  error_out.clear();

  RepositoryDaemonSession session;
  if (!session.open(cancellable, error_out)) {
    return false;
  }

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(session.connection,
                                                kDnfDaemonName,
                                                session.session_path.c_str(),
                                                kDnfDaemonRpmRepoInterface,
                                                "list",
                                                g_variant_new("(@a{sv})", repository_list_options()),
                                                G_VARIANT_TYPE("(aa{sv})"),
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                cancellable,
                                                &error);
  if (!reply) {
    error_out = error ? error->message : _("Could not list repositories from dnf5daemon.");
    g_clear_error(&error);
    return false;
  }

  GVariant *repositories = g_variant_get_child_value(reply, 0);
  const bool ok = parse_repository_list(repositories, repositories_out, error_out);
  g_variant_unref(repositories);
  g_variant_unref(reply);
  return ok;
}

// -----------------------------------------------------------------------------
// Apply repository enable and disable requests through dnf5daemon.
// -----------------------------------------------------------------------------
RepositoryWriteResult
repository_service_client_apply_changes(const std::vector<std::string> &enable_ids,
                                        const std::vector<std::string> &disable_ids)
{
  RepositoryWriteResult result;

  RepositoryDaemonSession session;
  if (!session.open(nullptr, result.error)) {
    result.enable_succeeded = false;
    result.disable_succeeded = false;
    return result;
  }

  if (!enable_ids.empty()) {
    result.enable_attempted = true;
    result.enable_succeeded = call_repository_write_method(
        session.connection, session.session_path, "enable_with_options", enable_ids, result.error);
  }

  if (result.enable_succeeded && !disable_ids.empty()) {
    result.disable_attempted = true;
    result.disable_succeeded = call_repository_write_method(
        session.connection, session.session_path, "disable_with_options", disable_ids, result.error);
  }

  return result;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
