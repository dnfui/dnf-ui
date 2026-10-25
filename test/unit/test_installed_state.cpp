// -----------------------------------------------------------------------------
// test_installed_state.cpp
// Installed snapshot, resolution and repository annotation tests
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "dnf_backend/dnf_internal.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Installed package cache consistency tests (read-only)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that the published installed snapshot matches a full installed scan.
// -----------------------------------------------------------------------------
TEST_CASE("Installed package cache matches returned list")
{
  reset_backend_globals();

  auto list = dnf_backend_get_installed_package_rows_interruptible(nullptr);

  REQUIRE(list.size() == dnf_backend_installed_snapshot_size_for_tests());

  for (const auto &row : list) {
    REQUIRE(dnf_backend_installed_snapshot_contains_for_tests(row.nevra));
  }
}

// -----------------------------------------------------------------------------
// Verify that refreshing the installed snapshot creates a non-empty snapshot.
// -----------------------------------------------------------------------------
TEST_CASE("dnf_backend_refresh_installed_snapshot populates installed snapshot")
{
  reset_backend_globals();

  REQUIRE(dnf_backend_refresh_installed_snapshot());
  REQUIRE_FALSE(dnf_backend_refresh_installed_snapshot());

  REQUIRE(dnf_backend_installed_snapshot_size_for_tests() > 0);
}

// -----------------------------------------------------------------------------
// Verify that installed row metadata is published without reporting an identity change.
// -----------------------------------------------------------------------------
TEST_CASE("Installed snapshot publish ignores metadata-only row changes")
{
  reset_backend_globals();

  PackageRow first;
  first.nevra = "demo-1.0-1.fc44.x86_64";
  first.name = "demo";
  first.arch = "x86_64";
  first.repo = "@System";
  first.installed_from_repo = "fedora";

  dnf_backend_internal::InstalledQueryResult first_result;
  first_result.rows = { first };
  first_result.nevras = { first.nevra };
  first_result.rows_by_name_arch.emplace(first.name_arch_key(), first);

  REQUIRE(dnf_backend_internal::publish_installed_snapshot(std::move(first_result), {}));

  PackageRow second = first;
  second.installed_from_repo = "updates";

  dnf_backend_internal::InstalledQueryResult second_result;
  second_result.rows = { second };
  second_result.nevras = { second.nevra };
  second_result.rows_by_name_arch.emplace(second.name_arch_key(), second);

  REQUIRE_FALSE(dnf_backend_internal::publish_installed_snapshot(std::move(second_result), {}));

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(second);
  REQUIRE(resolution.has_installed_row);
  REQUIRE(resolution.installed_row.installed_from_repo == "updates");
}

