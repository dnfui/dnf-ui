// -----------------------------------------------------------------------------
// test_dnf5daemon_client.cpp
// dnf5daemon transaction client tests
// Exercises the same preview client path used by the GTK frontend.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"
#include "transaction_request.hpp"
#include "transaction_service_client.hpp"

#include <glib.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Return the package used for daemon preview tests.
// -----------------------------------------------------------------------------
std::string
dnf5daemon_test_install_spec()
{
  const char *spec = g_getenv("DNFUI_TEST_DNF5DAEMON_INSTALL_SPEC");
  if (spec && *spec) {
    return spec;
  }
  return "cowsay";
}

// -----------------------------------------------------------------------------
// Return true when a preview section contains a package label by name.
// -----------------------------------------------------------------------------
bool
preview_section_contains_name(const std::vector<std::string> &items, const std::string &name)
{
  return std::any_of(
      items.begin(), items.end(), [&](const std::string &item) { return item.rfind(name + "-", 0) == 0; });
}

// -----------------------------------------------------------------------------
// Return true when progress contains the expected message.
// -----------------------------------------------------------------------------
bool
progress_contains(const std::vector<std::string> &progress_lines, const std::string &expected)
{
  return std::any_of(progress_lines.begin(), progress_lines.end(), [&](const std::string &line) {
    return line.find(expected) != std::string::npos;
  });
}

// -----------------------------------------------------------------------------
// Skip daemon tests unless the caller explicitly enabled them.
// -----------------------------------------------------------------------------
void
require_dnf5daemon_test_enabled()
{
  const char *enabled = g_getenv("DNFUI_TEST_DNF5DAEMON");
  if (!enabled || std::string(enabled) != "1") {
    SKIP("Set DNFUI_TEST_DNF5DAEMON=1 to run dnf5daemon client tests.");
  }
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that the client can ask dnf5daemon for an install preview.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client previews install requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest request;
  request.install.push_back(install_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());

  bool preview_contains_package = preview_section_contains_name(preview.install, install_spec);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
}

// -----------------------------------------------------------------------------
// Verify that the client can apply an install transaction through dnf5daemon.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client applies install requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest request;
  request.install.push_back(install_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());

  bool preview_contains_package = preview_section_contains_name(preview.install, install_spec);

  std::vector<std::string> progress_lines;
  bool applied = transaction_service_client_apply_started_request(
      transaction_path, [&](const std::string &line) { progress_lines.push_back(line); }, error);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
  REQUIRE(applied);
  REQUIRE(progress_contains(progress_lines, "Transaction applied successfully."));
}

// -----------------------------------------------------------------------------
// Verify that the client can preview removing an installed package.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client previews remove requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest request;
  request.remove.push_back(install_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());

  bool preview_contains_package = preview_section_contains_name(preview.remove, install_spec);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
}

// -----------------------------------------------------------------------------
// Verify that the client can preview reinstalling an installed package.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client previews reinstall requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest request;
  request.reinstall.push_back(install_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());

  bool preview_contains_package = preview_section_contains_name(preview.reinstall, install_spec);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
}

// -----------------------------------------------------------------------------
// Verify that daemon resolver failures are returned as normal client errors.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client reports resolve failure", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  TransactionRequest request;
  request.install.push_back("dnf-ui-definitely-not-a-real-package-name");

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE_FALSE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE(transaction_path.empty());
  REQUIRE_FALSE(error.empty());
  REQUIRE(preview.empty());

  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that connection failure is reported before any daemon session is opened.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client reports unavailable daemon", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  ScopedEnvVar missing_system_bus("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/tmp/dnfui-missing-system-bus");

  TransactionRequest request;
  request.install.push_back(dnf5daemon_test_install_spec());

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE_FALSE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE(transaction_path.empty());
  REQUIRE_FALSE(error.empty());
  REQUIRE(preview.empty());

  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
