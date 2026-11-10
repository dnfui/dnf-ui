// -----------------------------------------------------------------------------
// test_repository_service_client.cpp
// dnf5daemon repository client tests
// Exercises strict repository-list parsing and opt-in daemon repository changes.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf5daemon_client/repository_service_client.hpp"
#include "test_utils.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

struct RepositoryMapSpec {
  const char *id = nullptr;
  const char *name = nullptr;
  bool include_enabled = true;
  bool enabled = false;
  bool enabled_as_string = false;
};

constexpr const char *kDelayedSessionPath = "/org/rpm/dnf/v0/delayed_test";
constexpr const char *kDelayedDaemonXml = R"xml(
<node>
  <interface name="org.rpm.dnf.v0.SessionManager">
    <method name="open_session">
      <arg type="a{sv}" direction="in"/>
      <arg type="o" direction="out"/>
    </method>
    <method name="close_session">
      <arg type="o" direction="in"/>
      <arg type="b" direction="out"/>
    </method>
  </interface>
  <interface name="org.rpm.dnf.v0.rpm.Repo">
    <method name="enable_with_options">
      <arg type="as" direction="in"/>
      <arg type="a{sv}" direction="in"/>
    </method>
  </interface>
</node>)xml";

struct PrivateRepositoryTestBus {
  GSubprocess *process = nullptr;

  ~PrivateRepositoryTestBus()
  {
    if (process) {
      g_subprocess_force_exit(process);
      g_subprocess_wait(process, nullptr, nullptr);
      g_object_unref(process);
    }
  }

  std::string start()
  {
    GError *error = nullptr;
    process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, "dbus-daemon", "--session", "--nofork", "--print-address=1", nullptr);
    REQUIRE(process != nullptr);

    GDataInputStream *output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
    gchar *bus_address = g_data_input_stream_read_line(output, nullptr, nullptr, &error);
    g_object_unref(output);
    REQUIRE(bus_address != nullptr);
    std::string address = bus_address;
    g_free(bus_address);
    return address;
  }
};

struct DelayedRepositoryDaemon {
  PrivateRepositoryTestBus bus;
  GDBusConnection *server = nullptr;
  GDBusNodeInfo *info = nullptr;
  GMainContext *context = nullptr;
  GMainLoop *loop = nullptr;
  guint manager_registration = 0;
  guint repository_registration = 0;
  std::thread worker;
  std::string address;

  DelayedRepositoryDaemon()
  {
    GError *error = nullptr;
    address = bus.start();

    server = connect_to_test_bus(address.c_str(), &error);
    REQUIRE(server != nullptr);
    GVariant *name_reply = g_dbus_connection_call_sync(server,
                                                       "org.freedesktop.DBus",
                                                       "/org/freedesktop/DBus",
                                                       "org.freedesktop.DBus",
                                                       "RequestName",
                                                       g_variant_new("(su)", "org.rpm.dnf.v0", 0u),
                                                       G_VARIANT_TYPE("(u)"),
                                                       G_DBUS_CALL_FLAGS_NONE,
                                                       -1,
                                                       nullptr,
                                                       &error);
    REQUIRE(name_reply != nullptr);
    g_variant_unref(name_reply);

    info = g_dbus_node_info_new_for_xml(kDelayedDaemonXml, &error);
    REQUIRE(info != nullptr);
    context = g_main_context_new();
    loop = g_main_loop_new(context, FALSE);

    static const GDBusInterfaceVTable table = {
      +[](GDBusConnection *,
          const gchar *,
          const gchar *,
          const gchar *,
          const gchar *method,
          GVariant *,
          GDBusMethodInvocation *invocation,
          gpointer) {
        if (g_strcmp0(method, "open_session") == 0) {
          g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", kDelayedSessionPath));
        } else if (g_strcmp0(method, "close_session") == 0) {
          g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
        } else if (g_strcmp0(method, "enable_with_options") == 0) {
          std::this_thread::sleep_for(std::chrono::seconds(27));
          g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
        }
      },
      nullptr,
      nullptr,
      {}
    };

    g_main_context_push_thread_default(context);
    manager_registration = g_dbus_connection_register_object(
        server, "/org/rpm/dnf/v0", info->interfaces[0], &table, nullptr, nullptr, &error);
    repository_registration = g_dbus_connection_register_object(
        server, kDelayedSessionPath, info->interfaces[1], &table, nullptr, nullptr, &error);
    g_main_context_pop_thread_default(context);
    REQUIRE(manager_registration != 0);
    REQUIRE(repository_registration != 0);

    worker = std::thread([this] { g_main_loop_run(loop); });
  }

