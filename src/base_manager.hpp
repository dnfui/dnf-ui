// -----------------------------------------------------------------------------
// src/base_manager.hpp
// -----------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <libdnf5/base/base.hpp>

// -----------------------------------------------------------------------------
// Lock guard helpers for thread-safe Base access
// These small classes automatically hold a shared (read) or unique (write)
// lock on the BaseManager mutex for the duration of a backend operation.
// -----------------------------------------------------------------------------
class BaseGuard {
  public:
  // -----------------------------------------------------------------------------
  // Take ownership of a shared BaseManager lock.
  // -----------------------------------------------------------------------------
  explicit BaseGuard(std::shared_lock<std::shared_mutex> &&l)
      : lock(std::move(l))
  {
  }

  private:
  std::shared_lock<std::shared_mutex> lock;
};

class BaseWriteGuard {
  public:
  // -----------------------------------------------------------------------------
  // Take ownership of a unique BaseManager lock.
  // -----------------------------------------------------------------------------
  explicit BaseWriteGuard(std::unique_lock<std::shared_mutex> &&l)
      : lock(std::move(l))
  {
  }

  private:
  std::unique_lock<std::shared_mutex> lock;
};

// -----------------------------------------------------------------------------
// Read access bundle with Base reference, lock guard, and generation snapshot.
// -----------------------------------------------------------------------------
struct BaseRead {
  libdnf5::Base &base;
  BaseGuard guard;
  uint64_t generation;
};

// Result of one repository rebuild attempt.
// LIVE_METADATA means a normal online refresh succeeded.
// CACHED_METADATA means live refresh failed but cached repo metadata loaded.
// INSTALLED_ONLY means both repo-backed paths failed and only the local rpmdb remains.
enum class BaseRepoState {
  LIVE_METADATA,
  CACHED_METADATA,
  INSTALLED_ONLY,
};

// -----------------------------------------------------------------------------
// Shared access point for the cached libdnf5 Base instance.
// -----------------------------------------------------------------------------
class BaseManager {
  public:
  // -----------------------------------------------------------------------------
  // Return the process-wide BaseManager instance.
  // -----------------------------------------------------------------------------
  static BaseManager &instance();

  // -----------------------------------------------------------------------------
  // Return read access to the cached Base with its lock guard.
  // -----------------------------------------------------------------------------
  BaseRead acquire_read();
  // -----------------------------------------------------------------------------
  // Return write access to the cached Base with its lock guard.
  // -----------------------------------------------------------------------------
  std::pair<libdnf5::Base &, BaseWriteGuard> acquire_write();

  // -----------------------------------------------------------------------------
  // Return the current Base generation counter.
  // -----------------------------------------------------------------------------
  uint64_t current_generation() const
  {
    return generation.load(std::memory_order_relaxed);
  }

  // -----------------------------------------------------------------------------
  // Return the repo state of the cached Base.
  // -----------------------------------------------------------------------------
  BaseRepoState current_repo_state() const;

  // -----------------------------------------------------------------------------
  // Rebuild the cached Base from live metadata with fallback.
  // -----------------------------------------------------------------------------
  BaseRepoState rebuild();
  // -----------------------------------------------------------------------------
  // Rebuild the cached Base from the local rpmdb only.
  // -----------------------------------------------------------------------------
  void rebuild_system_only();
  // -----------------------------------------------------------------------------
  // Drop the cached Base so the next backend operation rebuilds it on demand.
  // -----------------------------------------------------------------------------
  void drop_cached_base();
  // -----------------------------------------------------------------------------
  // Initialize a system-only Base when no Base exists yet.
  // -----------------------------------------------------------------------------
  void ensure_system_only_initialized_if_needed();

#ifdef DNFUI_BUILD_TESTS
  // -----------------------------------------------------------------------------
  // Drop cached backend state for test setup.
  // -----------------------------------------------------------------------------
  void reset_for_tests();
#endif

  private:
  // -----------------------------------------------------------------------------
  // Construct the singleton BaseManager.
  // -----------------------------------------------------------------------------
  BaseManager() = default;
  // -----------------------------------------------------------------------------
  // Prevent copying the singleton BaseManager.
  // -----------------------------------------------------------------------------
  BaseManager(const BaseManager &) = delete;
  // -----------------------------------------------------------------------------
  // Prevent assigning the singleton BaseManager.
  // -----------------------------------------------------------------------------
  BaseManager &operator=(const BaseManager &) = delete;

  // -----------------------------------------------------------------------------
  // Build a Base initialized from the local rpmdb only.
  // -----------------------------------------------------------------------------
  std::shared_ptr<libdnf5::Base> build_initialized_system_only_base();
  // -----------------------------------------------------------------------------
  // Initialize the cached Base when it has not been built yet.
  // -----------------------------------------------------------------------------
  void ensure_base_initialized();

  std::shared_ptr<libdnf5::Base> base_ptr;
  BaseRepoState repo_state = BaseRepoState::LIVE_METADATA;

  std::atomic<uint64_t> generation { 0 };

  // Shared mutex allows many readers but only one writer
  mutable std::shared_mutex base_mutex;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
