// -----------------------------------------------------------------------------
// test/unit/test_transaction_service_client.cpp
// Transaction service client integration tests
// Covers GUI-side D-Bus client behavior that needs a live service process and private session bus.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "service/transaction_service_dbus.hpp"
#include "test_utils.hpp"
#include "transaction_request.hpp"
#include "transaction_service_client.hpp"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

#ifndef DNFUI_TEST_SERVICE_BIN
#define DNFUI_TEST_SERVICE_BIN ""
#endif

namespace {

constexpr size_t kExpectedMaxLiveRequestsPerClient = 8;

struct PreviewClientResult {
  bool ok = false;
  TransactionPreview preview;
  std::string transaction_path;
  std::string error;
};

// Save one environment variable and restore its previous state on scope exit.
struct ScopedEnvironmentOverride {
  // -----------------------------------------------------------------------------
  // Save the original environment variable value.
  // -----------------------------------------------------------------------------
  explicit ScopedEnvironmentOverride(const char *variable_name)
      : name(variable_name ? variable_name : "")
  {
    const char *current_value = g_getenv(name.c_str());
    if (current_value) {
      had_value = true;
      value = current_value;
    }
  }

  // -----------------------------------------------------------------------------
  // Restore the original environment variable value.
  // -----------------------------------------------------------------------------
  ~ScopedEnvironmentOverride()
  {
    if (had_value) {
      g_setenv(name.c_str(), value.c_str(), TRUE);
    } else {
      g_unsetenv(name.c_str());
    }
  }