// -----------------------------------------------------------------------------
// Verify that installed-only refresh does not require or cache repository metadata.
// -----------------------------------------------------------------------------
TEST_CASE("Installed package refresh uses a short-lived system-only Base")
{
  reset_backend_globals();

  auto &mgr = BaseManager::instance();
  mgr.reset_for_tests();
  REQUIRE_FALSE(mgr.has_cached_base_for_tests());

  {
    ScopedEnvVar force_failure("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
    ScopedEnvVar force_cache_failure("DNFUI_TEST_FORCE_CACHEONLY_REPO_LOAD_FAILURE", "1");

    REQUIRE_NOTHROW(dnf_backend_refresh_installed_snapshot());
    REQUIRE(dnf_backend_installed_snapshot_size_for_tests() > 0);
    REQUIRE_FALSE(mgr.has_cached_base_for_tests());
  }

  mgr.reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that passive installed-state checks keep the warmed shared Base.
// -----------------------------------------------------------------------------
TEST_CASE("Installed package refresh can preserve the cached Base")
{
  reset_backend_globals();

  auto &mgr = BaseManager::instance();
  mgr.reset_for_tests();

  REQUIRE_NOTHROW(mgr.acquire_read());
  REQUIRE(mgr.has_cached_base_for_tests());

  REQUIRE_NOTHROW(dnf_backend_refresh_installed_snapshot_preserving_cached_base());
  REQUIRE(dnf_backend_installed_snapshot_size_for_tests() > 0);
  REQUIRE(mgr.has_cached_base_for_tests());

  mgr.reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that daemon-target metadata lookup does not refresh installed state.
// -----------------------------------------------------------------------------
TEST_CASE_METHOD(ProviderRepositoryFixture, "Daemon upgrade metadata lookup does not publish installed state")
{
  reset_backend_globals();

  std::vector<PackageRow> rows =
      dnf_backend_get_browse_package_rows_interruptible(backend_search_options(false, false), nullptr);
  std::string target_nevra;
  for (const auto &row : rows) {
    if (row.repo != "@System") {
      target_nevra = row.nevra;
      break;
    }
  }
  REQUIRE(!target_nevra.empty());

  reset_backend_globals();
  REQUIRE(dnf_backend_installed_snapshot_size_for_tests() == 0);

  std::vector<PackageRow> metadata_rows =
      dnf_backend_get_available_package_metadata_by_nevras_interruptible({ target_nevra }, nullptr);

  REQUIRE(!metadata_rows.empty());
  REQUIRE(dnf_backend_installed_snapshot_size_for_tests() == 0);
}

// -----------------------------------------------------------------------------
// Verify that exact installed rows use repo relation to distinguish states.
// -----------------------------------------------------------------------------
TEST_CASE("Exact installed rows distinguish local-only and repo-backed states")
{
  reset_backend_globals();

  PackageRow row;
  row.nevra = "demo-1-1.x86_64";
  row.name = "demo";
  row.arch = "x86_64";

  dnf_backend_testonly_replace_installed_snapshot({ row.nevra });

  row.repo_candidate_relation = PackageRepoCandidateRelation::UNKNOWN;
  REQUIRE(dnf_backend_resolve_installed_package(row).state == PackageInstallState::INSTALLED);

  row.repo_candidate_relation = PackageRepoCandidateRelation::NONE;
  REQUIRE(dnf_backend_resolve_installed_package(row).state == PackageInstallState::LOCAL_ONLY);

  row.repo_candidate_relation = PackageRepoCandidateRelation::SAME;
  REQUIRE(dnf_backend_resolve_installed_package(row).state == PackageInstallState::INSTALLED);

  row.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  REQUIRE(dnf_backend_resolve_installed_package(row).state == PackageInstallState::UPGRADEABLE);

  row.repo_candidate_relation = PackageRepoCandidateRelation::OLDER;
  REQUIRE(dnf_backend_resolve_installed_package(row).state == PackageInstallState::INSTALLED_NEWER_THAN_REPO);
}

// -----------------------------------------------------------------------------
// Verify that an older installed EVR is not treated as upgradeable when the candidate version is already installed.
// -----------------------------------------------------------------------------
TEST_CASE("Older parallel installed rows do not expose already-installed updates")
{
  reset_backend_globals();

  PackageRow older_row;
  older_row.nevra = "demo-1.0-1.x86_64";
  older_row.name = "demo";
  older_row.version = "1.0";
  older_row.release = "1";
  older_row.arch = "x86_64";
  older_row.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  older_row.repo_candidate_nevra = "demo-2.0-1.x86_64";
  older_row.repo_candidate_version = "2.0";
  older_row.repo_candidate_release = "1";
  older_row.repo_candidate_is_newest_available = true;

  PackageRow newest_row = older_row;
  newest_row.nevra = "demo-2.0-1.x86_64";
  newest_row.version = "2.0";
  newest_row.repo_candidate_relation = PackageRepoCandidateRelation::SAME;
  newest_row.repo_candidate_nevra.clear();
  newest_row.repo_candidate_version.clear();
  newest_row.repo_candidate_release.clear();
  newest_row.repo_candidate_is_newest_available = false;

  dnf_backend_testonly_replace_installed_snapshot_rows({ older_row, newest_row });

  InstalledPackageResolution older_resolution = dnf_backend_resolve_installed_package(older_row);
  InstalledPackageResolution newest_resolution = dnf_backend_resolve_installed_package(newest_row);

  REQUIRE(older_resolution.exact_installed);
  REQUIRE(older_resolution.has_installed_row);
  REQUIRE(older_resolution.installed_row.nevra == newest_row.nevra);
  REQUIRE(older_resolution.state == PackageInstallState::INSTALLED);
  REQUIRE(newest_resolution.state == PackageInstallState::INSTALLED);
}

// -----------------------------------------------------------------------------
// Verify that only the newest installed EVR exposes a newer repository candidate.
// -----------------------------------------------------------------------------
TEST_CASE("Only newest parallel installed row exposes repository update")
{
  reset_backend_globals();

  PackageRow older_row;
  older_row.nevra = "demo-1.0-1.x86_64";
  older_row.name = "demo";
  older_row.version = "1.0";
  older_row.release = "1";
  older_row.arch = "x86_64";
  older_row.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  older_row.repo_candidate_nevra = "demo-3.0-1.x86_64";
  older_row.repo_candidate_version = "3.0";
  older_row.repo_candidate_release = "1";
  older_row.repo_candidate_is_newest_available = true;

  PackageRow newest_row = older_row;
  newest_row.nevra = "demo-2.0-1.x86_64";
  newest_row.version = "2.0";

  dnf_backend_testonly_replace_installed_snapshot_rows({ older_row, newest_row });

  InstalledPackageResolution older_resolution = dnf_backend_resolve_installed_package(older_row);
  InstalledPackageResolution newest_resolution = dnf_backend_resolve_installed_package(newest_row);

  REQUIRE(older_resolution.state == PackageInstallState::INSTALLED);
  REQUIRE(newest_resolution.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(newest_resolution.installed_row.nevra == newest_row.nevra);
}

// -----------------------------------------------------------------------------
// Verify that installed package resolution returns one coherent installed-state result.
// -----------------------------------------------------------------------------
TEST_CASE("Installed package resolution reports exact installed rows")
{
  reset_backend_globals();

  PackageRow row;
  row.nevra = "demo-1.0-1.x86_64";
  row.name = "demo";
  row.version = "1.0";
  row.release = "1";
  row.arch = "x86_64";

  dnf_backend_testonly_replace_installed_snapshot_rows({ row });

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(row);

  REQUIRE(resolution.state == PackageInstallState::INSTALLED);
  REQUIRE(resolution.exact_installed);
  REQUIRE(resolution.has_installed_row);
  REQUIRE(resolution.installed_row.nevra == row.nevra);
}

// -----------------------------------------------------------------------------
// Verify that installed package resolution reports available rows.
// -----------------------------------------------------------------------------
TEST_CASE("Installed package resolution reports available rows")
{
  reset_backend_globals();

  PackageRow row;
  row.nevra = "demo-1.0-1.x86_64";
  row.name = "demo";
  row.version = "1.0";
  row.release = "1";
  row.arch = "x86_64";

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(row);

  REQUIRE(resolution.state == PackageInstallState::AVAILABLE);
  REQUIRE_FALSE(resolution.exact_installed);
  REQUIRE_FALSE(resolution.has_installed_row);
}

// -----------------------------------------------------------------------------
// Verify that installed package resolution reports available updates.
// -----------------------------------------------------------------------------
TEST_CASE("Installed package resolution reports upgradeable rows")
{
  reset_backend_globals();

  PackageRow installed_row;
  installed_row.nevra = "demo-1.0-1.x86_64";
  installed_row.name = "demo";
  installed_row.version = "1.0";
  installed_row.release = "1";
  installed_row.arch = "x86_64";

  PackageRow update_row = installed_row;
  update_row.nevra = "demo-2.0-1.x86_64";
  update_row.version = "2.0";

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_row });

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(update_row);

  REQUIRE(resolution.state == PackageInstallState::UPGRADEABLE);
  REQUIRE_FALSE(resolution.exact_installed);
  REQUIRE(resolution.has_installed_row);
  REQUIRE(resolution.installed_row.nevra == installed_row.nevra);
}

// -----------------------------------------------------------------------------
// Verify that installed package resolution reports older available rows.
// -----------------------------------------------------------------------------
TEST_CASE("Installed package resolution reports downgradeable rows")
{
  reset_backend_globals();

  PackageRow installed_row;
  installed_row.nevra = "demo-2.0-1.x86_64";
  installed_row.name = "demo";
  installed_row.version = "2.0";
  installed_row.release = "1";
  installed_row.arch = "x86_64";

  PackageRow older_repo_row = installed_row;
  older_repo_row.nevra = "demo-1.0-1.x86_64";
  older_repo_row.version = "1.0";

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_row });

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(older_repo_row);

  REQUIRE(resolution.state == PackageInstallState::DOWNGRADEABLE);
  REQUIRE_FALSE(resolution.exact_installed);
  REQUIRE(resolution.has_installed_row);
  REQUIRE(resolution.installed_row.nevra == installed_row.nevra);
}

