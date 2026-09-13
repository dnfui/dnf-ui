#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "dnf_backend/dnf_internal.hpp"
#include "test_utils.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

// -----------------------------------------------------------------------------
// Supply an enabled plugin that cannot be loaded without changing system configuration.
// -----------------------------------------------------------------------------
struct UnloadablePluginConfig {
  std::filesystem::path directory;

  UnloadablePluginConfig()
  {
    gchar *temporary_directory = g_dir_make_tmp("dnfui-plugin-test-XXXXXX", nullptr);
    REQUIRE(temporary_directory != nullptr);
    directory = temporary_directory;
    g_free(temporary_directory);

    std::ofstream config(directory / "dnfui-unloadable.conf");
    config << "[main]\nname=dnfui-unloadable\nenabled=1\n";
    config.close();
    REQUIRE(config.good());
  }

  ~UnloadablePluginConfig()
  {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};

// -----------------------------------------------------------------------------
// Find a real available update candidate from installed-row repository annotation.
// -----------------------------------------------------------------------------
bool
find_update_pair_from_installed_annotation(PackageRow &installed_out, PackageRow &update_out)
{
  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  for (const auto &installed_row : installed_rows) {
    if (installed_row.repo_candidate_relation != PackageRepoCandidateRelation::NEWER ||
        installed_row.repo_candidate_nevra.empty()) {
      continue;
    }

    auto candidates = dnf_backend_get_available_package_rows_by_nevra(installed_row.repo_candidate_nevra);
    if (candidates.empty()) {
      continue;
    }

    update_out = candidates.front();
    InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(update_out);
    if (resolution.exact_installed || !resolution.has_installed_row ||
        resolution.state != PackageInstallState::UPGRADEABLE) {
      continue;
    }
    installed_out = resolution.installed_row;
    return true;
  }

  return false;
}

int
compare_backend_test_row_evr(const PackageRow &left, const PackageRow &right)
{
  int result = dnf_backend_compare_epoch_version_text(left.epoch, left.version, right.epoch, right.version);
  if (result != 0) {
    return result;
  }

  return dnf_backend_compare_rpm_version_text(left.release, right.release);
}

bool
find_parallel_installed_versions(PackageRow &older_out, PackageRow &newer_out)
{
  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);

  for (size_t i = 0; i < installed_rows.size(); ++i) {
    for (size_t j = i + 1; j < installed_rows.size(); ++j) {
      if (installed_rows[i].name_arch_key() != installed_rows[j].name_arch_key() ||
          installed_rows[i].nevra == installed_rows[j].nevra) {
        continue;
      }

      if (compare_backend_test_row_evr(installed_rows[i], installed_rows[j]) <= 0) {
        older_out = installed_rows[i];
        newer_out = installed_rows[j];
      } else {
        older_out = installed_rows[j];
        newer_out = installed_rows[i];
      }
      return true;
    }
  }

  return false;
}

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
// BaseManager safety & generation tests
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that rebuilding the package base advances the generation marker.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager generation increments on rebuild")
{
  auto &mgr = BaseManager::instance();

  auto before = mgr.current_generation();

  mgr.rebuild(); // metadata reload only, no system modification

  auto after = mgr.current_generation();

  REQUIRE(after > before);
}

