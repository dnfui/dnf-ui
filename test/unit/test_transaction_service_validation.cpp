// -----------------------------------------------------------------------------
// Transaction service validation tests
// Covers service-side request checks that run before backend transaction work.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "service/transaction_service_internal.hpp"
#include "test_utils.hpp"

#include <string>

namespace {

// -----------------------------------------------------------------------------
// Return one installed package row from the test environment.
// -----------------------------------------------------------------------------
PackageRow
first_installed_package_row()
{
  auto rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE_FALSE(rows.empty());
  return rows.front();
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that package installs do not need installed-package self-protection checks.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service validation accepts install-only requests")
{
  reset_backend_globals();

  TransactionRequest request;
  request.install.push_back("demo-install-1-1.x86_64");
  std::string error = "stale";

  REQUIRE(validate_transaction_request_for_service(request, error));
  REQUIRE(error.empty());
}

// -----------------------------------------------------------------------------
// Verify that preview authorization stays open on the session bus.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service preview authorization skips session bus")
{
  TransactionService service;
  service.bus_type = G_BUS_TYPE_SESSION;
  std::string error = "stale";

  REQUIRE(authorize_preview_start(&service, ":1.23", error));
  REQUIRE(error.empty());
}

// -----------------------------------------------------------------------------
// Verify that preview authorization requires a caller identity on the system bus.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service preview authorization requires caller identity")
{
  TransactionService service;
  service.bus_type = G_BUS_TYPE_SYSTEM;
  std::string error;

  REQUIRE_FALSE(authorize_preview_start(&service, "", error));
  REQUIRE(error == "Could not determine the caller identity.");
}

// -----------------------------------------------------------------------------
// Verify that remove and reinstall validation refreshes installed package state.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service validation refreshes installed state for remove and reinstall")
{
  reset_backend_globals();
  dnf_backend_testonly_clear_installed_snapshot();
  REQUIRE(dnf_backend_installed_snapshot_size() == 0);

  TransactionRequest request;
  request.remove.push_back("not-installed-by-this-name");
  request.reinstall.push_back("also-not-installed-by-this-name");
  std::string error = "stale";

  REQUIRE(validate_transaction_request_for_service(request, error));
  REQUIRE(error.empty());
  REQUIRE(dnf_backend_installed_snapshot_size() > 0);
}

// -----------------------------------------------------------------------------
// Verify that the service blocks removal of the package that owns the app.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service validation rejects self-protected removals")
{
  reset_backend_globals();
  PackageRow row = first_installed_package_row();
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", row.name.c_str());

  TransactionRequest request;
  request.remove.push_back(row.nevra);
  std::string error;

  REQUIRE_FALSE(validate_transaction_request_for_service(request, error));
  REQUIRE(error == "DNF UI cannot remove the package that owns the running application.");
}

// -----------------------------------------------------------------------------
// Verify that self-protection validation does not leave a cached Base behind.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service validation keeps self-protection lookup local-only")
{
  reset_backend_globals();
  PackageRow row = first_installed_package_row();
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", row.name.c_str());

  auto &mgr = BaseManager::instance();
  mgr.reset_for_tests();
  REQUIRE_FALSE(mgr.has_cached_base_for_tests());

  TransactionRequest request;
  request.remove.push_back(row.nevra);
  std::string error;

  REQUIRE_FALSE(validate_transaction_request_for_service(request, error));
  REQUIRE(error == "DNF UI cannot remove the package that owns the running application.");
  REQUIRE_FALSE(mgr.has_cached_base_for_tests());
}

// -----------------------------------------------------------------------------
// Verify that the service blocks reinstalling the package that owns the app.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service validation rejects self-protected reinstalls")
{
  reset_backend_globals();
  PackageRow row = first_installed_package_row();
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", row.name.c_str());

  TransactionRequest request;
  request.reinstall.push_back(row.nevra);
  std::string error;

  REQUIRE_FALSE(validate_transaction_request_for_service(request, error));
  REQUIRE(error == "DNF UI cannot reinstall the package that owns the running application while it is running.");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
