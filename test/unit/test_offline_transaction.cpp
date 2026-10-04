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
    <method name="clean_with_options">
      <arg type="a{sv}" direction="in"/>
      <arg type="b" direction="out"/><arg type="s" direction="out"/>
    </method>
  </interface>
</node>)xml";

struct OfflineScenario {
  std::vector<std::pair<std::string, std::string>> packages { { "upgrade", "dnf5daemon-server" } };
  bool fail_apply = false;
  bool prepared_scheduled = true;
  std::string prepared_state = "ready";
  bool clean_dbus_error = false;
  bool clean_rejected = false;
  bool clean_retains_state = false;
  bool clean_status_error = false;
};

// -----------------------------------------------------------------------------
// Own a private bus without changing the process environment or shared session bus.
// -----------------------------------------------------------------------------
struct PrivateTestBus {
  GSubprocess *process = nullptr;

  ~PrivateTestBus()
  {
    if (process) {
      g_subprocess_force_exit(process);
      g_subprocess_wait(process, nullptr, nullptr);
      g_object_unref(process);
    }
  }

  std::string start()
  {
    process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE, nullptr, "dbus-daemon", "--session", "--nofork", "--print-address=1", nullptr);
    REQUIRE(process != nullptr);
    GDataInputStream *output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
    gchar *line = g_data_input_stream_read_line(output, nullptr, nullptr, nullptr);
    g_object_unref(output);
    REQUIRE(line != nullptr);
    std::string address = line;
    g_free(line);
    REQUIRE_FALSE(address.empty());
    return address;
  }
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
    std::string bus_address = bus.start();
    GError *error = nullptr;
    server = connect_to_test_bus(bus_address.c_str(), &error);
    REQUIRE(server != nullptr);
    client = connect_to_test_bus(bus_address.c_str(), &error);
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
    // Dispatch shutdown after the worker enters its loop, even when no request was sent.
    GSource *shutdown = g_idle_source_new();
    g_source_set_callback(
        shutdown,
        +[](gpointer data) -> gboolean {
          g_main_loop_quit(static_cast<GMainLoop *>(data));
          return G_SOURCE_REMOVE;
        },
        loop,
        nullptr);
    g_source_attach(shutdown, context);
    g_source_unref(shutdown);
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

  bool discard(std::string &error, GCancellable *cancellable = nullptr)
  {
    return transaction_service_client_discard_offline_request(client, kSessionPath, error, cancellable);
  }

  std::atomic<unsigned> clean_calls { 0 };
  std::atomic<bool> clean_interactive { false };
  std::atomic<unsigned> apply_calls { 0 };
  std::atomic<bool> applied_offline { false };
  std::atomic<bool> interactive { false };

  private:
  void handle(const std::string &method, GVariant *parameters, GDBusMethodInvocation *invocation)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (method == "get_status") {
      if (scenario.clean_status_error && clean_calls > 0) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.rpm.dnf.v0.Error", "Status unavailable.");
        return;
      }
      GVariantBuilder fields;
      g_variant_builder_init(&fields, G_VARIANT_TYPE("a{sv}"));
      if (!state.empty()) {
        g_variant_builder_add(&fields, "{sv}", "status", g_variant_new_string(state.c_str()));
      }
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(ba{sv})", scheduled, &fields));
    } else if (method == "clean_with_options") {
      ++clean_calls;
      GVariant *options = g_variant_get_child_value(parameters, 0);
      gboolean allow_interactive = FALSE;
      g_variant_lookup(options, "interactive", "b", &allow_interactive);
      g_variant_unref(options);
      clean_interactive = allow_interactive;
      if (scenario.clean_dbus_error) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.rpm.dnf.v0.Error", "Not authorized.");
      } else {
        if (!scenario.clean_rejected && !scenario.clean_retains_state) {
          state.clear();
          scheduled = false;
        }
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(bs)", !scenario.clean_rejected, "Cleanup refused."));
      }
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
        if (offline) {
          state = scenario.prepared_state;
          scheduled = scenario.prepared_scheduled;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
      }
    }
  }

  OfflineScenario scenario;
  std::mutex mutex;
  std::string state;
  bool scheduled = false;
  PrivateTestBus bus;
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
// Immediate teardown must finish and leave later tests using the original environment.
// -----------------------------------------------------------------------------
TEST_CASE("Offline fixture preserves the process environment without requests", "[offline-transaction]")
{
  gchar **before = g_get_environ();
  {
    OfflineDaemonFixture daemon;
  }
  gchar **after = g_get_environ();
  bool unchanged = g_strv_equal(before, after);
  g_strfreev(before);
  g_strfreev(after);
  REQUIRE(unchanged);
}

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
  REQUIRE_FALSE(error.empty());
  REQUIRE_FALSE(daemon.preview(preview, error));
  REQUIRE(preview.empty());
}

