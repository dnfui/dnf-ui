// -----------------------------------------------------------------------------
// test_package_queries.cpp
// Package search, browsing, and result merge tests
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "dnf_backend/dnf_internal.hpp"
#include "test_utils.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

std::set<std::string>
collect_real_installonly_nevras(bool installed)
{
  std::set<std::string> nevras;

  auto read = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(read.base);
  query.filter_installonly();
  if (installed) {
    query.filter_installed();
  } else {
    query.filter_available();
  }

  for (const auto &pkg : query) {
    nevras.insert(pkg.get_nevra());
  }

  return nevras;
}

const PackageRow *
find_backend_test_row_by_nevras(const std::vector<PackageRow> &rows, const std::set<std::string> &nevras)
{
  for (const auto &row : rows) {
    if (nevras.count(row.nevra) > 0) {
      return &row;
    }
  }

  return nullptr;
}

PackageRow
make_backend_test_row(const std::string &nevra,
                      const std::string &name,
                      const std::string &version,
                      const std::string &release,
                      const std::string &arch,
                      const std::string &repo)
{
  PackageRow row;
  row.nevra = nevra;
  row.name = name;
  row.version = version;
  row.release = release;
  row.arch = arch;
  row.repo = repo;
  return row;
}

const PackageRow *
find_backend_test_row_by_nevra(const std::vector<PackageRow> &rows, const std::string &nevra)
{
  for (const auto &row : rows) {
    if (row.nevra == nevra) {
      return &row;
    }
  }

  return nullptr;
}

} // namespace

// -----------------------------------------------------------------------------
// Search behavior tests (read-only repo metadata)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that impossible package names do not produce fallback results.
// -----------------------------------------------------------------------------
TEST_CASE("Searching for impossible package name returns empty result")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible(
      "___definitely_not_a_real_package_123456___", backend_search_options(false, false), nullptr);

  REQUIRE(results.empty());
}

// -----------------------------------------------------------------------------
// Verify that exact search results remain a subset of contains search results.
// -----------------------------------------------------------------------------
TEST_CASE("Exact match results are subset of contains results")
{
  reset_backend_globals();

  auto contains = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false), nullptr);

  auto exact = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, true), nullptr);

  auto contains_nevras = package_row_nevras(contains);
  REQUIRE(contains.size() >= exact.size());
  for (const auto &row : exact) {
    INFO(row.nevra);
    REQUIRE(contains_nevras.count(row.nevra) == 1);
  }
}

// -----------------------------------------------------------------------------
// Verify that description search preserves all name-only matches.
// -----------------------------------------------------------------------------
TEST_CASE("Description search returns superset of name-only search")
{
  reset_backend_globals();

  auto name_only =
      dnf_backend_search_package_rows_interruptible("shell", backend_search_options(false, false), nullptr);

  auto desc_search =
      dnf_backend_search_package_rows_interruptible("shell", backend_search_options(true, false), nullptr);

  auto desc_search_nevras = package_row_nevras(desc_search);
  REQUIRE(desc_search.size() >= name_only.size());
  for (const auto &row : name_only) {
    INFO(row.nevra);
    REQUIRE(desc_search_nevras.count(row.nevra) == 1);
  }
}

// -----------------------------------------------------------------------------
// Verify that cancellation stops search without returning partial rows.
// -----------------------------------------------------------------------------
TEST_CASE("Cancelled search returns no results")
{
  reset_backend_globals();

  GCancellable *cancellable = g_cancellable_new();
  g_cancellable_cancel(cancellable);

  auto results =
      dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false), cancellable);

  REQUIRE(results.empty());
  g_object_unref(cancellable);
}

// -----------------------------------------------------------------------------
// Structured package row metadata tests
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that search rows contain the metadata needed by the table and details.
// -----------------------------------------------------------------------------
TEST_CASE("Structured package rows expose searchable metadata")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false), nullptr);
  REQUIRE(!results.empty());

  const auto &row = results.front();
  REQUIRE(!row.nevra.empty());
  REQUIRE(!row.name.empty());
  REQUIRE(!row.version.empty());
  REQUIRE(!row.release.empty());
  REQUIRE(!row.arch.empty());
  REQUIRE(!row.repo.empty());
  REQUIRE(!row.display_version().empty());
}

// -----------------------------------------------------------------------------
// Verify that installed rows expose install reason from the rpm database.
// -----------------------------------------------------------------------------
TEST_CASE("Installed package rows expose install reason")
{
  reset_backend_globals();

  auto rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE(!rows.empty());

  REQUIRE(std::any_of(rows.begin(), rows.end(), [](const PackageRow &row) {
    return row.install_reason != PackageInstallReason::UNKNOWN;
  }));
}

