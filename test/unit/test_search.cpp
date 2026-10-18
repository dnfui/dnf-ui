#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_internal.hpp"

#include <libdnf5/repo/repo.hpp>
#include <libdnf5/repo/repo_sack.hpp>
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
  REQUIRE(dnf_backend_resolve_installed_package(*upper_bash).state ==
          dnf_backend_resolve_installed_package(*lower_bash).state);
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
// Find providers by installed file ownership, not package names.
// -----------------------------------------------------------------------------
TEST_CASE_METHOD(ProviderRepositoryFixture, "Find Provider resolves an installed executable path")
{
  reset_backend_globals();
  auto options = backend_search_options(false, false);
  options.find_provider = true;
  auto results = dnf_backend_search_package_rows_interruptible("/usr/bin/bash", options, nullptr);
  REQUIRE(contains_package_name(results, "bash"));
}

// -----------------------------------------------------------------------------
// Find an RPM capability supplied by a package.
// -----------------------------------------------------------------------------
TEST_CASE_METHOD(ProviderRepositoryFixture, "Find Provider resolves a package capability")
{
  reset_backend_globals();
  auto options = backend_search_options(false, false);
  options.find_provider = true;
  auto results = dnf_backend_search_package_rows_interruptible("dnfui-filelists-fixture", options, nullptr);
  REQUIRE(contains_package_name(results, "dnfui-filelists-fixture"));
}

// -----------------------------------------------------------------------------
// Unknown providers must not fall back to package-text search.
// -----------------------------------------------------------------------------
TEST_CASE_METHOD(ProviderRepositoryFixture, "Find Provider returns no matches for an unknown capability")
{
  reset_backend_globals();
  auto options = backend_search_options(false, false);
  options.find_provider = true;
  auto results =
      dnf_backend_search_package_rows_interruptible("___dnfui_nonexistent_capability_987654___", options, nullptr);
  REQUIRE(results.empty());
}

// -----------------------------------------------------------------------------
// A package absent from the installed rpmdb must still be found when its file
// appears only in repository filelists metadata (not primary metadata).
// -----------------------------------------------------------------------------
TEST_CASE("Find Provider resolves a repository-only file from filelists metadata")
{
  libdnf5::Base base;
  base.load_config();
  base.get_config().get_plugins_option().set(false);
  base_manager_enable_filelists_metadata(base);
  base.setup();

  auto repo_sack = base.get_repo_sack();
  auto repo = repo_sack->create_repo("dnfui-filelists-test");
  const std::string baseurl = std::string("file://") + DNFUI_TEST_PROVIDER_REPO;
  repo->get_config().get_baseurl_option().set(std::vector<std::string> { baseurl });
  repo_sack->load_repos(libdnf5::repo::Repo::Type::AVAILABLE);

  auto empty_repo = repo_sack->create_repo("dnfui-empty-filelists-test");
  empty_repo->get_config().get_enabled_option().set(true);

  auto matches =
      dnf_backend_internal::collect_provider_matching_nevras(base, "/usr/share/dnfui-review/file-2", nullptr);
  REQUIRE(matches.size() == 1);

  // Bare file names and filename globs must also find repository-only files.
  REQUIRE(dnf_backend_internal::collect_provider_matching_nevras(base, "file-2", nullptr) == matches);
  REQUIRE(dnf_backend_internal::collect_provider_matching_nevras(base, "file-*", nullptr) == matches);
  REQUIRE(dnf_backend_internal::collect_provider_matching_nevras(base, "file-[12]", nullptr) == matches);
  REQUIRE(dnf_backend_internal::collect_provider_matching_nevras(base, "/usr/share/dnfui-review/file-[12]", nullptr) ==
          matches);
  REQUIRE(dnf_backend_internal::collect_provider_matching_nevras(base, "file", nullptr).empty());

  libdnf5::rpm::PackageQuery query(base);
  query.filter_name(std::vector<std::string> { "dnfui-filelists-fixture" }, libdnf5::sack::QueryCmp::EQ);
  REQUIRE(query.size() == 1);
  for (auto package : query) {
    REQUIRE(matches.count(package.get_nevra()) == 1);
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
