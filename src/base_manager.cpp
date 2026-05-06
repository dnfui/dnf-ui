// -----------------------------------------------------------------------------
// src/base_manager.cpp
// Shared libdnf5 Base manager
// Creates, reuses, and rebuilds Base objects while protecting access from
// worker threads.
// -----------------------------------------------------------------------------
#include "base_manager.hpp"
#include "debug_trace.hpp"

#include <libdnf5/conf/const.hpp>
#include <libdnf5/repo/repo.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {

enum class RepoLoadMode {
  FULL,
  CACHE_ONLY_METADATA,
  SYSTEM_ONLY,
};

struct BuiltBase {
  std::shared_ptr<libdnf5::Base> base;
  BaseRepoState repo_state = BaseRepoState::LIVE_METADATA;
};

// -----------------------------------------------------------------------------
// Build one fully configured Base before any repo metadata is loaded.
// -----------------------------------------------------------------------------
static std::shared_ptr<libdnf5::Base>
create_configured_base(RepoLoadMode mode, bool load_changelog_metadata)
{
  DNFUI_TRACE("BaseManager initialize start");

  auto base = std::make_shared<libdnf5::Base>();
  DNFUI_TRACE("BaseManager load config start");
  base->load_config();
  DNFUI_TRACE("BaseManager load config done");

  if (mode == RepoLoadMode::CACHE_ONLY_METADATA) {
    // When live repo refresh is not available, keep repo-backed queries working
    // from cached metadata instead of dropping immediately to installed-only mode.
    base->get_config().get_cacheonly_option().set("metadata");
    base->get_config().get_cachedir_option().set(base->get_config().get_system_cachedir_option().get_value());
  }

  if (load_changelog_metadata && mode != RepoLoadMode::SYSTEM_ONLY) {
    // Available-package changelog lookups need repo "other" metadata.
    base->get_config().get_optional_metadata_types_option().add_item(libdnf5::Option::Priority::RUNTIME,
                                                                     libdnf5::METADATA_TYPE_OTHER);
  }
  DNFUI_TRACE("BaseManager setup start");
  base->setup();
  DNFUI_TRACE("BaseManager setup done");

  auto repo_sack = base->get_repo_sack();
  DNFUI_TRACE("BaseManager create repos start");
  repo_sack->create_repos_from_system_configuration();
  DNFUI_TRACE("BaseManager create repos done");

  return base;
}

// -----------------------------------------------------------------------------
// Load repository data for one already configured Base. Startup fallback should
// only trigger when this step fails for the full repo set.
// -----------------------------------------------------------------------------
static void
load_repo_data(libdnf5::Base &base, RepoLoadMode mode)
{
  auto repo_sack = base.get_repo_sack();

#ifdef DNFUI_BUILD_TESTS
  if (mode == RepoLoadMode::FULL) {
    const char *force_failure = std::getenv("DNFUI_TEST_FORCE_FULL_REPO_LOAD_FAILURE");
    if (force_failure && std::string(force_failure) == "1") {
      throw std::runtime_error("forced full repo load failure");
    }
  } else if (mode == RepoLoadMode::CACHE_ONLY_METADATA) {
    const char *force_failure = std::getenv("DNFUI_TEST_FORCE_CACHEONLY_REPO_LOAD_FAILURE");
    if (force_failure && std::string(force_failure) == "1") {
      throw std::runtime_error("forced cache-only repo load failure");
    }
  }
#endif

  DNFUI_TRACE("BaseManager load repos start");
  if (mode == RepoLoadMode::SYSTEM_ONLY) {
    repo_sack->load_repos(libdnf5::repo::Repo::Type::SYSTEM);
  } else {
    repo_sack->load_repos();
  }
  DNFUI_TRACE("BaseManager load repos done");
}

} // namespace

// -----------------------------------------------------------------------------
// Build one Base and load repository data for the requested mode.
// -----------------------------------------------------------------------------
static BuiltBase
build_base_for_mode(RepoLoadMode mode, bool load_changelog_metadata)
{
  BuiltBase result;
  result.base = create_configured_base(mode, load_changelog_metadata);
  load_repo_data(*result.base, mode);
  if (mode == RepoLoadMode::CACHE_ONLY_METADATA) {
    result.repo_state = BaseRepoState::CACHED_METADATA;
  } else if (mode == RepoLoadMode::SYSTEM_ONLY) {
    result.repo_state = BaseRepoState::INSTALLED_ONLY;
  }
  DNFUI_TRACE("BaseManager initialize done");
  return result;
}