// -----------------------------------------------------------------------------
// Verify that merged search results keep one visible EVR per name and arch.
// -----------------------------------------------------------------------------
TEST_CASE("Search results keep one visible EVR per package name and architecture")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false), nullptr);
  REQUIRE(!results.empty());

  std::map<std::string, std::set<std::string>> versions_by_name_arch;
  for (const auto &row : results) {
    versions_by_name_arch[row.name_arch_key()].insert(row.display_version());
  }

  for (const auto &[key, versions] : versions_by_name_arch) {
    INFO(key);
    REQUIRE(versions.size() == 1);
  }
}

// -----------------------------------------------------------------------------
// Verify that the default browse view keeps the current latest-only row contract.
// -----------------------------------------------------------------------------
TEST_CASE("Browse results keep one latest row per package name and architecture by default")
{
  reset_backend_globals();

  auto results = dnf_backend_get_browse_package_rows_interruptible(backend_search_options(false, false), nullptr);
  REQUIRE(!results.empty());

  std::set<std::string> keys;
  for (const auto &row : results) {
    INFO(row.nevra);
    REQUIRE(keys.insert(row.name_arch_key()).second);
    if (row.repo != "@System") {
      REQUIRE(row.is_newest_available);
    }
  }
}

// -----------------------------------------------------------------------------
// Verify that real available installonly packages keep the backend installonly flag.
// -----------------------------------------------------------------------------
TEST_CASE("Backend marks real available installonly packages")
{
  reset_backend_globals();

  std::set<std::string> installonly_nevras = collect_real_installonly_nevras(false);
  if (installonly_nevras.empty()) {
    SKIP("Current repository metadata does not contain available installonly packages.");
  }

  auto compact_rows = dnf_backend_get_browse_package_rows_interruptible(backend_search_options(false, false), nullptr);
  const PackageRow *compact_row = find_backend_test_row_by_nevras(compact_rows, installonly_nevras);
  if (!compact_row) {
    SKIP("Current repository metadata does not expose an installonly package in the compact browse result.");
  }
  REQUIRE(compact_row->installonly);

  auto all_rows =
      dnf_backend_get_browse_package_rows_interruptible(backend_search_options(false, false, false), nullptr);
  const PackageRow *all_row = find_backend_test_row_by_nevra(all_rows, compact_row->nevra);
  REQUIRE(all_row != nullptr);
  REQUIRE(all_row->installonly);

  auto exact_rows = dnf_backend_get_available_package_rows_by_nevra(compact_row->nevra);
  const PackageRow *exact_row = find_backend_test_row_by_nevra(exact_rows, compact_row->nevra);
  REQUIRE(exact_row != nullptr);
  REQUIRE(exact_row->installonly);

  auto metadata_rows =
      dnf_backend_get_available_package_metadata_by_nevras_interruptible({ compact_row->nevra }, nullptr);
  const PackageRow *metadata_row = find_backend_test_row_by_nevra(metadata_rows, compact_row->nevra);
  REQUIRE(metadata_row != nullptr);
  REQUIRE(metadata_row->installonly);
}

// -----------------------------------------------------------------------------
// Verify that real installed installonly packages keep the backend installonly flag.
// -----------------------------------------------------------------------------
TEST_CASE("Backend marks real installed installonly packages")
{
  reset_backend_globals();

  std::set<std::string> installonly_nevras = collect_real_installonly_nevras(true);
  if (installonly_nevras.empty()) {
    SKIP("Current rpmdb does not contain installed installonly packages.");
  }

  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  const PackageRow *installed_row = find_backend_test_row_by_nevras(installed_rows, installonly_nevras);
  REQUIRE(installed_row != nullptr);
  REQUIRE(installed_row->installonly);

  auto exact_rows = dnf_backend_get_installed_package_rows_by_nevra(installed_row->nevra);
  const PackageRow *exact_row = find_backend_test_row_by_nevra(exact_rows, installed_row->nevra);
  REQUIRE(exact_row != nullptr);
  REQUIRE(exact_row->installonly);
}

// -----------------------------------------------------------------------------
// Verify that all-version browse rows are exact package versions without duplicate NEVRAs.
// -----------------------------------------------------------------------------
TEST_CASE("All-version browse results keep one row per exact NEVRA")
{
  reset_backend_globals();

  auto results =
      dnf_backend_get_browse_package_rows_interruptible(backend_search_options(false, false, false), nullptr);
  REQUIRE(!results.empty());

  std::set<std::string> nevras;
  for (const auto &row : results) {
    INFO(row.nevra);
    REQUIRE(nevras.insert(row.nevra).second);
  }
}

// -----------------------------------------------------------------------------
// Verify that all-version browse keeps installed packages visible by exact package ID.
// -----------------------------------------------------------------------------
TEST_CASE("All-version browse results include every installed NEVRA")
{
  reset_backend_globals();

  auto installed = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE(!installed.empty());

  auto results =
      dnf_backend_get_browse_package_rows_interruptible(backend_search_options(false, false, false), nullptr);
  auto result_nevras = package_row_nevras(results);

  for (const auto &row : installed) {
    INFO(row.nevra);
    REQUIRE(result_nevras.count(row.nevra) == 1);
  }
}