// -----------------------------------------------------------------------------
// Verify that installed package resolution reports local-only exact installed rows.
// -----------------------------------------------------------------------------
TEST_CASE("Installed package resolution reports local-only rows")
{
  reset_backend_globals();

  PackageRow row;
  row.nevra = "demo-1.0-1.x86_64";
  row.name = "demo";
  row.version = "1.0";
  row.release = "1";
  row.arch = "x86_64";
  row.repo_candidate_relation = PackageRepoCandidateRelation::NONE;

  dnf_backend_testonly_replace_installed_snapshot_rows({ row });

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(row);

  REQUIRE(resolution.state == PackageInstallState::LOCAL_ONLY);
  REQUIRE(resolution.exact_installed);
  REQUIRE(resolution.has_installed_row);
  REQUIRE(resolution.installed_row.nevra == row.nevra);
}

// -----------------------------------------------------------------------------
// Verify that install-state sorting keeps installed-like rows before available.
// -----------------------------------------------------------------------------
TEST_CASE("Default install state sort keeps installed rows first")
{
  REQUIRE(dnf_backend_get_install_state_sort_rank(PackageInstallState::INSTALLED) <
          dnf_backend_get_install_state_sort_rank(PackageInstallState::LOCAL_ONLY));
  REQUIRE(dnf_backend_get_install_state_sort_rank(PackageInstallState::LOCAL_ONLY) <
          dnf_backend_get_install_state_sort_rank(PackageInstallState::UPGRADEABLE));
  REQUIRE(dnf_backend_get_install_state_sort_rank(PackageInstallState::UPGRADEABLE) <
          dnf_backend_get_install_state_sort_rank(PackageInstallState::DOWNGRADEABLE));
  REQUIRE(dnf_backend_get_install_state_sort_rank(PackageInstallState::DOWNGRADEABLE) <
          dnf_backend_get_install_state_sort_rank(PackageInstallState::AVAILABLE));
}