TEST_CASE("Ready offline changes explain reboot and in-app discard", "[offline-transaction]")
{
  OfflineDaemonFixture daemon;
  daemon.set_status("ready", true);
  TransactionPreview preview;
  std::string error;
  REQUIRE_FALSE(daemon.preview(preview, error));
  REQUIRE(error.find("Restart to install them") != std::string::npos);
  REQUIRE(error.find("Package > Discard Prepared Updates") != std::string::npos);
  REQUIRE(error.find("dnf5 offline status") == std::string::npos);
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
    scenario.prepared_scheduled = false;
    scenario.prepared_state.clear();
  }
  SECTION("ready state without scheduling")
  {
    scenario.prepared_scheduled = false;
  }
  SECTION("scheduled without ready state")
  {
    scenario.prepared_state = "download-complete";
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

TEST_CASE("Discarding stored DNF updates unblocks a fresh transaction", "[offline-transaction]")
{
  OfflineDaemonFixture daemon;
  SECTION("ready")
  {
    daemon.set_status("ready", true);
  }
  SECTION("partial preparation")
  {
    daemon.set_status("download-incomplete", false);
  }
  SECTION("cancelled but still stored")
  {
    daemon.set_status("download-complete", false);
  }
  TransactionPreview preview;
  std::string error;
  REQUIRE_FALSE(daemon.preview(preview, error));
  REQUIRE(daemon.discard(error));
  REQUIRE(error.empty());
  REQUIRE(daemon.clean_calls == 1);
  REQUIRE(daemon.clean_interactive);
  REQUIRE(daemon.apply_calls == 0);
  REQUIRE(daemon.preview(preview, error));
  REQUIRE(daemon.discard(error));
  REQUIRE(daemon.clean_calls == 2);
}

TEST_CASE("Discard requests cleanup when offline status is empty", "[offline-transaction]")
{
  OfflineScenario scenario;
  SECTION("cleanup succeeds")
  {
  }
  SECTION("cleanup fails")
  {
    scenario.clean_rejected = true;
  }
  OfflineDaemonFixture daemon(scenario);
  std::string error;
  REQUIRE(daemon.discard(error) == !scenario.clean_rejected);
  REQUIRE(error.empty() == !scenario.clean_rejected);
  REQUIRE(daemon.clean_calls == 1);
  REQUIRE(daemon.clean_interactive);
  REQUIRE(daemon.apply_calls == 0);
}

TEST_CASE("Discard errors and unverified cleanup do not report success", "[offline-transaction]")
{
  OfflineScenario scenario;
  SECTION("authorization or D-Bus failure")
  {
    scenario.clean_dbus_error = true;
  }
  SECTION("daemon reports cleanup failure")
  {
    scenario.clean_rejected = true;
  }
  SECTION("daemon leaves stored state behind")
  {
    scenario.clean_retains_state = true;
  }
  SECTION("status check fails after cleanup")
  {
    scenario.clean_status_error = true;
  }
  OfflineDaemonFixture daemon(scenario);
  daemon.set_status("ready", true);
  std::string error;
  REQUIRE_FALSE(daemon.discard(error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(daemon.clean_calls == 1);
  REQUIRE(daemon.apply_calls == 0);
  TransactionPreview preview;
  REQUIRE_FALSE(daemon.preview(preview, error));
}

TEST_CASE("Cancelled discard does not send a cleanup request", "[offline-transaction]")
{
  OfflineDaemonFixture daemon;
  daemon.set_status("ready", true);
  GCancellable *cancellable = g_cancellable_new();
  g_cancellable_cancel(cancellable);
  std::string error;
  bool discarded = daemon.discard(error, cancellable);
  g_object_unref(cancellable);
  REQUIRE_FALSE(discarded);
  REQUIRE_FALSE(error.empty());
  REQUIRE(daemon.clean_calls == 0);
}