// -----------------------------------------------------------------------------
// Verify that all-version searches return exact package versions without duplicate NEVRAs.
// -----------------------------------------------------------------------------
TEST_CASE("All-version search results keep one row per exact NEVRA")
{
  reset_backend_globals();

  auto results =
      dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false, false), nullptr);
  REQUIRE(!results.empty());

  std::set<std::string> nevras;
  for (const auto &row : results) {
    INFO(row.nevra);
    REQUIRE(nevras.insert(row.nevra).second);
  }
}

// -----------------------------------------------------------------------------
// Verify that exact-version available views keep distinct available EVRs.
// -----------------------------------------------------------------------------
TEST_CASE("All-version merge keeps distinct available package versions")
{
  PackageRow older = make_backend_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64", "fedora");
  PackageRow newer = make_backend_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64", "updates");

  dnf_backend_internal::AvailableViewRows available;
  available.newest_available_by_name_arch.emplace(newer.name_arch_key(), newer);
  dnf_backend_internal::add_available_view_row(available, older);
  dnf_backend_internal::add_available_view_row(available, newer);

  dnf_backend_internal::InstalledQueryResult installed;
  auto rows = dnf_backend_internal::visible_rows_from_available_view(std::move(available), installed);

  const PackageRow *older_result = find_backend_test_row_by_nevra(rows, older.nevra);
  const PackageRow *newer_result = find_backend_test_row_by_nevra(rows, newer.nevra);

  REQUIRE(older_result != nullptr);
  REQUIRE(newer_result != nullptr);
  REQUIRE_FALSE(older_result->is_newest_available);
  REQUIRE(newer_result->is_newest_available);
}

// -----------------------------------------------------------------------------
// Verify that exact-version available views hide duplicate repository copies.
// -----------------------------------------------------------------------------
TEST_CASE("All-version merge hides duplicate available repository copies")
{
  PackageRow first_repo = make_backend_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64", "fedora");
  PackageRow second_repo = make_backend_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64", "updates");

  dnf_backend_internal::AvailableViewRows available;
  available.newest_available_by_name_arch.emplace(first_repo.name_arch_key(), first_repo);
  dnf_backend_internal::add_available_view_row(available, first_repo);
  dnf_backend_internal::add_available_view_row(available, second_repo);

  dnf_backend_internal::InstalledQueryResult installed;
  auto rows = dnf_backend_internal::visible_rows_from_available_view(std::move(available), installed);

  REQUIRE(rows.size() == 1);
  REQUIRE(rows.front().nevra == first_repo.nevra);
  REQUIRE(rows.front().repo == first_repo.repo);
}

// -----------------------------------------------------------------------------
// Verify that an installed NEVRA also present in a repository keeps one visible
// row and still receives update candidate data.
// -----------------------------------------------------------------------------
TEST_CASE("All-version merge annotates installed rows also present in repositories")
{
  PackageRow installed_row = make_backend_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64", "@System");
  PackageRow current_repo_row = make_backend_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64", "fedora");
  PackageRow update_row = make_backend_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64", "updates");

  dnf_backend_internal::AvailableViewRows available;
  available.newest_available_by_name_arch.emplace(update_row.name_arch_key(), update_row);
  dnf_backend_internal::add_available_view_row(available, current_repo_row);
  dnf_backend_internal::add_available_view_row(available, update_row);

  dnf_backend_internal::InstalledQueryResult installed;
  installed.rows = { installed_row };
  installed.nevras = { installed_row.nevra };
  installed.rows_by_name_arch.emplace(installed_row.name_arch_key(), installed_row);

  auto rows = dnf_backend_internal::visible_rows_from_available_view(std::move(available), installed);

  const PackageRow *current_result = find_backend_test_row_by_nevra(rows, current_repo_row.nevra);
  const PackageRow *update_result = find_backend_test_row_by_nevra(rows, update_row.nevra);

  REQUIRE(rows.size() == 2);
  REQUIRE(current_result != nullptr);
  REQUIRE(update_result != nullptr);
  REQUIRE(current_result->repo_candidate_relation == PackageRepoCandidateRelation::NEWER);
  REQUIRE(current_result->repo_candidate_nevra == update_row.nevra);
  REQUIRE(current_result->repo_candidate_version == update_row.version);
  REQUIRE(current_result->repo_candidate_release == update_row.release);
  REQUIRE(current_result->repo_candidate_repo == update_row.repo);
  REQUIRE(update_result->is_newest_available);

  reset_backend_globals();
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_row });
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(*current_result);

  REQUIRE(resolution.exact_installed);
  REQUIRE(resolution.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(resolution.installed_row.nevra == installed_row.nevra);
}

