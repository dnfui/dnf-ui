// -----------------------------------------------------------------------------
// Offline transaction safety tests
// A private daemon fixture records calls without changing installed packages.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf5daemon_client/transaction_service_client_internal.hpp"
#include "transaction/transaction_preview.hpp"
#include "test_utils.hpp"

#include <atomic>
#include <mutex>
#include <thread>

namespace {

constexpr const char *kSessionPath = "/org/rpm/dnf/v0/test";
constexpr const char *kInterfaces = R"xml(
<node>
  <interface name="org.rpm.dnf.v0.Goal">
    <method name="resolve">
      <arg type="a{sv}" direction="in"/>
      <arg type="a(sssa{sv}a{sv})" direction="out"/>
      <arg type="u" direction="out"/>
    </method>
    <method name="do_transaction"><arg type="a{sv}" direction="in"/></method>
  </interface>
  <interface name="org.rpm.dnf.v0.Offline">
    <method name="get_status">
      <arg type="b" direction="out"/><arg type="a{sv}" direction="out"/>
    </method>
  </interface>
</node>)xml";

struct OfflineScenario {
  std::vector<std::pair<std::string, std::string>> packages { { "upgrade", "dnf5daemon-server" } };
  bool fail_apply = false;
  bool confirm_schedule = true;
};

class OfflineDaemonFixture {
  public:
  explicit OfflineDaemonFixture(OfflineScenario scenario = {})
      : scenario(std::move(scenario))
  {
    // These tests never change an existing host boot trigger.
    if (g_file_test("/system-update", G_FILE_TEST_IS_SYMLINK) ||
        g_file_test("/etc/system-update", G_FILE_TEST_IS_SYMLINK)) {
      SKIP("A host offline update is already scheduled.");
    }
    bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    GError *error = nullptr;
    server = connect_to_test_bus(g_test_dbus_get_bus_address(bus), &error);
    REQUIRE(server != nullptr);
    client = connect_to_test_bus(g_test_dbus_get_bus_address(bus), &error);
    REQUIRE(client != nullptr);
    GVariant *reply = g_dbus_connection_call_sync(server,
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
    REQUIRE(reply != nullptr);
    g_variant_unref(reply);

    context = g_main_context_new();
    loop = g_main_loop_new(context, FALSE);
    g_main_context_push_thread_default(context);
    info = g_dbus_node_info_new_for_xml(kInterfaces, &error);
    REQUIRE(info != nullptr);
    static const GDBusInterfaceVTable table = {
      +[](GDBusConnection *,
          const gchar *,
          const gchar *,
          const gchar *,
          const gchar *method,
          GVariant *parameters,
          GDBusMethodInvocation *invocation,
          gpointer data) { static_cast<OfflineDaemonFixture *>(data)->handle(method, parameters, invocation); },
      nullptr,
      nullptr,
      {}
    };
    for (GDBusInterfaceInfo **iface = info->interfaces; *iface; ++iface) {
      guint id = g_dbus_connection_register_object(server, kSessionPath, *iface, &table, this, nullptr, &error);
      REQUIRE(id != 0);
      registrations.push_back(id);
    }
    g_main_context_pop_thread_default(context);
    worker = std::thread([this] { g_main_loop_run(loop); });
  }

  ~OfflineDaemonFixture()
  {
    g_main_loop_quit(loop);
    worker.join();
    for (guint id : registrations) {
      g_dbus_connection_unregister_object(server, id);
    }
    g_dbus_connection_close_sync(client, nullptr, nullptr);
    g_dbus_connection_close_sync(server, nullptr, nullptr);
    g_object_unref(client);
    g_object_unref(server);
    g_dbus_node_info_unref(info);
    g_main_loop_unref(loop);
    g_main_context_unref(context);
    g_test_dbus_down(bus);
    g_object_unref(bus);
    transaction_service_client_reset_for_tests();
  }

  void set_status(const std::string &value, bool is_scheduled)
  {
    std::lock_guard<std::mutex> lock(mutex);
    state = value;
    scheduled = is_scheduled;
  }

  bool preview(TransactionPreview &result, std::string &error)
  {
    return transaction_service_client_get_transaction_preview(client, kSessionPath, nullptr, nullptr, result, error);
  }

  bool apply(bool offline, std::string &error)
  {
    return transaction_service_client_start_apply_request(client, kSessionPath, nullptr, nullptr, error, offline);
  }

  std::atomic<unsigned> apply_calls { 0 };
  std::atomic<bool> applied_offline { false };
  std::atomic<bool> interactive { false };

  private:
  void handle(const std::string &method, GVariant *parameters, GDBusMethodInvocation *invocation)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (method == "get_status") {
      GVariantBuilder fields;
      g_variant_builder_init(&fields, G_VARIANT_TYPE("a{sv}"));
      if (!state.empty()) {
        g_variant_builder_add(&fields, "{sv}", "status", g_variant_new_string(state.c_str()));
      }
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(ba{sv})", scheduled, &fields));
    } else if (method == "resolve") {
      GVariantBuilder items;
      g_variant_builder_init(&items, G_VARIANT_TYPE("a(sssa{sv}a{sv})"));
      for (const auto &[action, name] : scenario.packages) {
        GVariantBuilder attributes;
        GVariantBuilder package;
        g_variant_builder_init(&attributes, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_init(&package, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&package, "{sv}", "name", g_variant_new_string(name.c_str()));
        g_variant_builder_add(&package, "{sv}", "arch", g_variant_new_string("x86_64"));
        g_variant_builder_add(&package, "{sv}", "version", g_variant_new_string("2.0"));
        g_variant_builder_add(&package, "{sv}", "release", g_variant_new_string("1"));
        g_variant_builder_add(
            &items, "(sssa{sv}a{sv})", "package", action.c_str(), "dependency", &attributes, &package);
      }
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(sssa{sv}a{sv})u)", &items, 0u));
    } else if (method == "do_transaction") {
      ++apply_calls;
      GVariant *options = g_variant_get_child_value(parameters, 0);
      gboolean offline = FALSE;
      gboolean allow_interactive = FALSE;
      g_variant_lookup(options, "offline", "b", &offline);
      g_variant_lookup(options, "interactive", "b", &allow_interactive);
      g_variant_unref(options);
      applied_offline = offline;
      interactive = allow_interactive;
      if (scenario.fail_apply) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.rpm.dnf.v0.Error", "Failed to download packages.");
      } else {
        if (offline && scenario.confirm_schedule) {
          state = "ready";
          scheduled = true;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
      }
    }
  }