// -----------------------------------------------------------------------------
// Try the normal live repo load first, then cached metadata, then finally
// installed-package-only mode so the app stays usable when the network is down.
// -----------------------------------------------------------------------------
static BuiltBase
build_base_with_offline_fallback(bool load_changelog_metadata = false)
{
  try {
    return build_base_for_mode(RepoLoadMode::FULL, load_changelog_metadata);
  } catch (const std::exception &repo_error) {
    std::cerr << "Warning: repo load failed: " << repo_error.what() << std::endl;
    DNFUI_TRACE("BaseManager load repos failed: %s", repo_error.what());
    std::cerr << "Warning: live repo load failed, retrying from cached metadata: " << repo_error.what() << std::endl;
    DNFUI_TRACE("BaseManager live repo load failed, trying cached metadata fallback: %s", repo_error.what());

    try {
      return build_base_for_mode(RepoLoadMode::CACHE_ONLY_METADATA, load_changelog_metadata);
    } catch (const std::exception &cache_error) {
      std::cerr << "Warning: cached repo metadata load failed: " << cache_error.what() << std::endl;
      DNFUI_TRACE("BaseManager cached repo load failed, trying system-only fallback: %s", cache_error.what());

      try {
        return build_base_for_mode(RepoLoadMode::SYSTEM_ONLY, load_changelog_metadata);
      } catch (const std::exception &fallback_error) {
        throw std::runtime_error(
            "DNF backend initialization failed after repo load error: " + std::string(repo_error.what()) +
            "; cached metadata fallback failed: " + cache_error.what() +
            "; system-only fallback failed: " + fallback_error.what());
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Ask glibc to return free heap pages after dropping large libdnf metadata.
// -----------------------------------------------------------------------------
static void
trim_free_heap()
{
#ifdef __GLIBC__
  malloc_trim(0);
#endif
}

// -----------------------------------------------------------------------------
// Keep one temporary Base alive while its serialized lock guard is held.
// -----------------------------------------------------------------------------
TemporaryBaseRead::TemporaryBaseRead(std::shared_ptr<libdnf5::Base> &&base_ptr, BaseGuard &&base_guard)
    : base(std::move(base_ptr))
    , guard(std::move(base_guard))
{
}

TemporaryBaseRead::~TemporaryBaseRead()
{
  if (!base) {
    return;
  }
  base.reset();
  trim_free_heap();
}

// -----------------------------------------------------------------------------
// Return the single BaseManager used by the process.
// -----------------------------------------------------------------------------
BaseManager &
BaseManager::instance()
{
  static BaseManager mgr;
  return mgr;
}

// -----------------------------------------------------------------------------
// Return the repository state of the current Base.
// -----------------------------------------------------------------------------
BaseRepoState
BaseManager::current_repo_state() const
{
  std::shared_lock<std::shared_mutex> shared(base_mutex);
  return repo_state;
}

// -----------------------------------------------------------------------------
// Return serialized read access to the current Base.
// -----------------------------------------------------------------------------
BaseRead
BaseManager::acquire_read()
{
  // Keep the lock exclusive for the whole libdnf Base operation. PackageQuery
  // work can touch shared Base internals even when the caller only reads data.
  std::unique_lock<std::shared_mutex> lock(base_mutex);
  if (!base_ptr) {
    ensure_base_initialized();
  }
  if (!base_ptr) {
    // Never return a null Base reference.
    throw std::runtime_error("DNF backend not initialized (Base is null).");
  }

  return { *base_ptr, BaseGuard(std::move(lock)), generation.load(std::memory_order_relaxed) };
}

// -----------------------------------------------------------------------------
// Return serialized access to a temporary Base that loads changelog metadata.
// -----------------------------------------------------------------------------
TemporaryBaseRead
BaseManager::acquire_changelog_read()
{
  std::unique_lock<std::shared_mutex> lock(base_mutex);
  BuiltBase built = build_base_with_offline_fallback(true);
  if (!built.base) {
    throw std::runtime_error("Changelog backend initialization failed (Base is null).");
  }

  return TemporaryBaseRead(std::move(built.base), BaseGuard(std::move(lock)));
}

// -----------------------------------------------------------------------------
// Return write access to the current Base.
// -----------------------------------------------------------------------------
std::pair<libdnf5::Base &, BaseWriteGuard>
BaseManager::acquire_write()
{
  std::unique_lock<std::shared_mutex> write_lock(base_mutex);
  if (!base_ptr) {
    ensure_base_initialized();
  }
  if (!base_ptr) {
    // Never return a null Base reference.
    throw std::runtime_error("DNF backend not initialized (Base is null).");
  }

  return { *base_ptr, BaseWriteGuard(std::move(write_lock)) };
}

// -----------------------------------------------------------------------------
// Rebuild the cached Base after repository refresh or transaction work.
// -----------------------------------------------------------------------------
BaseRepoState
BaseManager::rebuild()
{
  // Allow only one Base rebuild at a time.
  std::unique_lock lock(base_mutex);

  // Build the replacement first so a refresh failure does not discard the last
  // usable Base. Offline fallback keeps the UI query paths working from cached
  // metadata or, as a last resort, from the local rpmdb only.
  BuiltBase rebuilt = build_base_with_offline_fallback();
  if (!rebuilt.base) {
    throw std::runtime_error("Repository rebuild failed (Base is null).");
  }

  base_ptr = rebuilt.base;
  repo_state = rebuilt.repo_state;

  // Publish the generation change only after the new Base is ready so readers
  // never drop their cached results without a replacement snapshot to use.
  generation.fetch_add(1, std::memory_order_relaxed);
  return rebuilt.repo_state;
}

// -----------------------------------------------------------------------------
// Force a local-only rebuild that loads only the installed-package view from
// the rpmdb. This keeps remove-only transaction flows independent of remote
// repository availability.
// -----------------------------------------------------------------------------
void
BaseManager::rebuild_system_only()
{
  std::unique_lock lock(base_mutex);

  auto rebuilt_base = build_initialized_system_only_base();
  if (!rebuilt_base) {
    throw std::runtime_error("System-only repository rebuild failed (Base is null).");
  }

  base_ptr = rebuilt_base;
  repo_state = BaseRepoState::INSTALLED_ONLY;
  generation.fetch_add(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// Drop the cached Base so memory-heavy metadata does not stay resident after
// short-lived backend work.
// -----------------------------------------------------------------------------
void
BaseManager::drop_cached_base()
{
  {
    std::unique_lock<std::shared_mutex> lock(base_mutex);
    if (!base_ptr) {
      return;
    }
    base_ptr.reset();
  }

  trim_free_heap();
}

// -----------------------------------------------------------------------------
// Ensure one local-only Base exists without attempting a live repo refresh.
// Remove-only transaction flows use this when the shared Base has not been
// initialized yet and no repo metadata is required.
// -----------------------------------------------------------------------------
void
BaseManager::ensure_system_only_initialized_if_needed()
{
  std::unique_lock<std::shared_mutex> unique(base_mutex);
  if (!base_ptr) {
    base_ptr = build_initialized_system_only_base();
    repo_state = BaseRepoState::INSTALLED_ONLY;
  }
}

// -----------------------------------------------------------------------------
// Build a Base that reads only the local installed package database.
// -----------------------------------------------------------------------------
std::shared_ptr<libdnf5::Base>
BaseManager::build_initialized_system_only_base()
{
  return build_base_for_mode(RepoLoadMode::SYSTEM_ONLY, false).base;
}

// -----------------------------------------------------------------------------
// Create the shared Base while the caller holds the write lock.
// -----------------------------------------------------------------------------
void
BaseManager::ensure_base_initialized()
{
  if (!base_ptr) {
    BuiltBase built = build_base_with_offline_fallback();
    base_ptr = built.base;
    repo_state = built.repo_state;
  }
}

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Clear cached Base state between tests.
// -----------------------------------------------------------------------------
void
BaseManager::reset_for_tests()
{
  std::unique_lock<std::shared_mutex> unique(base_mutex);
  base_ptr.reset();
  generation.store(0, std::memory_order_relaxed);
}
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