// -----------------------------------------------------------------------------
// Verify that exact installed checks use the NEVRA snapshot, not name matching.
// -----------------------------------------------------------------------------
TEST_CASE("Exact installed checks use the cached installed NEVRA snapshot")
{
  reset_backend_globals();

  PackageRow exact_row;
  exact_row.nevra = "demo-1.0-1.x86_64";
  exact_row.name = "demo";
  exact_row.arch = "x86_64";

  PackageRow different_row = exact_row;
  different_row.nevra = "demo-2.0-1.x86_64";

  dnf_backend_testonly_replace_installed_snapshot({ exact_row.nevra });

  REQUIRE(dnf_backend_resolve_installed_package(exact_row).exact_installed);
  REQUIRE_FALSE(dnf_backend_resolve_installed_package(different_row).exact_installed);
}

// -----------------------------------------------------------------------------
// Verify that available upgrade candidates can resolve their installed row.
// -----------------------------------------------------------------------------
TEST_CASE("Installed row lookup resolves upgrade candidates by name and architecture")
{
  reset_backend_globals();

  PackageRow installed_row;
  installed_row.nevra = "demo-1.0-1.x86_64";
  installed_row.name = "demo";
  installed_row.version = "1.0";
  installed_row.release = "1";
  installed_row.arch = "x86_64";

  PackageRow upgrade_row = installed_row;
  upgrade_row.nevra = "demo-2.0-1.x86_64";
  upgrade_row.version = "2.0";

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_row });

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(upgrade_row);
  REQUIRE(resolution.has_installed_row);
  REQUIRE(resolution.installed_row.nevra == installed_row.nevra);
}

// -----------------------------------------------------------------------------
// Verify that repo annotation records exact repository availability.
// -----------------------------------------------------------------------------
TEST_CASE("Installed row annotation records exact available package")
{
  reset_backend_globals();

  PackageRow installed_row;
  installed_row.nevra = "demo-1.0-1.x86_64";
  installed_row.name = "demo";
  installed_row.version = "1.0";
  installed_row.release = "1";
  installed_row.arch = "x86_64";

  PackageRow available_row = installed_row;
  available_row.is_newest_available = true;

  dnf_backend_internal::annotate_installed_row_with_repo_candidate(
      installed_row, { { installed_row.name_arch_key(), available_row } });

  REQUIRE(installed_row.repo_candidate_relation == PackageRepoCandidateRelation::SAME);
  REQUIRE(installed_row.repo_candidate_nevra == installed_row.nevra);
  REQUIRE(installed_row.repo_candidate_exact_available);
}