  ~DelayedRepositoryDaemon()
  {
    g_main_loop_quit(loop);
    worker.join();
    g_dbus_connection_unregister_object(server, manager_registration);
    g_dbus_connection_unregister_object(server, repository_registration);
    g_dbus_connection_close_sync(server, nullptr, nullptr);
    g_object_unref(server);
    g_dbus_node_info_unref(info);
    g_main_loop_unref(loop);
    g_main_context_unref(context);
  }
};

GVariant *
repository_maps_variant(const std::vector<RepositoryMapSpec> &specs)
{
  GVariantBuilder repositories_builder;
  g_variant_builder_init(&repositories_builder, G_VARIANT_TYPE("aa{sv}"));

  for (const auto &spec : specs) {
    GVariantBuilder repository_builder;
    g_variant_builder_init(&repository_builder, G_VARIANT_TYPE("a{sv}"));
    if (spec.id) {
      g_variant_builder_add(&repository_builder, "{sv}", "id", g_variant_new_string(spec.id));
    }
    if (spec.name) {
      g_variant_builder_add(&repository_builder, "{sv}", "name", g_variant_new_string(spec.name));
    }
    if (spec.include_enabled) {
      if (spec.enabled_as_string) {
        g_variant_builder_add(&repository_builder, "{sv}", "enabled", g_variant_new_string("true"));
      } else {
        g_variant_builder_add(&repository_builder, "{sv}", "enabled", g_variant_new_boolean(spec.enabled));
      }
    }
    g_variant_builder_add_value(&repositories_builder, g_variant_builder_end(&repository_builder));
  }

  return g_variant_ref_sink(g_variant_builder_end(&repositories_builder));
}

void
require_repository_client_test_enabled()
{
  const char *enabled = g_getenv("DNFUI_TEST_REPOSITORY_CLIENT");
  if (!enabled || std::string(enabled) != "1") {
    SKIP("Set DNFUI_TEST_REPOSITORY_CLIENT=1 to run repository client daemon tests.");
  }
}

std::vector<RepositoryInfo>
list_repositories_or_fail()
{
  std::vector<RepositoryInfo> repositories;
  std::string error;
  REQUIRE(repository_service_client_list(repositories, error));
  REQUIRE(error.empty());
  return repositories;
}

const RepositoryInfo *
find_repository(const std::vector<RepositoryInfo> &repositories, const std::string &id)
{
  auto it = std::find_if(
      repositories.begin(), repositories.end(), [&](const RepositoryInfo &repository) { return repository.id == id; });

  if (it == repositories.end()) {
    return nullptr;
  }

  return &*it;
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that valid repository maps become sorted plain values.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client parser accepts valid repository list")
{
  GVariant *repositories = repository_maps_variant({
      { "updates", "Fedora Updates", true, true },
      { "fedora", "Fedora", true, false },
  });

  std::vector<RepositoryInfo> parsed;
  std::string error;
  REQUIRE(repository_service_client_testonly_parse_repository_list(repositories, parsed, error));
  g_variant_unref(repositories);

  REQUIRE(error.empty());
  REQUIRE(parsed.size() == 2);
  REQUIRE(parsed[0].id == "fedora");
  REQUIRE(parsed[0].name == "Fedora");
  REQUIRE_FALSE(parsed[0].enabled);
  REQUIRE(parsed[1].id == "updates");
  REQUIRE(parsed[1].name == "Fedora Updates");
  REQUIRE(parsed[1].enabled);
}

// -----------------------------------------------------------------------------
// Verify that an empty repository name falls back to the ID.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client parser falls back to ID for empty name")
{
  GVariant *repositories = repository_maps_variant({
      { "fedora", "", true, true },
  });

  std::vector<RepositoryInfo> parsed;
  std::string error;
  REQUIRE(repository_service_client_testonly_parse_repository_list(repositories, parsed, error));
  g_variant_unref(repositories);

  REQUIRE(error.empty());
  REQUIRE(parsed.size() == 1);
  REQUIRE(parsed[0].name == "fedora");
}

// -----------------------------------------------------------------------------
// Verify that malformed daemon repository objects are rejected.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client parser rejects missing identity and enabled state")
{
  std::vector<RepositoryInfo> parsed;
  std::string error;

  GVariant *missing_id = repository_maps_variant({
      { nullptr, "No ID", true, true },
  });
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(missing_id, parsed, error));
  g_variant_unref(missing_id);
  REQUIRE(error.find("ID") != std::string::npos);
  REQUIRE(parsed.empty());

  GVariant *empty_id = repository_maps_variant({
      { "", "Empty ID", true, true },
  });
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(empty_id, parsed, error));
  g_variant_unref(empty_id);
  REQUIRE(error.find("ID") != std::string::npos);
  REQUIRE(parsed.empty());

  GVariant *missing_enabled = repository_maps_variant({
      { "fedora", "Fedora", false, false },
  });
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(missing_enabled, parsed, error));
  g_variant_unref(missing_enabled);
  REQUIRE(error.find("enabled") != std::string::npos);
  REQUIRE(parsed.empty());

  GVariant *wrong_enabled = repository_maps_variant({
      { "fedora", "Fedora", true, false, true },
  });
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(wrong_enabled, parsed, error));
  g_variant_unref(wrong_enabled);
  REQUIRE(error.find("enabled") != std::string::npos);
  REQUIRE(parsed.empty());
}