  std::string name;
  bool had_value = false;
  std::string value;
};

// -----------------------------------------------------------------------------
// Return true when the private test bus reports that the service name is owned.
// -----------------------------------------------------------------------------
static bool
wait_for_bus_name_owner(GDBusConnection *connection, const char *service_name, int timeout_ms)
{
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (std::chrono::steady_clock::now() < deadline) {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(connection,
                                                  "org.freedesktop.DBus",
                                                  "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus",
                                                  "NameHasOwner",
                                                  g_variant_new("(s)", service_name),
                                                  G_VARIANT_TYPE("(b)"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  nullptr,
                                                  &error);
    if (reply) {
      gboolean has_owner = FALSE;
      g_variant_get(reply, "(b)", &has_owner);
      g_variant_unref(reply);
      if (has_owner) {
        return true;
      }
    }

    g_clear_error(&error);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  return false;
}

// -----------------------------------------------------------------------------
// Return true when the preview worker writes its started marker file.
// -----------------------------------------------------------------------------
static bool
wait_for_file(const std::string &path, int timeout_ms)
{
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (std::chrono::steady_clock::now() < deadline) {
    if (g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  return false;
}

// -----------------------------------------------------------------------------
// Call StartTransaction directly so service-side request errors can be inspected.
// -----------------------------------------------------------------------------
static bool
call_start_transaction(GDBusConnection *connection,
                       const char *install_spec,
                       std::string &transaction_path_out,
                       std::string &error_out)
{
  transaction_path_out.clear();
  error_out.clear();

  GVariantBuilder install_builder;
  GVariantBuilder remove_builder;
  GVariantBuilder reinstall_builder;
  g_variant_builder_init(&install_builder, G_VARIANT_TYPE("as"));
  g_variant_builder_init(&remove_builder, G_VARIANT_TYPE("as"));
  g_variant_builder_init(&reinstall_builder, G_VARIANT_TYPE("as"));

  if (install_spec && *install_spec) {
    g_variant_builder_add(&install_builder, "s", install_spec);
  }

  GError *error = nullptr;
  GVariant *reply =
      g_dbus_connection_call_sync(connection,
                                  kTransactionServiceName,
                                  kTransactionServiceManagerPath,
                                  kTransactionServiceManagerInterface,
                                  "StartTransaction",
                                  g_variant_new("(asasas)", &install_builder, &remove_builder, &reinstall_builder),
                                  G_VARIANT_TYPE("(o)"),
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1,
                                  nullptr,
                                  &error);
  if (!reply) {
    error_out = error && error->message ? error->message : "";
    g_clear_error(&error);
    return false;
  }

  const char *transaction_path = nullptr;
  g_variant_get(reply, "(&o)", &transaction_path);
  if (transaction_path) {
    transaction_path_out = transaction_path;
  }
  g_variant_unref(reply);
  return true;
}

// -----------------------------------------------------------------------------
// Call Apply directly on a request object and return the D-Bus error text.
// -----------------------------------------------------------------------------
static bool
call_apply_transaction(GDBusConnection *connection, const std::string &transaction_path, std::string &error_out)
{
  error_out.clear();

  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(connection,
                                                kTransactionServiceName,
                                                transaction_path.c_str(),
                                                kTransactionServiceRequestInterface,
                                                "Apply",
                                                nullptr,
                                                nullptr,
                                                G_DBUS_CALL_FLAGS_NONE,
                                                -1,
                                                nullptr,
                                                &error);
  if (!reply) {
    error_out = error && error->message ? error->message : "";
    g_clear_error(&error);
    return false;
  }

  g_variant_unref(reply);
  return true;
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that the client returns an error if the service exits while waiting.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service client reports an error when the service disappears while waiting")
{
  REQUIRE(std::string(DNFUI_TEST_SERVICE_BIN).size() > 0);

  // Start one private session bus so the test does not depend on the user's
  // real desktop bus state.
  GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  REQUIRE(test_bus != nullptr);
  g_test_dbus_up(test_bus);

  ScopedEnvironmentOverride session_bus_address_env("DBUS_SESSION_BUS_ADDRESS");
  ScopedEnvironmentOverride transaction_bus_env("DNFUI_TRANSACTION_BUS");

  const char *bus_address = g_test_dbus_get_bus_address(test_bus);
  REQUIRE(bus_address != nullptr);
  REQUIRE(g_setenv("DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE));
  REQUIRE(g_setenv("DNFUI_TRANSACTION_BUS", "session", TRUE));

  GError *error = nullptr;
  gchar *temp_dir = g_dir_make_tmp("dnfui-service-client-XXXXXX", &error);
  std::string error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(temp_dir != nullptr);

  // Start the service with a test-only preview delay.
  // This gives the client time to enter its wait path before the service is stopped.
  std::string started_file = std::string(temp_dir) + "/preview-started";

  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
  REQUIRE(launcher != nullptr);
  g_subprocess_launcher_setenv(launcher, "DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE);
  g_subprocess_launcher_setenv(launcher, "DNFUI_TEST_PREVIEW_STARTED_FILE", started_file.c_str(), TRUE);
  g_subprocess_launcher_setenv(launcher, "DNFUI_TEST_PREVIEW_DELAY_MS", "10000", TRUE);

  const char *service_argv[] = {
    DNFUI_TEST_SERVICE_BIN,
    "--session",
    nullptr,
  };
  GSubprocess *service = g_subprocess_launcher_spawnv(launcher, service_argv, &error);
  error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(service != nullptr);
  g_object_unref(launcher);

  GDBusConnection *connection = connect_to_test_bus(bus_address, &error);
  error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(connection != nullptr);
  REQUIRE(wait_for_bus_name_owner(connection, "com.fedora.Dnfui.Transaction1", 5000));

  // Start one normal preview request through the GUI-side transaction client.
  TransactionRequest request;
  request.install.push_back("bash");

  auto future = std::async(std::launch::async, [request]() {
    PreviewClientResult result;
    result.ok =
        transaction_service_client_preview_request(request, result.preview, result.transaction_path, result.error);
    return result;
  });

  // Stop the service only after the preview worker has started so the test can
  // verify the client behavior while it is already waiting for the result.
  REQUIRE(wait_for_file(started_file, 5000));

  g_subprocess_force_exit(service);

  // The client should report a normal error instead of staying blocked.
  auto wait_status = future.wait_for(std::chrono::seconds(10));
  REQUIRE(wait_status == std::future_status::ready);

  PreviewClientResult result = future.get();
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error == "Transaction service disappeared while waiting for the result.");

  transaction_service_client_reset_for_tests();
  g_object_unref(connection);
  g_object_unref(service);
  g_remove(started_file.c_str());
  g_rmdir(temp_dir);
  g_free(temp_dir);
  g_test_dbus_down(test_bus);
  g_object_unref(test_bus);
}

// -----------------------------------------------------------------------------
// Verify that the service enforces the per-client live request limit.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service rejects too many active requests from one client")
{
  REQUIRE(std::string(DNFUI_TEST_SERVICE_BIN).size() > 0);

  GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  REQUIRE(test_bus != nullptr);
  g_test_dbus_up(test_bus);

  ScopedEnvironmentOverride session_bus_address_env("DBUS_SESSION_BUS_ADDRESS");
  ScopedEnvironmentOverride transaction_bus_env("DNFUI_TRANSACTION_BUS");

  const char *bus_address = g_test_dbus_get_bus_address(test_bus);
  REQUIRE(bus_address != nullptr);
  REQUIRE(g_setenv("DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE));
  REQUIRE(g_setenv("DNFUI_TRANSACTION_BUS", "session", TRUE));

  GError *error = nullptr;
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
  REQUIRE(launcher != nullptr);
  g_subprocess_launcher_setenv(launcher, "DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE);
  g_subprocess_launcher_setenv(launcher, "DNFUI_TEST_PREVIEW_DELAY_MS", "10000", TRUE);

  const char *service_argv[] = {
    DNFUI_TEST_SERVICE_BIN,
    "--session",
    nullptr,
  };
  GSubprocess *service = g_subprocess_launcher_spawnv(launcher, service_argv, &error);
  std::string error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(service != nullptr);
  g_object_unref(launcher);

  GDBusConnection *connection = connect_to_test_bus(bus_address, &error);
  error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(connection != nullptr);
  REQUIRE(wait_for_bus_name_owner(connection, kTransactionServiceName, 5000));

  for (size_t i = 0; i < kExpectedMaxLiveRequestsPerClient; i++) {
    std::string transaction_path;
    std::string start_error;

    REQUIRE(call_start_transaction(connection, "bash", transaction_path, start_error));
    REQUIRE_FALSE(transaction_path.empty());
  }

  std::string rejected_path;
  std::string rejected_error;
  REQUIRE_FALSE(call_start_transaction(connection, "bash", rejected_path, rejected_error));
  REQUIRE(rejected_error.find("This client has too many active transaction requests.") != std::string::npos);

  transaction_service_client_reset_for_tests();
  g_object_unref(connection);
  g_subprocess_force_exit(service);
  g_object_unref(service);
  g_test_dbus_down(test_bus);
  g_object_unref(test_bus);
}

// -----------------------------------------------------------------------------
// Verify that package-list preview helper rejects upgrade-all requests.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service client rejects upgrade-all through the package-list preview helper")
{
  TransactionRequest request;
  request.upgrade_all = true;

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE_FALSE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE(error == "Use the upgrade-all preview helper for upgrade-all requests.");
  REQUIRE(transaction_path.empty());
  REQUIRE(preview.empty());
}

// -----------------------------------------------------------------------------
// Verify that empty upgrade-all previews cannot be applied.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service upgrade-all preview handles no available package updates")
{
  REQUIRE(std::string(DNFUI_TEST_SERVICE_BIN).size() > 0);

  GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  REQUIRE(test_bus != nullptr);
  g_test_dbus_up(test_bus);

  ScopedEnvironmentOverride session_bus_address_env("DBUS_SESSION_BUS_ADDRESS");
  ScopedEnvironmentOverride transaction_bus_env("DNFUI_TRANSACTION_BUS");

  const char *bus_address = g_test_dbus_get_bus_address(test_bus);
  REQUIRE(bus_address != nullptr);
  REQUIRE(g_setenv("DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE));
  REQUIRE(g_setenv("DNFUI_TRANSACTION_BUS", "session", TRUE));

  GError *error = nullptr;
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
  REQUIRE(launcher != nullptr);
  g_subprocess_launcher_setenv(launcher, "DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE);
  g_subprocess_launcher_setenv(launcher, "DNFUI_TEST_FORCE_EMPTY_UPGRADE_ALL_PREVIEW", "1", TRUE);

  const char *service_argv[] = {
    DNFUI_TEST_SERVICE_BIN,
    "--session",
    nullptr,
  };
  GSubprocess *service = g_subprocess_launcher_spawnv(launcher, service_argv, &error);
  std::string error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(service != nullptr);
  g_object_unref(launcher);

  GDBusConnection *connection = connect_to_test_bus(bus_address, &error);
  error_text = error && error->message ? error->message : "";
  INFO(error_text);
  REQUIRE(connection != nullptr);
  REQUIRE(wait_for_bus_name_owner(connection, kTransactionServiceName, 5000));

  TransactionPreview preview;
  std::string transaction_path;
  std::string preview_error;
  REQUIRE(transaction_service_client_preview_upgrade_all_request(preview, transaction_path, preview_error));
  REQUIRE(preview_error.empty());
  REQUIRE_FALSE(transaction_path.empty());
  REQUIRE(preview.empty());

  std::string apply_error;
  REQUIRE_FALSE(call_apply_transaction(connection, transaction_path, apply_error));
  REQUIRE(apply_error.find("No package changes are available.") != std::string::npos);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();
  g_object_unref(connection);
  g_subprocess_force_exit(service);
  g_object_unref(service);
  g_test_dbus_down(test_bus);
  g_object_unref(test_bus);
}