// -----------------------------------------------------------------------------
// Verify that an installed row is compared against the newest visible candidate.
// -----------------------------------------------------------------------------
TEST_CASE("All-version merge ignores hidden newest candidates for installed row annotation")
{
  PackageRow installed_row = make_backend_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64", "@System");
  PackageRow visible_repo_row = make_backend_test_row("demo-1.5-1.x86_64", "demo", "1.5", "1", "x86_64", "fedora");
  PackageRow hidden_newest_row = make_backend_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64", "updates");

  dnf_backend_internal::AvailableViewRows available;
  available.newest_available_by_name_arch.emplace(hidden_newest_row.name_arch_key(), hidden_newest_row);
  dnf_backend_internal::add_available_view_row(available, visible_repo_row);

  dnf_backend_internal::InstalledQueryResult installed;
  installed.rows = { installed_row };
  installed.nevras = { installed_row.nevra };
  installed.rows_by_name_arch.emplace(installed_row.name_arch_key(), installed_row);

  auto rows = dnf_backend_internal::visible_rows_from_available_view(std::move(available), installed);
  const PackageRow *installed_result = find_backend_test_row_by_nevra(rows, installed_row.nevra);

  REQUIRE(installed_result != nullptr);
  REQUIRE(installed_result->repo_candidate_relation == PackageRepoCandidateRelation::NEWER);
  REQUIRE(installed_result->repo_candidate_nevra == visible_repo_row.nevra);
  REQUIRE(installed_result->repo_candidate_nevra != hidden_newest_row.nevra);
  REQUIRE_FALSE(installed_result->repo_candidate_is_newest_available);
}

// -----------------------------------------------------------------------------
// Verify that exact-version available views keep every installed NEVRA.
// -----------------------------------------------------------------------------
TEST_CASE("All-version merge keeps distinct installed package versions")
{
  PackageRow installed_one = make_backend_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64", "@System");
  PackageRow installed_two = make_backend_test_row("demo-1.1-1.x86_64", "demo", "1.1", "1", "x86_64", "@System");

  dnf_backend_internal::AvailableViewRows available;

  dnf_backend_internal::InstalledQueryResult installed;
  installed.rows = { installed_one, installed_two };
  installed.nevras = { installed_one.nevra, installed_two.nevra };
  installed.rows_by_name_arch.emplace(installed_two.name_arch_key(), installed_two);

  auto rows = dnf_backend_internal::visible_rows_from_available_view(std::move(available), installed);

  REQUIRE(find_backend_test_row_by_nevra(rows, installed_one.nevra) != nullptr);
  REQUIRE(find_backend_test_row_by_nevra(rows, installed_two.nevra) != nullptr);
}

// -----------------------------------------------------------------------------
// Verify that all-version merge keeps exact reinstall availability found before visible rows are merged.
// -----------------------------------------------------------------------------
TEST_CASE("All-version merge keeps exact reinstall availability for hidden installed versions")
{
  PackageRow older_installed = make_backend_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64", "@System");
  older_installed.repo_candidate_exact_available = true;

  PackageRow newer_installed = make_backend_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64", "@System");
  newer_installed.repo_candidate_exact_available = true;

  PackageRow visible_update = make_backend_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64", "updates");
  visible_update.is_newest_available = true;

  dnf_backend_internal::AvailableViewRows available;
  available.newest_available_by_name_arch.emplace(visible_update.name_arch_key(), visible_update);
  dnf_backend_internal::add_available_view_row(available, visible_update);

  dnf_backend_internal::InstalledQueryResult installed;
  installed.rows = { older_installed, newer_installed };
  installed.nevras = { older_installed.nevra, newer_installed.nevra };
  installed.rows_by_name_arch.emplace(newer_installed.name_arch_key(), newer_installed);

  auto rows = dnf_backend_internal::visible_rows_from_available_view(std::move(available), installed);
  const PackageRow *older_result = find_backend_test_row_by_nevra(rows, older_installed.nevra);

  REQUIRE(older_result != nullptr);
  REQUIRE(older_result->repo_candidate_exact_available);
}

// -----------------------------------------------------------------------------
// Verify that cancelling all-version browse returns no rows and does not publish a partial installed snapshot.
// -----------------------------------------------------------------------------
TEST_CASE("Cancelled all-version browse returns no results")
{
  reset_backend_globals();

  GCancellable *cancellable = g_cancellable_new();
  g_cancellable_cancel(cancellable);

  auto results =
      dnf_backend_get_browse_package_rows_interruptible(backend_search_options(false, false, false), cancellable);

  REQUIRE(results.empty());
  REQUIRE(dnf_backend_installed_snapshot_size_for_tests() == 0);
  g_object_unref(cancellable);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
