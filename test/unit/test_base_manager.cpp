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
// Ordinary package queries must retain their metadata requirements before and after provider searches.
// -----------------------------------------------------------------------------
TEST_CASE("Provider searches keep filelists separate from normal package queries")
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
  auto provider_options = backend_search_options(false, false);
  provider_options.find_provider = true;
  rows = dnf_backend_search_package_rows_interruptible("file-2", provider_options, nullptr);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].name == "dnfui-filelists-fixture");
  REQUIRE(manager.current_generation() == generation);
  {
    ScopedEnvVar offline("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE", "1");
    rows = dnf_backend_search_package_rows_interruptible("file-2", provider_options, nullptr);
    REQUIRE(rows.size() == 1);
  }

  GCancellable *cancelled = g_cancellable_new();
  g_cancellable_cancel(cancelled);
  rows = dnf_backend_search_package_rows_interruptible("file-2", provider_options, cancelled);
  g_object_unref(cancelled);
  REQUIRE(rows.empty());
  {
    auto read = manager.acquire_read();
    REQUIRE(&read.base == normal_base);
    REQUIRE(read.base.get_config().get_optional_metadata_types_option().get_value() == configured_types);
  }

  manager.reset_for_tests();
  rows = dnf_backend_search_package_rows_interruptible("file-2", provider_options, nullptr);
  REQUIRE(rows.size() == 1);
  REQUIRE_FALSE(manager.has_cached_base_for_tests());
}

// -----------------------------------------------------------------------------
// Provider searches must reuse their own cached metadata before trying the system cache.
// -----------------------------------------------------------------------------
TEST_CASE("Provider searches use separate configured and system metadata caches")
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
  options.find_provider = true;

  SECTION("A previous provider search supplies the offline metadata")
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
// Cached primary metadata must survive a missing optional filelists cache.
// Provides must report that file search is incomplete rather than returning empty results.
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

    auto provider_options = backend_search_options(false, false);
    provider_options.find_provider = true;
    REQUIRE_THROWS_WITH(dnf_backend_search_package_rows_interruptible("file-2", provider_options, nullptr),
                        Catch::Matchers::ContainsSubstring("file lists"));
  }
  manager.reset_for_tests();
}

// -----------------------------------------------------------------------------
// Repositories may omit optional filelists even when the caller requested them.
// Provider searches must report incomplete metadata rather than a false negative.
// -----------------------------------------------------------------------------
TEST_CASE("Provider searches reject repositories that omit filelists metadata")
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
  options.find_provider = true;
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