// -----------------------------------------------------------------------------
// Verify that a newer candidate does not count as exact reinstall availability.
// -----------------------------------------------------------------------------
TEST_CASE("Installed row annotation rejects newer candidate as exact available package")
{
  reset_backend_globals();

  PackageRow installed_row;
  installed_row.nevra = "demo-1.0-1.x86_64";
  installed_row.name = "demo";
  installed_row.version = "1.0";
  installed_row.release = "1";
  installed_row.arch = "x86_64";

  PackageRow available_row = installed_row;
  available_row.nevra = "demo-2.0-1.x86_64";
  available_row.version = "2.0";
  available_row.is_newest_available = true;

  dnf_backend_internal::annotate_installed_row_with_repo_candidate(
      installed_row, { { installed_row.name_arch_key(), available_row } });

  REQUIRE(installed_row.repo_candidate_relation == PackageRepoCandidateRelation::NEWER);
  REQUIRE(installed_row.repo_candidate_nevra == available_row.nevra);
  REQUIRE_FALSE(installed_row.repo_candidate_exact_available);
}

// -----------------------------------------------------------------------------
// Verify that newest-candidate annotation keeps a previously known exact match.
// -----------------------------------------------------------------------------
TEST_CASE("Installed row annotation preserves exact availability with newer candidate")
{
  reset_backend_globals();

  PackageRow installed_row;
  installed_row.nevra = "demo-1.0-1.x86_64";
  installed_row.name = "demo";
  installed_row.version = "1.0";
  installed_row.release = "1";
  installed_row.arch = "x86_64";
  installed_row.repo_candidate_exact_available = true;

  PackageRow available_row = installed_row;
  available_row.nevra = "demo-2.0-1.x86_64";
  available_row.version = "2.0";
  available_row.is_newest_available = true;

  dnf_backend_internal::annotate_installed_row_with_repo_candidate(
      installed_row, { { installed_row.name_arch_key(), available_row } });

  REQUIRE(installed_row.repo_candidate_relation == PackageRepoCandidateRelation::NEWER);
  REQUIRE(installed_row.repo_candidate_nevra == available_row.nevra);
  REQUIRE(installed_row.repo_candidate_exact_available);
}

// -----------------------------------------------------------------------------
// Verify that visible-candidate annotation keeps exact availability from a separate query.
// -----------------------------------------------------------------------------
TEST_CASE("Installed row annotation preserves exact availability without visible candidate")
{
  reset_backend_globals();

  PackageRow installed_row;
  installed_row.nevra = "demo-1.0-1.x86_64";
  installed_row.name = "demo";
  installed_row.version = "1.0";
  installed_row.release = "1";
  installed_row.arch = "x86_64";
  installed_row.repo_candidate_exact_available = true;

  dnf_backend_internal::annotate_installed_row_with_repo_candidate(installed_row, {});

  REQUIRE(installed_row.repo_candidate_relation == PackageRepoCandidateRelation::NONE);
  REQUIRE(installed_row.repo_candidate_nevra.empty());
  REQUIRE(installed_row.repo_candidate_exact_available);
}

// -----------------------------------------------------------------------------
// Verify that failed repo annotation does not make installed rows unusable.
// -----------------------------------------------------------------------------
TEST_CASE("Annotation fallback keeps installed rows usable when repo lookup fails")
{
  reset_backend_globals();

  PackageRow row;
  row.nevra = "demo-1.0-1.x86_64";
  row.name = "demo";
  row.arch = "x86_64";

  dnf_backend_testonly_replace_installed_snapshot({ row.nevra });

  std::vector<PackageRow> rows { row };
  dnf_backend_internal::annotate_installed_rows_with_repo_candidates_best_effort(
      rows, nullptr, [](GCancellable *) -> std::map<std::string, PackageRow> {
        throw std::runtime_error("forced annotation failure");
      });

  REQUIRE(rows.size() == 1);
  REQUIRE(rows.front().repo_candidate_relation == PackageRepoCandidateRelation::UNKNOWN);
  REQUIRE(dnf_backend_resolve_installed_package(rows.front()).state == PackageInstallState::INSTALLED);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
