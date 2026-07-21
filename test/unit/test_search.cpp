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

// -----------------------------------------------------------------------------
// Return the first search row for a package name.
// -----------------------------------------------------------------------------
const PackageRow *
find_package_name(const std::vector<PackageRow> &rows, const std::string &name)
{
  for (const auto &row : rows) {
    if (row.name == name) {
      return &row;
    }
  }

  return nullptr;
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that contains search can find a common repository package.
// -----------------------------------------------------------------------------
TEST_CASE("Search contains mode returns results for common package")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false), nullptr);

  REQUIRE(!results.empty());
}

// -----------------------------------------------------------------------------
// Verify that search casing does not change installed package state.
// -----------------------------------------------------------------------------
TEST_CASE("Search contains mode is case-insensitive for installed package annotation")
{
  reset_backend_globals();

  const DnfBackendSearchOptions options = backend_search_options(false, false);
  auto lower_results = dnf_backend_search_package_rows_interruptible("bash", options, nullptr);
  auto upper_results = dnf_backend_search_package_rows_interruptible("BASH", options, nullptr);

  const PackageRow *lower_bash = find_package_name(lower_results, "bash");
  const PackageRow *upper_bash = find_package_name(upper_results, "bash");

  REQUIRE(lower_bash != nullptr);
  REQUIRE(upper_bash != nullptr);
  REQUIRE(dnf_backend_get_package_install_state(*upper_bash) == dnf_backend_get_package_install_state(*lower_bash));
  REQUIRE(upper_bash->repo_candidate_relation == lower_bash->repo_candidate_relation);
}

// -----------------------------------------------------------------------------
// Verify that exact search does not fall back to substring matching.
// -----------------------------------------------------------------------------
TEST_CASE("Search exact mode does not match partial substring")
{
  reset_backend_globals();

  auto exact = dnf_backend_search_package_rows_interruptible("ba", backend_search_options(false, true), nullptr);

  REQUIRE(exact.empty());
}

// -----------------------------------------------------------------------------
// Verify that normal search accepts shell-style wildcard terms.
// -----------------------------------------------------------------------------
TEST_CASE("Search wildcard mode returns matching package names")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("ba*", backend_search_options(false, false), nullptr);

  REQUIRE(contains_package_name(results, "bash"));
}

// -----------------------------------------------------------------------------
// Verify that exact search treats wildcard characters as literal text.
// -----------------------------------------------------------------------------
TEST_CASE("Search exact mode treats wildcard characters literally")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("ba*", backend_search_options(false, true), nullptr);

  REQUIRE(results.empty());
}

// -----------------------------------------------------------------------------
// Verify that enabling description search does not drop name-only matches.
// -----------------------------------------------------------------------------
TEST_CASE("Search description mode expands or equals name-only results")
{
  reset_backend_globals();

  auto name_only =
      dnf_backend_search_package_rows_interruptible("shell", backend_search_options(false, false), nullptr);

  auto desc_search =
      dnf_backend_search_package_rows_interruptible("shell", backend_search_options(true, false), nullptr);

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

  auto name_only =
      dnf_backend_search_package_rows_interruptible("bourne", backend_search_options(false, false), nullptr);

  auto desc_search =
      dnf_backend_search_package_rows_interruptible("bourne", backend_search_options(true, false), nullptr);

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

  auto results = dnf_backend_search_package_rows_interruptible(
      "___definitely_not_a_real_package_987654___", backend_search_options(false, false), nullptr);

  REQUIRE(results.empty());
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
