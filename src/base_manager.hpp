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
// Lock guard helpers for thread-safe Base access.
// libdnf Base queries are serialized because PackageQuery work can touch
// shared Base internals even for read-only UI operations.
// -----------------------------------------------------------------------------
class BaseGuard {
  public:
  // -----------------------------------------------------------------------------
  // Take ownership of a BaseManager lock.
  // -----------------------------------------------------------------------------
  explicit BaseGuard(std::unique_lock<std::shared_mutex> &&l)
      : lock(std::move(l))
  {
  }
  BaseGuard(BaseGuard &&) noexcept = default;
  BaseGuard &operator=(BaseGuard &&) noexcept = default;
  BaseGuard(const BaseGuard &) = delete;
  BaseGuard &operator=(const BaseGuard &) = delete;

  private:
  std::unique_lock<std::shared_mutex> lock;
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
// Serialized read access bundle with Base reference, lock guard, and generation snapshot.
// -----------------------------------------------------------------------------
struct BaseRead {
  libdnf5::Base &base;
  BaseGuard guard;
  uint64_t generation;
};

// -----------------------------------------------------------------------------
// Serialized access bundle for a short-lived Base that is not cached globally.
// -----------------------------------------------------------------------------
struct TemporaryBaseRead {
  std::shared_ptr<libdnf5::Base> base;
  BaseGuard guard;

  TemporaryBaseRead(std::shared_ptr<libdnf5::Base> &&base_ptr, BaseGuard &&base_guard);
  TemporaryBaseRead(TemporaryBaseRead &&) noexcept = default;
  TemporaryBaseRead &operator=(TemporaryBaseRead &&) noexcept = default;
  TemporaryBaseRead(const TemporaryBaseRead &) = delete;
  TemporaryBaseRead &operator=(const TemporaryBaseRead &) = delete;
  ~TemporaryBaseRead();
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
  // Return serialized read access to the cached Base with its lock guard.
  // -----------------------------------------------------------------------------
  BaseRead acquire_read();
  // -----------------------------------------------------------------------------
  // Return write access to the cached Base with its lock guard.
  // -----------------------------------------------------------------------------
  std::pair<libdnf5::Base &, BaseWriteGuard> acquire_write();
  // -----------------------------------------------------------------------------
  // Return serialized access to a temporary Base that includes changelog metadata.
  // -----------------------------------------------------------------------------
  TemporaryBaseRead acquire_changelog_read();
  // -----------------------------------------------------------------------------
  // Return serialized access to a temporary Base that reads only the local rpmdb.
  // -----------------------------------------------------------------------------
  TemporaryBaseRead acquire_system_only_read();

  // -----------------------------------------------------------------------------
  // Return the current Base generation counter.
  // -----------------------------------------------------------------------------
  uint64_t current_generation() const
  {
    return generation.load(std::memory_order_relaxed);
  }

  // -----------------------------------------------------------------------------
  // Return the current cached Base lifetime marker.
  // This changes whenever the shared cached Base is replaced, created, or dropped.
  // -----------------------------------------------------------------------------
  uint64_t current_base_epoch() const
  {
    return base_epoch.load(std::memory_order_relaxed);
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
  // Return true when a cached Base exists.
  // -----------------------------------------------------------------------------
  bool has_cached_base_for_tests() const;
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
  std::atomic<uint64_t> base_epoch { 0 };

  // Serializes Base queries, rebuilds, transactions, and Base destruction.
  mutable std::shared_mutex base_mutex;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