// -----------------------------------------------------------------------------
// Verify that duplicate repository IDs are rejected.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client parser rejects duplicate repository IDs")
{
  GVariant *repositories = repository_maps_variant({
      { "fedora", "Fedora", true, true },
      { "fedora", "Fedora Copy", true, false },
  });

  std::vector<RepositoryInfo> parsed;
  std::string error;
  REQUIRE_FALSE(repository_service_client_testonly_parse_repository_list(repositories, parsed, error));
  g_variant_unref(repositories);

  REQUIRE(error.find("duplicate") != std::string::npos);
  REQUIRE(parsed.empty());
}

// -----------------------------------------------------------------------------
// Verify that repository writes can wait beyond the default D-Bus timeout.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client waits for a delayed write reply", "[.slow]")
{
  DelayedRepositoryDaemon daemon;
  ScopedEnvVar system_bus("DBUS_SYSTEM_BUS_ADDRESS", daemon.address.c_str());

  auto started = std::chrono::steady_clock::now();
  RepositoryWriteResult result = repository_service_client_apply_changes({ "test-repo" }, {});
  auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(result.enable_attempted);
  REQUIRE(result.enable_succeeded);
  REQUIRE(result.error.empty());
  REQUIRE(elapsed >= std::chrono::seconds(25));
}

// -----------------------------------------------------------------------------
// Verify that repository enable and disable changes persist through fresh daemon sessions.
// -----------------------------------------------------------------------------
TEST_CASE("Repository client applies changes through fresh daemon sessions", "[dnf5daemon]")
{
  require_repository_client_test_enabled();

  const char *repo_id_env = g_getenv("DNFUI_TEST_REPOSITORY_CLIENT_REPO_ID");
  const std::string repo_id = repo_id_env && *repo_id_env ? repo_id_env : "dnfui-repository-client-test";

  std::vector<RepositoryInfo> initial = list_repositories_or_fail();
  const RepositoryInfo *initial_repo = find_repository(initial, repo_id);
  REQUIRE(initial_repo);
  REQUIRE_FALSE(initial_repo->enabled);

  RepositoryWriteResult enable_result = repository_service_client_apply_changes({ repo_id }, {});
  REQUIRE(enable_result.enable_attempted);
  REQUIRE(enable_result.enable_succeeded);
  REQUIRE_FALSE(enable_result.disable_attempted);
  REQUIRE(enable_result.disable_succeeded);
  REQUIRE(enable_result.error.empty());

  std::vector<RepositoryInfo> after_enable = list_repositories_or_fail();
  const RepositoryInfo *enabled_repo = find_repository(after_enable, repo_id);
  REQUIRE(enabled_repo);
  REQUIRE(enabled_repo->enabled);

  RepositoryWriteResult disable_result = repository_service_client_apply_changes({}, { repo_id });
  REQUIRE_FALSE(disable_result.enable_attempted);
  REQUIRE(disable_result.enable_succeeded);
  REQUIRE(disable_result.disable_attempted);
  REQUIRE(disable_result.disable_succeeded);
  REQUIRE(disable_result.error.empty());

  std::vector<RepositoryInfo> after_disable = list_repositories_or_fail();
  const RepositoryInfo *disabled_repo = find_repository(after_disable, repo_id);
  REQUIRE(disabled_repo);
  REQUIRE_FALSE(disabled_repo->enabled);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
