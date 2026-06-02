#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"

#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Return true when search results include one package name.
// -----------------------------------------------------------------------------
bool
contains_package_name(const std::vector<PackageRow> &rows, const std::string &name)
{
  for (const auto &row : rows) {
    if (row.name == name) {
      return true;
    }
  }

  return false;
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that contains search can find a common repository package.
// -----------------------------------------------------------------------------
TEST_CASE("Search contains mode returns results for common package")
{
  reset_backend_globals();

  set_backend_search_options(false, false);

  auto results = dnf_backend_search_package_rows_interruptible("bash", nullptr);

  REQUIRE(!results.empty());
}

// -----------------------------------------------------------------------------
// Verify that exact search does not fall back to substring matching.
// -----------------------------------------------------------------------------
TEST_CASE("Search exact mode does not match partial substring")
{
  reset_backend_globals();

  set_backend_search_options(false, true);

  auto exact = dnf_backend_search_package_rows_interruptible("ba", nullptr);

  REQUIRE(exact.empty());
}

// -----------------------------------------------------------------------------
// Verify that enabling description search does not drop name-only matches.
// -----------------------------------------------------------------------------
TEST_CASE("Search description mode expands or equals name-only results")
{
  reset_backend_globals();

  set_backend_search_options(false, false);
  auto name_only = dnf_backend_search_package_rows_interruptible("shell", nullptr);

  set_backend_search_options(true, false);
  auto desc_search = dnf_backend_search_package_rows_interruptible("shell", nullptr);

  if (desc_search.size() < name_only.size()) {
    FAIL("Description search returned fewer results than name-only search");
  }
}

// -----------------------------------------------------------------------------
// Verify that description search includes package summaries shown in the table.
// -----------------------------------------------------------------------------
TEST_CASE("Search description mode includes package summaries")
{
  reset_backend_globals();

  set_backend_search_options(false, false);
  auto name_only = dnf_backend_search_package_rows_interruptible("bourne", nullptr);

  set_backend_search_options(true, false);
  auto desc_search = dnf_backend_search_package_rows_interruptible("bourne", nullptr);

  if (name_only.empty() && desc_search.empty()) {
    SKIP("Current repository metadata does not provide the expected bash summary search fixture.");
  }

  REQUIRE_FALSE(contains_package_name(name_only, "bash"));
  REQUIRE(contains_package_name(desc_search, "bash"));
}

// -----------------------------------------------------------------------------
// Verify that an impossible package name produces an empty result.
// -----------------------------------------------------------------------------
TEST_CASE("Search returns empty for impossible package name")
{
  reset_backend_globals();

  set_backend_search_options(false, false);

  auto results = dnf_backend_search_package_rows_interruptible("___definitely_not_a_real_package_987654___", nullptr);

  REQUIRE(results.empty());
}
