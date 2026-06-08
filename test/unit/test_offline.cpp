// -----------------------------------------------------------------------------
// test/unit/test_offline.cpp
// Offline behavior tests
// Covers cached repo search without network.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"

#include <algorithm>

// -----------------------------------------------------------------------------
// Verify that cached repository metadata can satisfy search without network.
// -----------------------------------------------------------------------------
TEST_CASE("Offline cached search finds a repo package", "[offline]")
{
  const char *repo_spec = g_getenv("DNFUI_TEST_OFFLINE_REPO_SPEC");
  if (!repo_spec || !*repo_spec) {
    SKIP("Set DNFUI_TEST_OFFLINE_REPO_SPEC to run the offline cached-search test.");
  }

  reset_backend_globals();

  auto &mgr = BaseManager::instance();
  mgr.reset_for_tests();
  set_backend_search_options(false, true);

  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE_FALSE(std::any_of(
      installed_rows.begin(), installed_rows.end(), [&](const PackageRow &row) { return row.name == repo_spec; }));

  auto results = dnf_backend_search_package_rows_interruptible(repo_spec, nullptr);

  INFO(repo_spec);
  REQUIRE(std::any_of(results.begin(), results.end(), [&](const PackageRow &row) { return row.name == repo_spec; }));

  mgr.reset_for_tests();
}
