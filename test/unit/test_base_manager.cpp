// -----------------------------------------------------------------------------
// test_base_manager.cpp
// BaseManager lifecycle and repository loading tests
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "dnf_backend/dnf_internal.hpp"
#include "test_utils.hpp"

#include <libdnf5/conf/const.hpp>
#include <libdnf5/repo/repo.hpp>
#include <libdnf5/repo/repo_sack.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
// A stopped rebuild must finish while another operation still owns the Base lock.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager rebuild can be cancelled while waiting for the Base lock")
{
  ProviderRepositoryFixture fixture;
  auto &manager = BaseManager::instance();
  auto cancel_requested = std::make_shared<std::atomic<bool>>(false);
  std::promise<void> rebuild_started;
  auto started = rebuild_started.get_future();
  std::future<bool> rebuild;
  std::future_status cancelled_status = std::future_status::timeout;
  libdnf5::Base *original_base = nullptr;
  BaseRepoState original_repo_state = BaseRepoState::LIVE_METADATA;
  const auto original_generation = manager.current_generation();

  {
    auto read = manager.acquire_read();
    original_base = &read.base;
    original_repo_state = read.repo_state;
    rebuild = std::async(std::launch::async, [&manager, &rebuild_started, cancel_requested]() {
      rebuild_started.set_value();
      try {
        manager.rebuild(BaseRefreshMode::NORMAL, cancel_requested);
      } catch (const BaseOperationCancelled &) {
        return true;
      }
      return false;
    });

    REQUIRE(started.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE(rebuild.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    cancel_requested->store(true, std::memory_order_relaxed);
    cancelled_status = rebuild.wait_for(std::chrono::seconds(2));
  }

  REQUIRE(cancelled_status == std::future_status::ready);
  REQUIRE(rebuild.get());
  REQUIRE(manager.current_generation() == original_generation);
  {
    auto read = manager.acquire_read();
    REQUIRE(&read.base == original_base);
    REQUIRE(read.repo_state == original_repo_state);
    libdnf5::rpm::PackageQuery query(read.base);
    query.filter_name("dnfui-filelists-fixture", libdnf5::sack::QueryCmp::EQ);
    REQUIRE(query.size() == 1);
  }
}

// -----------------------------------------------------------------------------
// A cancelled search must finish while another operation still holds the Base lock.
// -----------------------------------------------------------------------------
TEST_CASE_METHOD(ProviderRepositoryFixture, "Cancelled searches finish before the Base lock is released")
{
  auto &manager = BaseManager::instance();
  const uint64_t original_generation = manager.current_generation();
  DnfBackendSearchOptions options = backend_search_options(false, false);
  std::string term;

  SECTION("Package search")
  {
    term = "dnfui-filelists-fixture";
  }

  SECTION("File search")
  {
    options.mode = DnfBackendSearchMode::FILES;
    term = "file-2";
  }

  auto cancellable = std::unique_ptr<GCancellable, decltype(&g_object_unref)>(g_cancellable_new(), g_object_unref);
  GCancellable *cancel = cancellable.get();
  std::promise<void> worker_started;
  auto started = worker_started.get_future();
  std::future<std::vector<PackageRow>> result;
  std::future_status start_status;
  std::future_status blocked_status;
  std::future_status cancelled_status;

  {
    auto read = manager.acquire_read();
    result = std::async(std::launch::async, [&worker_started, &term, &options, cancel]() {
      worker_started.set_value();
      return dnf_backend_search_package_rows_interruptible(term, options, cancel);
    });

    start_status = started.wait_for(std::chrono::seconds(2));
    blocked_status = result.wait_for(std::chrono::milliseconds(50));
    g_cancellable_cancel(cancel);
    cancelled_status = result.wait_for(std::chrono::seconds(2));
  }

  const auto rows = result.get();

  REQUIRE(start_status == std::future_status::ready);
  REQUIRE(blocked_status == std::future_status::timeout);
  REQUIRE(cancelled_status == std::future_status::ready);
  REQUIRE(rows.empty());
  REQUIRE(dnf_backend_installed_snapshot_size_for_tests() == 0);
  REQUIRE(manager.current_generation() == original_generation);

  // A cancelled query must not prevent the next query from completing.
  const auto retry_rows = dnf_backend_search_package_rows_interruptible(term, options, nullptr);
  REQUIRE_FALSE(retry_rows.empty());
}

// -----------------------------------------------------------------------------
// Verify that unloadable plugins cannot break local package data after an upgrade.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager reads package data without loading libdnf5 plugins")
{
  ProviderRepositoryFixture fixture;
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
// Capability lookup must still work for installed packages without repository metadata.
// -----------------------------------------------------------------------------
TEST_CASE_METHOD(ProviderRepositoryFixture, "Provides search works with installed packages offline")
{
  ScopedEnvVar full_failure("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
  ScopedEnvVar cache_failure("DNFUI_TEST_FORCE_CACHEONLY_REPO_LOAD_FAILURE", "1");
  auto &manager = BaseManager::instance();
  auto options = backend_search_options(false, false);
  options.mode = DnfBackendSearchMode::PROVIDES;

  BaseRepoState search_state = BaseRepoState::LIVE_METADATA;
  auto installed = dnf_backend_search_package_rows_interruptible("bash", options, nullptr, &search_state);
  REQUIRE(manager.current_repo_state() == BaseRepoState::INSTALLED_ONLY);
  REQUIRE(search_state == BaseRepoState::INSTALLED_ONLY);
  REQUIRE_FALSE(installed.empty());
  REQUIRE(installed.front().name == "bash");

  auto unavailable = dnf_backend_search_package_rows_interruptible("dnfui-filelists-fixture", options, nullptr);
  REQUIRE(unavailable.empty());
}

// -----------------------------------------------------------------------------
// Ordinary package queries must retain their metadata requirements before and after file searches.
// -----------------------------------------------------------------------------
TEST_CASE("File searches keep filelists separate from normal package queries")
{
  TemporaryTestDirectory repo_dir;
  TemporaryTestDirectory cache_dir;
  const std::string repo_url = std::string("file://") + DNFUI_TEST_PROVIDER_REPO;
  ScopedEnvVar isolated_repos("DNFUI_TEST_REPOS_DIR", repo_dir.path.c_str());
  ScopedEnvVar isolated_cache("DNFUI_TEST_CACHE_DIR", cache_dir.path.c_str());
  ScopedEnvVar fixture_repo("DNFUI_TEST_PROVIDER_REPO_URL", repo_url.c_str());

  libdnf5::Base configured_base;
  configured_base.load_config();
  const auto configured_types = configured_base.get_config().get_optional_metadata_types_option().get_value();
  auto &manager = BaseManager::instance();
  manager.reset_for_tests();
  libdnf5::Base *normal_base = nullptr;
  {
    auto read = manager.acquire_read();
    normal_base = &read.base;
    libdnf5::repo::RepoQuery repositories(read.base);
    repositories.filter_type(libdnf5::repo::Repo::Type::AVAILABLE);
    std::set<std::string> repository_ids;
    for (auto repo : repositories) {
      repository_ids.insert(repo->get_id());
    }
    REQUIRE(repository_ids == std::set<std::string> { "dnfui-filelists-cache-test" });
    REQUIRE(read.base.get_config().get_optional_metadata_types_option().get_value() == configured_types);
  }

  auto rows = dnf_backend_search_package_rows_interruptible(
      "dnfui-filelists-fixture", backend_search_options(false, true), nullptr);
  REQUIRE_FALSE(rows.empty());
  if (configured_types.count(libdnf5::METADATA_TYPE_FILELISTS) == 0 &&
      configured_types.count(libdnf5::METADATA_TYPE_ALL) == 0) {
    for (const auto &entry : std::filesystem::recursive_directory_iterator(cache_dir.path)) {
      if (entry.path().parent_path().filename() == "repodata") {
        REQUIRE(entry.path().filename().string().find("filelists") == std::string::npos);
      }
    }
  }

  const uint64_t generation = manager.current_generation();
  auto capability_options = backend_search_options(false, false);
  capability_options.mode = DnfBackendSearchMode::PROVIDES;
  auto capability_rows =
      dnf_backend_search_package_rows_interruptible("dnfui-filelists-fixture >= 1", capability_options, nullptr);
  REQUIRE(capability_rows.size() == 1);
  {
    auto read = manager.acquire_read();
    REQUIRE(&read.base == normal_base);
    REQUIRE(read.base.get_config().get_optional_metadata_types_option().get_value() == configured_types);
  }

  auto file_options = backend_search_options(false, false);
  file_options.mode = DnfBackendSearchMode::FILES;
  rows = dnf_backend_search_package_rows_interruptible("file-2", file_options, nullptr);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].name == "dnfui-filelists-fixture");
  REQUIRE(manager.current_generation() == generation);
  {
    ScopedEnvVar offline("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
    BaseRepoState search_state = BaseRepoState::LIVE_METADATA;
    rows = dnf_backend_search_package_rows_interruptible("file-2", file_options, nullptr, &search_state);
    REQUIRE(rows.size() == 1);
    REQUIRE(search_state == BaseRepoState::CACHED_METADATA);
  }

  GCancellable *cancelled = g_cancellable_new();
  g_cancellable_cancel(cancelled);
  rows = dnf_backend_search_package_rows_interruptible("file-2", file_options, cancelled);
  g_object_unref(cancelled);
  REQUIRE(rows.empty());
  {
    auto read = manager.acquire_read();
    REQUIRE(&read.base == normal_base);
    REQUIRE(read.base.get_config().get_optional_metadata_types_option().get_value() == configured_types);
  }

  manager.reset_for_tests();
  rows = dnf_backend_search_package_rows_interruptible("file-2", file_options, nullptr);
  REQUIRE(rows.size() == 1);
  REQUIRE_FALSE(manager.has_cached_base_for_tests());
}

// -----------------------------------------------------------------------------
// File searches must reuse their own cached metadata before trying the system cache.
// -----------------------------------------------------------------------------
TEST_CASE("File searches use separate configured and system metadata caches")
{
  TemporaryTestDirectory repo_dir;
  TemporaryTestDirectory cache_dir;
  TemporaryTestDirectory system_cache_dir;
  const std::string repo_url = std::string("file://") + DNFUI_TEST_PROVIDER_REPO;
  ScopedEnvVar isolated_repos("DNFUI_TEST_REPOS_DIR", repo_dir.path.c_str());
  ScopedEnvVar isolated_cache("DNFUI_TEST_CACHE_DIR", cache_dir.path.c_str());
  ScopedEnvVar isolated_system_cache("DNFUI_TEST_SYSTEM_CACHE_DIR", system_cache_dir.path.c_str());
  ScopedEnvVar fixture_repo("DNFUI_TEST_PROVIDER_REPO_URL", repo_url.c_str());
  auto &manager = BaseManager::instance();
  manager.reset_for_tests();
  auto options = backend_search_options(false, false);
  options.mode = DnfBackendSearchMode::FILES;

  SECTION("A previous file search supplies the offline metadata")
  {
    auto rows = dnf_backend_search_package_rows_interruptible("file-2", options, nullptr);
    REQUIRE(rows.size() == 1);
    REQUIRE(std::filesystem::is_empty(system_cache_dir.path));

    ScopedEnvVar offline("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
    rows = dnf_backend_search_package_rows_interruptible("file-2", options, nullptr);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].name == "dnfui-filelists-fixture");
    REQUIRE(std::filesystem::is_empty(system_cache_dir.path));
  }

  SECTION("System metadata remains available when the configured cache is empty")
  {
    {
      libdnf5::Base base;
      base.load_config();
      base.get_config().get_plugins_option().set(false);
      base.get_config().get_cachedir_option().set(system_cache_dir.path.string());
      base_manager_enable_filelists_metadata(base);
      base.setup();
      auto repo_sack = base.get_repo_sack();
      auto repo = repo_sack->create_repo("dnfui-filelists-cache-test");
      repo->get_config().get_baseurl_option().set(std::vector<std::string> { repo_url });
      repo_sack->load_repos(libdnf5::repo::Repo::Type::AVAILABLE);
    }

    REQUIRE(std::filesystem::is_empty(cache_dir.path));
    ScopedEnvVar offline("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
    const auto rows = dnf_backend_search_package_rows_interruptible("file-2", options, nullptr);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].name == "dnfui-filelists-fixture");
    {
      auto read = manager.acquire_read();
      REQUIRE(read.base.get_config().get_cachedir_option().get_value() == system_cache_dir.path.string());
    }
  }

  manager.reset_for_tests();
}

// -----------------------------------------------------------------------------
// Normal package queries must use whichever metadata cache is available offline.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager uses configured and system metadata caches for offline queries")
{
  ProviderRepositoryFixture fixture;
  auto &manager = BaseManager::instance();
  std::filesystem::path expected_cache;

  SECTION("Configured cache contains the repository metadata")
  {
    {
      auto read = manager.acquire_read();
      libdnf5::rpm::PackageQuery query(read.base);
      query.filter_name("dnfui-filelists-fixture", libdnf5::sack::QueryCmp::EQ);
      REQUIRE(query.size() == 1);
      libdnf5::repo::RepoQuery repositories(read.base);
      repositories.filter_type(libdnf5::repo::Repo::Type::AVAILABLE);
      REQUIRE_FALSE(repositories.empty());
      REQUIRE_FALSE((*repositories.begin())->get_metadata_path("primary").empty());
    }
    REQUIRE(std::filesystem::is_empty(fixture.system_cache_dir.path));
    expected_cache = fixture.cache_dir.path;
  }

  SECTION("System cache contains the repository metadata")
  {
    {
      libdnf5::Base base;
      base.load_config();
      base.get_config().get_plugins_option().set(false);
      base.get_config().get_cachedir_option().set(fixture.system_cache_dir.path.string());
      base.setup();
      auto repo_sack = base.get_repo_sack();
      auto repo = repo_sack->create_repo("dnfui-filelists-cache-test");
      repo->get_config().get_baseurl_option().set(std::vector<std::string> { fixture.repo_url });
      repo_sack->load_repos(libdnf5::repo::Repo::Type::AVAILABLE);
    }
    REQUIRE(std::filesystem::is_empty(fixture.cache_dir.path));
    expected_cache = fixture.system_cache_dir.path;
  }

  manager.reset_for_tests();
  ScopedEnvVar offline("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
  REQUIRE(manager.rebuild() == BaseRepoState::CACHED_METADATA);
  {
    auto read = manager.acquire_read();
    REQUIRE(read.base.get_config().get_cachedir_option().get_value() == expected_cache.string());
    libdnf5::rpm::PackageQuery query(read.base);
    query.filter_name("dnfui-filelists-fixture", libdnf5::sack::QueryCmp::EQ);
    REQUIRE(query.size() == 1);
  }
}

// -----------------------------------------------------------------------------
// A partial configured cache must not hide repositories present in the system cache.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager prefers the system cache when the configured cache is partial")
{
  TemporaryTestDirectory repo_dir;
  TemporaryTestDirectory cache_dir;
  TemporaryTestDirectory system_cache_dir;
  const std::string repo_url = std::string("file://") + DNFUI_TEST_PROVIDER_REPO;

  std::ofstream repo_config(repo_dir.path / "dnfui-cache-test.repo");
  for (const char *repo_id : { "dnfui-cache-a", "dnfui-cache-b" }) {
    repo_config << '[' << repo_id << "]\nname=" << repo_id << "\nbaseurl=" << repo_url
                << "\nenabled=1\nskip_if_unavailable=1\n";
  }
  repo_config.close();
  REQUIRE(repo_config.good());

  auto populate_cache = [&repo_url](const std::filesystem::path &directory, bool include_second_repo) {
    libdnf5::Base base;
    base.load_config();
    base.get_config().get_plugins_option().set(false);
    base.get_config().get_cachedir_option().set(directory.string());
    base.setup();
    auto repo_sack = base.get_repo_sack();
    auto first_repo = repo_sack->create_repo("dnfui-cache-a");
    first_repo->get_config().get_baseurl_option().set(std::vector<std::string> { repo_url });
    if (include_second_repo) {
      auto second_repo = repo_sack->create_repo("dnfui-cache-b");
      second_repo->get_config().get_baseurl_option().set(std::vector<std::string> { repo_url });
    }
    repo_sack->load_repos(libdnf5::repo::Repo::Type::AVAILABLE);
  };
  populate_cache(cache_dir.path, false);
  populate_cache(system_cache_dir.path, true);

  ScopedEnvVar isolated_repos("DNFUI_TEST_REPOS_DIR", repo_dir.path.c_str());
  ScopedEnvVar isolated_cache("DNFUI_TEST_CACHE_DIR", cache_dir.path.c_str());
  ScopedEnvVar isolated_system_cache("DNFUI_TEST_SYSTEM_CACHE_DIR", system_cache_dir.path.c_str());
  ScopedEnvVar offline("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
  auto &manager = BaseManager::instance();
  manager.reset_for_tests();

  REQUIRE(manager.rebuild() == BaseRepoState::CACHED_METADATA);
  {
    auto read = manager.acquire_read();
    REQUIRE(read.base.get_config().get_cachedir_option().get_value() == system_cache_dir.path.string());
    libdnf5::repo::RepoQuery repositories(read.base);
    repositories.filter_enabled(true);
    repositories.filter_type(libdnf5::repo::Repo::Type::AVAILABLE);
    std::set<std::string> loaded_repositories;
    for (auto repo : repositories) {
      if (!repo->get_metadata_path("primary").empty()) {
        loaded_repositories.insert(repo->get_id());
      }
    }
    REQUIRE(loaded_repositories == std::set<std::string> { "dnfui-cache-a", "dnfui-cache-b" });
  }
  manager.reset_for_tests();
}

// -----------------------------------------------------------------------------
// Empty metadata caches must leave only the local installed package database.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager uses installed-only mode when metadata caches are empty")
{
  ProviderRepositoryFixture fixture;
  ScopedEnvVar offline("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
  auto &manager = BaseManager::instance();

  REQUIRE(std::filesystem::is_empty(fixture.cache_dir.path));
  REQUIRE(std::filesystem::is_empty(fixture.system_cache_dir.path));
  REQUIRE(manager.rebuild() == BaseRepoState::INSTALLED_ONLY);
  auto read = manager.acquire_read();
  REQUIRE(read.repo_state == BaseRepoState::INSTALLED_ONLY);
  libdnf5::rpm::PackageQuery query(read.base);
  query.filter_installed();
  REQUIRE_FALSE(query.empty());
}

// -----------------------------------------------------------------------------
// Cached primary metadata must survive a missing optional filelists cache.
// File search must report missing metadata rather than returning incomplete results.
// -----------------------------------------------------------------------------
TEST_CASE("BaseManager preserves offline cached packages without cached filelists")
{
  TemporaryTestDirectory repo_dir;
  TemporaryTestDirectory cache_dir;
  const std::string repo_url = std::string("file://") + DNFUI_TEST_PROVIDER_REPO;

  {
    // Prime only primary metadata for the fixture repository.
    libdnf5::Base base;
    base.load_config();
    base.get_config().get_plugins_option().set(false);
    base.get_config().get_cachedir_option().set(cache_dir.path.string());
    base.get_config().get_optional_metadata_types_option().set(std::set<std::string> {});
    base.setup();
    auto sack = base.get_repo_sack();
    auto repo = sack->create_repo("dnfui-filelists-cache-test");
    repo->get_config().get_baseurl_option().set(std::vector<std::string> { repo_url });
    sack->load_repos(libdnf5::repo::Repo::Type::AVAILABLE);

    libdnf5::rpm::PackageQuery query(base);
    query.filter_name("dnfui-filelists-fixture", libdnf5::sack::QueryCmp::EQ);
    REQUIRE(query.size() == 1);
  }

  auto &manager = BaseManager::instance();
  manager.reset_for_tests();
  {
    ScopedEnvVar isolated_repos("DNFUI_TEST_REPOS_DIR", repo_dir.path.c_str());
    ScopedEnvVar isolated_cache("DNFUI_TEST_CACHE_DIR", cache_dir.path.c_str());
    ScopedEnvVar fixture_repo("DNFUI_TEST_PROVIDER_REPO_URL", repo_url.c_str());
    ScopedEnvVar offline("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");

    REQUIRE(manager.rebuild() == BaseRepoState::CACHED_METADATA);

    auto rows = dnf_backend_search_package_rows_interruptible(
        "dnfui-filelists-fixture", backend_search_options(false, true), nullptr);
    REQUIRE_FALSE(rows.empty());

    auto capability_options = backend_search_options(false, false);
    capability_options.mode = DnfBackendSearchMode::PROVIDES;
    BaseRepoState search_state = BaseRepoState::LIVE_METADATA;
    auto capability_rows = dnf_backend_search_package_rows_interruptible(
        "dnfui-filelists-fixture >= 1", capability_options, nullptr, &search_state);
    REQUIRE(capability_rows.size() == 1);
    REQUIRE(capability_rows[0].name == "dnfui-filelists-fixture");
    REQUIRE(search_state == BaseRepoState::CACHED_METADATA);

    auto file_options = backend_search_options(false, false);
    file_options.mode = DnfBackendSearchMode::FILES;
    REQUIRE_THROWS_WITH(dnf_backend_search_package_rows_interruptible("file-2", file_options, nullptr),
                        Catch::Matchers::ContainsSubstring("file lists"));
  }
  manager.reset_for_tests();
}

// -----------------------------------------------------------------------------
// Repositories may omit optional filelists even when the caller requested them.
// File searches must report incomplete metadata rather than a false negative.
// -----------------------------------------------------------------------------
TEST_CASE("File searches reject repositories that omit filelists metadata")
{
  TemporaryTestDirectory repo_dir;
  TemporaryTestDirectory cache_dir;
  const auto fixture =
      std::filesystem::path(DNFUI_TEST_PROVIDER_REPO).parent_path() / "provider_repo_without_filelists";
  const std::string repo_url = std::string("file://") + fixture.string();
  ScopedEnvVar isolated_repos("DNFUI_TEST_REPOS_DIR", repo_dir.path.c_str());
  ScopedEnvVar isolated_cache("DNFUI_TEST_CACHE_DIR", cache_dir.path.c_str());
  ScopedEnvVar fixture_repo("DNFUI_TEST_PROVIDER_REPO_URL", repo_url.c_str());
  auto &manager = BaseManager::instance();
  manager.reset_for_tests();

  auto options = backend_search_options(false, false);
  options.mode = DnfBackendSearchMode::FILES;
  REQUIRE_THROWS_WITH(dnf_backend_search_package_rows_interruptible("/usr/share/dnfui-review/file-2", options, nullptr),
                      Catch::Matchers::ContainsSubstring("file lists"));
  REQUIRE_FALSE(manager.has_cached_base_for_tests());

  const auto rows = dnf_backend_search_package_rows_interruptible(
      "dnfui-filelists-fixture", backend_search_options(false, true), nullptr);
  REQUIRE(rows.size() == 1);
  manager.reset_for_tests();
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