// -----------------------------------------------------------------------------
// Verify that unloadable plugins cannot break local package data after an upgrade.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager reads package data without loading libdnf5 plugins")
{
  UnloadablePluginConfig config;
  ScopedEnvVar plugin_directory("LIBDNF_PLUGINS_CONFIG_DIR", config.directory.c_str());
  auto &mgr = BaseManager::instance();
  mgr.reset_for_tests();

  // Verify that this configuration really fails when libdnf5 plugins are enabled.
  {
    libdnf5::Base base;
    base.load_config();
    base.get_config().get_plugins_option().set(true);
    REQUIRE_THROWS_WITH(base.setup(), Catch::Matchers::ContainsSubstring("dnfui-unloadable.conf"));
  }

  SECTION("Search and repository rebuild")
  {
    auto rows = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, true), nullptr);
    REQUIRE_FALSE(rows.empty());
    REQUIRE_NOTHROW(mgr.rebuild());
  }

  SECTION("Installed package reads")
  {
    auto rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
    REQUIRE_FALSE(rows.empty());
  }

  SECTION("History Base")
  {
    REQUIRE(mgr.build_transaction_history_base() != nullptr);
  }

  SECTION("Changelog Base")
  {
    REQUIRE(mgr.build_changelog_base() != nullptr);
  }

  SECTION("Installed-only fallback")
  {
    ScopedEnvVar full_failure("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
    ScopedEnvVar cache_failure("DNFUI_TEST_FORCE_CACHEONLY_REPO_LOAD_FAILURE", "1");
    REQUIRE(mgr.rebuild() == BaseRepoState::INSTALLED_ONLY);
  }

  mgr.reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that dropping cached backend memory does not mark package data stale.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager cache drop keeps generation stable")
{
  auto &mgr = BaseManager::instance();

  REQUIRE_NOTHROW(mgr.acquire_read());
  const auto before = mgr.current_generation();

  mgr.drop_cached_base();

  REQUIRE(mgr.current_generation() == before);
}

// -----------------------------------------------------------------------------
// Verify that installed-state refresh reads cannot race with cached Base recreation.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager system-only refresh read drops cached Base before holding the lock")
{
  auto &mgr = BaseManager::instance();
  mgr.reset_for_tests();

  REQUIRE_NOTHROW(mgr.acquire_read());
  REQUIRE(mgr.has_cached_base_for_tests());

  {
    auto read = mgr.acquire_system_only_read_after_dropping_cached_base();
    REQUIRE(read.base != nullptr);
  }

  REQUIRE_FALSE(mgr.has_cached_base_for_tests());

  std::atomic<bool> reader_started { false };
  std::future<bool> normal_reader;
  {
    auto read = mgr.acquire_system_only_read_after_dropping_cached_base();
    REQUIRE(read.base != nullptr);

    normal_reader = std::async(std::launch::async, [&mgr, &reader_started]() {
      reader_started.store(true, std::memory_order_relaxed);
      auto normal_read = mgr.acquire_read();
      (void)normal_read;
      return true;
    });

    for (int i = 0; i < 50 && !reader_started.load(std::memory_order_relaxed); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(reader_started.load(std::memory_order_relaxed));
    REQUIRE(normal_reader.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
  }

  REQUIRE(normal_reader.get());
  REQUIRE(mgr.has_cached_base_for_tests());
  mgr.reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that startup still exposes installed packages when repo loading fails.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager falls back to installed-package-only initialization when repo-backed startup fails")
{
  reset_backend_globals();

  auto &mgr = BaseManager::instance();
  mgr.reset_for_tests();
  {
    ScopedEnvVar force_failure("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
    ScopedEnvVar force_cache_failure("DNFUI_TEST_FORCE_CACHEONLY_REPO_LOAD_FAILURE", "1");
    REQUIRE_NOTHROW(dnf_backend_refresh_installed_snapshot());
    REQUIRE(dnf_backend_installed_snapshot_size_for_tests() > 0);
  }
  mgr.reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that a failed repo refresh leaves installed package queries usable.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager rebuild keeps the app usable when repo-backed refresh fails")
{
  reset_backend_globals();

  auto &mgr = BaseManager::instance();
  mgr.reset_for_tests();
  REQUIRE_NOTHROW(mgr.acquire_read());
  const auto before = mgr.current_generation();

  {
    ScopedEnvVar force_failure("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
    ScopedEnvVar force_cache_failure("DNFUI_TEST_FORCE_CACHEONLY_REPO_LOAD_FAILURE", "1");
    REQUIRE_NOTHROW(mgr.rebuild());
  }

  REQUIRE(mgr.current_generation() > before);
  REQUIRE_NOTHROW(dnf_backend_refresh_installed_snapshot());
  REQUIRE(dnf_backend_installed_snapshot_size_for_tests() > 0);
  mgr.reset_for_tests();
}

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
TEST_CASE("Daemon upgrade metadata lookup does not publish installed state")
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
// Package info tests (read-only)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that invalid package identifiers return a friendly details message.
// -----------------------------------------------------------------------------
TEST_CASE("Invalid NEVRA returns friendly message")
{
  auto info = dnf_backend_get_package_info("invalid-0-0.x86_64");

  REQUIRE(info.find("No details found") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that package details include the expected user-visible fields.
// -----------------------------------------------------------------------------
TEST_CASE("Package info formatting contains expected fields")
{
  reset_backend_globals();

  auto results = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE(!results.empty());

  auto info = dnf_backend_get_package_info(results.front().nevra);

  REQUIRE(info.find("Name: ") != std::string::npos);
  REQUIRE(info.find("Package ID: ") != std::string::npos);
  REQUIRE(info.find("Version: ") != std::string::npos);
  REQUIRE(info.find("Release: ") != std::string::npos);
  REQUIRE(info.find("Arch: ") != std::string::npos);
  REQUIRE(info.find("Installed From: ") != std::string::npos);
  REQUIRE(info.find("Install Size: ") != std::string::npos);
  REQUIRE(info.find("Install Reason: ") != std::string::npos);
  REQUIRE(info.find("Summary:") != std::string::npos);
  REQUIRE(info.find("Description:") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that package details can display an explicit daemon update target.
// -----------------------------------------------------------------------------
TEST_CASE("Package info formatting can use explicit upgrade details")
{
  reset_backend_globals();

  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE(!installed_rows.empty());

  PackageRow installed_row = installed_rows.front();
  PackageRow daemon_target = installed_row;
  daemon_target.version = "999.0";
  daemon_target.release = "1.test";
  daemon_target.nevra = daemon_target.name + "-999.0-1.test." + daemon_target.arch;
  daemon_target.repo = "daemon-test";

  auto info = dnf_backend_get_package_info(installed_row.nevra, &daemon_target);

  REQUIRE(info.find("Package ID: " + installed_row.nevra) != std::string::npos);
  REQUIRE(info.find("Installed Version: " + installed_row.display_version()) != std::string::npos);
  REQUIRE(info.find("Upgradable Version: " + daemon_target.display_version()) != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that selected-version details describe the exact selected NEVRA.
// -----------------------------------------------------------------------------
TEST_CASE("Package info selected-version mode uses selected package metadata")
{
  reset_backend_globals();

  PackageRow installed_row;
  PackageRow update_row;
  if (!find_update_pair_from_installed_annotation(installed_row, update_row)) {
    SUCCEED("No installed package with a newer repo candidate in the test environment.");
    return;
  }

  auto info = dnf_backend_get_package_info(update_row.nevra, PackageDetailsContext::SELECTED_VERSION);

  REQUIRE(info.find("Package ID: " + update_row.nevra) != std::string::npos);
  REQUIRE(info.find("Package ID: " + installed_row.nevra) == std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that installed context keeps exact installed package metadata.
// -----------------------------------------------------------------------------
TEST_CASE("Package info installed context keeps exact installed version")
{
  reset_backend_globals();

  PackageRow older_row;
  PackageRow newer_row;
  if (!find_parallel_installed_versions(older_row, newer_row)) {
    SUCCEED("No parallel installed package versions in the test environment.");
    return;
  }

  auto info = dnf_backend_get_package_info(older_row.nevra);

  REQUIRE(info.find("Package ID: " + older_row.nevra) != std::string::npos);
  REQUIRE(info.find("Package ID: " + newer_row.nevra) == std::string::npos);
  REQUIRE(info.find("Upgradable Version: ") == std::string::npos);
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
// Dependency and file list tests (read-only)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that dependency details keep all expected section headings.
// -----------------------------------------------------------------------------
TEST_CASE("Dependency info contains expected section headers")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false), nullptr);
  REQUIRE(!results.empty());

  auto deps = dnf_backend_get_package_deps(results.front().nevra);

  REQUIRE(deps.find("Requires:") != std::string::npos);
  REQUIRE(deps.find("Required By:") != std::string::npos);
  REQUIRE(deps.find("Provides:") != std::string::npos);
  REQUIRE(deps.find("Conflicts:") != std::string::npos);
  REQUIRE(deps.find("Obsoletes:") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that dependency details for an update row describe the currently installed package.
// -----------------------------------------------------------------------------
TEST_CASE("Dependency info uses installed package for update rows")
{
  reset_backend_globals();

  PackageRow installed_row;
  PackageRow update_row;
  if (!find_update_pair_from_installed_annotation(installed_row, update_row)) {
    SUCCEED("No installed package with a newer repo candidate in the test environment.");
    return;
  }

  auto installed_deps = dnf_backend_get_package_deps(installed_row.nevra);
  auto upgrade_deps = dnf_backend_get_package_deps(update_row.nevra);

  REQUIRE(upgrade_deps == installed_deps);
  REQUIRE(upgrade_deps.find("(installed packages only)") == std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that file list lookup returns either content or a friendly state.
// -----------------------------------------------------------------------------
TEST_CASE("File list query is safe and returns valid state")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false), nullptr);
  REQUIRE(!results.empty());

  auto files = dnf_backend_get_installed_package_files(results.front().nevra);

  // Either it is installed (returns file list)
  // or not installed (returns friendly message).
  bool is_not_installed_msg = files.find("File list available only for installed packages.") != std::string::npos;

  bool has_content = !files.empty();
  if (!is_not_installed_msg) {
    REQUIRE(has_content);
  }
}

// -----------------------------------------------------------------------------
// Verify that the Files tab can use an available update row to show the files from the currently installed package.
// -----------------------------------------------------------------------------
TEST_CASE("File list query uses installed package for update rows")
{
  reset_backend_globals();

  PackageRow installed_row;
  PackageRow update_row;
  if (!find_update_pair_from_installed_annotation(installed_row, update_row)) {
    SUCCEED("No installed package with a newer repo candidate in the test environment.");
    return;
  }

  auto installed_files = dnf_backend_get_installed_package_files(installed_row.nevra, 1500);
  auto upgrade_files = dnf_backend_get_installed_package_files(update_row.nevra, 1500);

  REQUIRE(upgrade_files.find("File list available only for installed packages.") == std::string::npos);
  REQUIRE(upgrade_files == installed_files);
}

// -----------------------------------------------------------------------------
// Verify that selected-version file lookup does not use another installed version.
// -----------------------------------------------------------------------------
TEST_CASE("File list selected-version mode requires exact installed package")
{
  reset_backend_globals();

  PackageRow installed_row;
  PackageRow update_row;
  if (!find_update_pair_from_installed_annotation(installed_row, update_row)) {
    SUCCEED("No installed package with a newer repo candidate in the test environment.");
    return;
  }

  auto files = dnf_backend_get_installed_package_files(update_row.nevra, PackageDetailsContext::SELECTED_VERSION, 1500);

  REQUIRE(files.find("File list available only for installed packages.") != std::string::npos);
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