  OfflineScenario scenario;
  std::mutex mutex;
  std::string state;
  bool scheduled = false;
  GTestDBus *bus = nullptr;
  GDBusConnection *server = nullptr;
  GDBusConnection *client = nullptr;
  GDBusNodeInfo *info = nullptr;
  GMainContext *context = nullptr;
  GMainLoop *loop = nullptr;
  std::vector<guint> registrations;
  std::thread worker;
};

} // namespace

// -----------------------------------------------------------------------------
// Daemon replacement must never be submitted through the live apply path.
// -----------------------------------------------------------------------------
TEST_CASE("Daemon changes require the offline mode shown in their preview", "[offline-transaction]")
{
  OfflineScenario scenario;
  SECTION("upgrade with old package replacement")
  {
    scenario.packages = { { "upgrade", "dnf5daemon-server" },
                          { "replaced", "dnf5daemon-server" },
                          { "upgrade", "another-package" } };
  }
  SECTION("downgrade")
  {
    scenario.packages = { { "downgrade", "dnf5daemon-server" } };
  }
  SECTION("reinstall")
  {
    scenario.packages = { { "reinstall", "dnf5daemon-server" } };
  }
  SECTION("install-side dependency")
  {
    scenario.packages = { { "install", "dnf5daemon-server" } };
  }

  OfflineDaemonFixture daemon(scenario);
  TransactionPreview preview;
  std::string error;
  REQUIRE(daemon.preview(preview, error));
  REQUIRE(preview.requires_offline);
  REQUIRE_FALSE(daemon.apply(false, error));
  REQUIRE(daemon.apply_calls == 0);
  REQUIRE(daemon.apply(true, error));
  REQUIRE(daemon.apply_calls == 1);
  REQUIRE(daemon.applied_offline);
  REQUIRE(daemon.interactive);
  REQUIRE_FALSE(daemon.apply(true, error));
  REQUIRE(daemon.apply_calls == 1);
}

TEST_CASE("Ordinary package changes still apply live", "[offline-transaction]")
{
  OfflineScenario scenario;
  scenario.packages = { { "upgrade", "libdnf5" }, { "upgrade", "dnf5daemon-client" } };
  OfflineDaemonFixture daemon(scenario);
  TransactionPreview preview;
  std::string error;
  REQUIRE(daemon.preview(preview, error));
  REQUIRE_FALSE(preview.requires_offline);
  REQUIRE_FALSE(daemon.apply(true, error));
  REQUIRE(daemon.apply_calls == 0);
  REQUIRE(daemon.apply(false, error));
  REQUIRE_FALSE(daemon.applied_offline);
}

TEST_CASE("Stored offline changes are not overwritten by a new transaction", "[offline-transaction]")
{
  OfflineDaemonFixture daemon;
  std::string state;
  SECTION("scheduled")
  {
    state = "ready";
  }
  SECTION("download failed")
  {
    state = "download-incomplete";
  }
  SECTION("downloaded but not scheduled")
  {
    state = "download-complete";
  }
  SECTION("previous boot failed")
  {
    state = "transaction-incomplete";
  }
  SECTION("future state")
  {
    state = "future-state";
  }

  TransactionPreview preview;
  std::string error;
  REQUIRE(daemon.preview(preview, error));
  daemon.set_status(state, state == "ready");
  REQUIRE_FALSE(daemon.apply(true, error));
  REQUIRE(daemon.apply_calls == 0);
  REQUIRE(error.find("already exists") != std::string::npos);
  REQUIRE_FALSE(daemon.preview(preview, error));
  REQUIRE(preview.empty());
}

TEST_CASE("Offline errors never fall back to live apply", "[offline-transaction]")
{
  OfflineScenario scenario;
  SECTION("daemon rejects preparation")
  {
    scenario.fail_apply = true;
  }
  SECTION("daemon reply does not confirm scheduling")
  {
    scenario.confirm_schedule = false;
  }
  OfflineDaemonFixture daemon(scenario);
  TransactionPreview preview;
  std::string error;
  REQUIRE(daemon.preview(preview, error));
  REQUIRE_FALSE(daemon.apply(true, error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(daemon.apply_calls == 1);
  REQUIRE(daemon.applied_offline);
  REQUIRE_FALSE(daemon.apply(true, error));
  REQUIRE(daemon.apply_calls == 1);
}
