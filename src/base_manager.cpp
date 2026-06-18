// -----------------------------------------------------------------------------
// src/base_manager.cpp
// Shared libdnf5 Base manager
// Creates, reuses, and rebuilds Base objects while protecting access from worker threads.
// -----------------------------------------------------------------------------
#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "i18n.hpp"

#include <libdnf5/conf/const.hpp>
#include <libdnf5/repo/download_callbacks.hpp>
#include <libdnf5/repo/repo.hpp>
#include <libdnf5/repo/repo_cache.hpp>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_set>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {

// Controls which repository sources are loaded into a temporary libdnf5 Base.
// FULL uses the normal enabled repositories.
// CACHE_ONLY_METADATA uses already cached repository metadata when live repo loading failed.
// SYSTEM_ONLY loads only the local installed package database and skips available repositories.
enum class RepoLoadMode {
  FULL,
  CACHE_ONLY_METADATA,
  SYSTEM_ONLY,
};

struct BuiltBase {
  std::shared_ptr<libdnf5::Base> base;
  BaseRepoState repo_state = BaseRepoState::LIVE_METADATA;
};

struct DownloadProgressState {
  std::string description;
  int last_reported_bucket = -1;
};

// -----------------------------------------------------------------------------
// Return true when the caller has requested that a Base operation stop.
// -----------------------------------------------------------------------------
static bool
base_operation_cancel_requested(const std::shared_ptr<std::atomic<bool>> &cancel_requested)
{
  return cancel_requested && cancel_requested->load(std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// Stop repository downloads when the caller cancels the rebuild and report real download progress when requested.
// libdnf calls these methods while downloading repository metadata.
// Returning ABORT tells libdnf to stop the current transfer.
// This is cooperative cancellation, so it only runs when libdnf reaches one of these callbacks.
// -----------------------------------------------------------------------------
class CancelableDownloadCallbacks : public libdnf5::repo::DownloadCallbacks {
  public:
  CancelableDownloadCallbacks(std::shared_ptr<std::atomic<bool>> cancel_requested,
                              BaseProgressCallback progress_callback)
      : cancel_requested(std::move(cancel_requested))
      , progress_callback(std::move(progress_callback))
  {
  }

  ~CancelableDownloadCallbacks() override
  {
    std::lock_guard<std::mutex> guard(downloads_mutex);
    for (auto *state : active_downloads) {
      delete state;
    }
    active_downloads.clear();
  }

  void *add_new_download(void *, const char *description, double) override
  {
    auto *state = new DownloadProgressState;
    state->description = description ? description : _("repository metadata");
    {
      std::lock_guard<std::mutex> guard(downloads_mutex);
      active_downloads.insert(state);
    }
    emit_progress(std::string(_("Downloading: ")) + state->description);
    return state;
  }

  int progress(void *user_cb_data, double total_to_download, double downloaded) override
  {
    trace_cancel_once("progress");
    auto *state = static_cast<DownloadProgressState *>(user_cb_data);
    std::string progress_message;

    if (state && total_to_download > 0.0) {
      std::lock_guard<std::mutex> guard(downloads_mutex);
      if (active_downloads.find(state) == active_downloads.end()) {
        return cancelled() ? libdnf5::repo::DownloadCallbacks::ABORT : libdnf5::repo::DownloadCallbacks::OK;
      }

      int percent = static_cast<int>((downloaded * 100.0) / total_to_download);
      percent = std::clamp(percent, 0, 100);
      int bucket = percent / 10;

      // libdnf calls this often. Ten percent steps keep the UI live without sending every byte update.
      if (bucket > state->last_reported_bucket) {
        state->last_reported_bucket = bucket;
        progress_message =
            std::string(_("Download progress: ")) + state->description + " (" + std::to_string(percent) + "%)";
      }
    }
    emit_progress(progress_message);
    return cancelled() ? libdnf5::repo::DownloadCallbacks::ABORT : libdnf5::repo::DownloadCallbacks::OK;
  }

  int end(void *user_cb_data, TransferStatus status, const char *msg) override
  {
    trace_cancel_once("end");
    auto *state = static_cast<DownloadProgressState *>(user_cb_data);
    std::unique_ptr<DownloadProgressState> owned_state;
    {
      std::lock_guard<std::mutex> guard(downloads_mutex);
      if (active_downloads.erase(state) != 0) {
        owned_state.reset(state);
      }
    }

    const std::string description = owned_state ? owned_state->description : _("repository metadata");
    if (status == TransferStatus::SUCCESSFUL || status == TransferStatus::ALREADYEXISTS) {
      emit_progress(std::string(_("Download ready: ")) + description);
    } else if (msg && msg[0] != '\0') {
      emit_progress(msg);
    }
    return libdnf5::repo::DownloadCallbacks::OK;
  }

  int mirror_failure(void *, const char *msg, const char *, const char *) override
  {
    trace_cancel_once("mirror failure");
    if (msg && msg[0] != '\0') {
      emit_progress(msg);
    }
    return cancelled() ? libdnf5::repo::DownloadCallbacks::ABORT : libdnf5::repo::DownloadCallbacks::OK;
  }

  private:
  // Log only the first callback that notices cancellation.
  // NOTE: After Stop is pressed, libdnf may call several download callbacks before the load ends.
  void trace_cancel_once(const char *callback_name)
  {
    if (!cancelled()) {
      return;
    }

#ifndef DNFUI_DEBUG_TRACE
    (void)callback_name;
#endif
    bool expected = false;
    if (cancel_trace_written.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
      DNFUI_TRACE("BaseManager repository refresh stop requested from %s callback", callback_name);
    }
  }

  bool cancelled() const
  {
    return base_operation_cancel_requested(cancel_requested);
  }

  void emit_progress(const std::string &message)
  {
    if (progress_callback && !message.empty()) {
      progress_callback(message);
    }
  }

  // Shared with the UI task that owns this refresh.
  // The button sets this flag, and the libdnf callback reads it from the worker thread.
  std::shared_ptr<std::atomic<bool>> cancel_requested;
  BaseProgressCallback progress_callback;
  std::atomic<bool> cancel_trace_written { false };
  // libdnf gives the same pointer back to progress and end.
  // Track active states so cancellation cannot free the same state twice.
  std::mutex downloads_mutex;
  std::unordered_set<DownloadProgressState *> active_downloads;
};

// -----------------------------------------------------------------------------
// Clear download callbacks before a temporary Base leaves scope.
// Repository refresh cancellation raises exceptions from libdnf callbacks, so
// callback cleanup must not depend on the normal return path.
// -----------------------------------------------------------------------------
class DownloadCallbacksReset {
  public:
  explicit DownloadCallbacksReset(libdnf5::Base &base)
      : base(base)
  {
  }

  ~DownloadCallbacksReset()
  {
    base.set_download_callbacks(std::unique_ptr<libdnf5::repo::DownloadCallbacks>());
  }

  private:
  libdnf5::Base &base;
};

// -----------------------------------------------------------------------------
// Throw when the caller has requested that a rebuild stop.
// -----------------------------------------------------------------------------
static void
throw_if_base_operation_cancelled(const std::shared_ptr<std::atomic<bool>> &cancel_requested, const char *message)
{
  if (base_operation_cancel_requested(cancel_requested)) {
    DNFUI_TRACE("BaseManager operation stop observed");
    throw BaseOperationCancelled(message ? message : "Base operation was cancelled.");
  }
}

// -----------------------------------------------------------------------------
// Throw when the caller has requested that a rebuild stop.
// -----------------------------------------------------------------------------
static void
throw_if_rebuild_cancelled(const std::shared_ptr<std::atomic<bool>> &cancel_requested)
{
  throw_if_base_operation_cancelled(cancel_requested, "Repository refresh was cancelled.");
}

// -----------------------------------------------------------------------------
// Build one fully configured Base before any repo metadata is loaded.
// -----------------------------------------------------------------------------
static std::shared_ptr<libdnf5::Base>
create_configured_base(RepoLoadMode mode, bool load_changelog_metadata = false)
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
// Mark repository metadata caches as expired before loading repos.
// This follows DNF's documented expire-cache behavior.
// -----------------------------------------------------------------------------
static void
expire_repository_metadata_cache(libdnf5::Base &base)
{
  const std::filesystem::path cachedir { base.get_config().get_cachedir_option().get_value() };
  DNFUI_TRACE("BaseManager repository cache expiration start cachedir=%s", cachedir.string().c_str());

  // Expiring the cache makes the next repo load check whether repository metadata needs to be refreshed.
  std::error_code iter_error;
  const std::filesystem::directory_iterator cache_dirs(cachedir, iter_error);

  if (iter_error) {
    // A missing cache directory is fine on a fresh system or after cleanup.
    // Other errors should be visible in logs, but should not stop the refresh.
    if (iter_error != std::errc::no_such_file_or_directory) {
      std::cerr << "Warning: failed to read repository cache directory " << cachedir.string() << ": "
                << iter_error.message() << std::endl;
      DNFUI_TRACE("BaseManager failed to read repository cache directory %s: %s",
                  cachedir.string().c_str(),
                  iter_error.message().c_str());
    }
    return;
  }

  for (const auto &dir_entry : cache_dirs) {
    std::error_code entry_error;
    if (!dir_entry.is_directory(entry_error)) {
      if (entry_error) {
        const std::string path = dir_entry.path().string();
        std::cerr << "Warning: failed to inspect repo cache path " << path << ": " << entry_error.message()
                  << std::endl;
        DNFUI_TRACE(
            "BaseManager failed to inspect repo cache path %s: %s", path.c_str(), entry_error.message().c_str());
      }
      continue;
    }

    const std::string path = dir_entry.path().string();

    try {
      libdnf5::repo::RepoCache cache(base.get_weak_ptr(), dir_entry.path());
      cache.write_attribute(libdnf5::repo::RepoCache::ATTRIBUTE_EXPIRED);
      DNFUI_TRACE("BaseManager marked repository cache expired path=%s", path.c_str());
    } catch (const std::exception &e) {
      // Keep going. A failed expire attempt should not make the app unusable.
      // The following repo load can still succeed, or the existing fallback
      // path can use cached metadata or installed packages only.
      std::cerr << "Warning: failed to expire repo cache " << path << ": " << e.what() << std::endl;
      DNFUI_TRACE("BaseManager failed to expire repo cache %s: %s", path.c_str(), e.what());
    }
  }

  DNFUI_TRACE("BaseManager finished repository metadata cache expiration attempt");
}

// -----------------------------------------------------------------------------
// Load repository data for one already configured Base. Startup fallback should
// only trigger when this step fails for the full repo set.
// -----------------------------------------------------------------------------
static void
load_repo_data(libdnf5::Base &base, RepoLoadMode mode, const std::shared_ptr<std::atomic<bool>> &cancel_requested)
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

  throw_if_rebuild_cancelled(cancel_requested);
  DNFUI_TRACE("BaseManager load repos start mode=%d", static_cast<int>(mode));
  if (mode == RepoLoadMode::SYSTEM_ONLY) {
    repo_sack->load_repos(libdnf5::repo::Repo::Type::SYSTEM);
  } else {
    repo_sack->load_repos();
  }
  DNFUI_TRACE("BaseManager load repos done mode=%d", static_cast<int>(mode));
  throw_if_rebuild_cancelled(cancel_requested);
}

} // namespace

// -----------------------------------------------------------------------------
// Build one Base and load repository data for the requested mode.
// -----------------------------------------------------------------------------
static BuiltBase
build_base_for_mode(RepoLoadMode mode,
                    BaseRefreshMode refresh_mode,
                    const std::shared_ptr<std::atomic<bool>> &cancel_requested,
                    BaseProgressCallback progress_callback,
                    bool load_changelog_metadata = false)
{
  BuiltBase result;
  DNFUI_TRACE(
      "BaseManager build base start mode=%d refresh=%d", static_cast<int>(mode), static_cast<int>(refresh_mode));
  throw_if_rebuild_cancelled(cancel_requested);
  result.base = create_configured_base(mode, load_changelog_metadata);
  const bool has_download_callbacks = cancel_requested || static_cast<bool>(progress_callback);
  if (has_download_callbacks) {
    result.base->set_download_callbacks(
        std::make_unique<CancelableDownloadCallbacks>(cancel_requested, std::move(progress_callback)));
  }
  std::unique_ptr<DownloadCallbacksReset> download_callbacks_reset;
  if (has_download_callbacks) {
    download_callbacks_reset = std::make_unique<DownloadCallbacksReset>(*result.base);
  }
  if (mode == RepoLoadMode::FULL && refresh_mode == BaseRefreshMode::FORCE_METADATA_CHECK) {
    throw_if_rebuild_cancelled(cancel_requested);
    expire_repository_metadata_cache(*result.base);
  }
  load_repo_data(*result.base, mode, cancel_requested);
  if (mode == RepoLoadMode::CACHE_ONLY_METADATA) {
    result.repo_state = BaseRepoState::CACHED_METADATA;
  } else if (mode == RepoLoadMode::SYSTEM_ONLY) {
    result.repo_state = BaseRepoState::INSTALLED_ONLY;
  }
  DNFUI_TRACE(
      "BaseManager initialize done mode=%d state=%d", static_cast<int>(mode), static_cast<int>(result.repo_state));
  return result;
}

// -----------------------------------------------------------------------------
// Try the normal live repo load first, then cached metadata, then finally
// installed-package-only mode so the app stays usable when the network is down.
// -----------------------------------------------------------------------------
static BuiltBase
build_base_with_offline_fallback(BaseRefreshMode refresh_mode = BaseRefreshMode::NORMAL,
                                 std::shared_ptr<std::atomic<bool>> cancel_requested = nullptr,
                                 BaseProgressCallback progress_callback = {},
                                 bool load_changelog_metadata = false)
{
  try {
    return build_base_for_mode(
        RepoLoadMode::FULL, refresh_mode, cancel_requested, progress_callback, load_changelog_metadata);
  } catch (const BaseOperationCancelled &) {
    // Stop is not a repo load failure. Do not continue into fallback modes.
    DNFUI_TRACE("BaseManager live repo load stopped before fallback");
    throw;
  } catch (const std::exception &repo_error) {
    throw_if_rebuild_cancelled(cancel_requested);
    std::cerr << "Warning: repo load failed: " << repo_error.what() << std::endl;
    DNFUI_TRACE("BaseManager load repos failed: %s", repo_error.what());
    std::cerr << "Warning: live repo load failed, retrying from cached metadata: " << repo_error.what() << std::endl;
    DNFUI_TRACE("BaseManager live repo load failed, trying cached metadata fallback: %s", repo_error.what());

    try {
      return build_base_for_mode(
          RepoLoadMode::CACHE_ONLY_METADATA, BaseRefreshMode::NORMAL, cancel_requested, {}, load_changelog_metadata);
    } catch (const BaseOperationCancelled &) {
      // Stop is not a cache load failure. Do not continue into fallback modes.
      DNFUI_TRACE("BaseManager cached repo load stopped before fallback");
      throw;
    } catch (const std::exception &cache_error) {
      throw_if_rebuild_cancelled(cancel_requested);
      std::cerr << "Warning: cached repo metadata load failed: " << cache_error.what() << std::endl;
      DNFUI_TRACE("BaseManager cached repo load failed, trying system-only fallback: %s", cache_error.what());

      try {
        return build_base_for_mode(
            RepoLoadMode::SYSTEM_ONLY, BaseRefreshMode::NORMAL, cancel_requested, {}, load_changelog_metadata);
      } catch (const BaseOperationCancelled &) {
        // Stop is not a system load failure.
        DNFUI_TRACE("BaseManager system-only load stopped");
        throw;
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
// Return serialized read access, but stop if the caller cancels while waiting.
// -----------------------------------------------------------------------------
BaseRead
BaseManager::acquire_read(std::shared_ptr<std::atomic<bool>> cancel_requested)
{
  // Keep the lock exclusive for the whole libdnf Base operation.
  // PackageQuery work can touch shared Base internals even when the caller only reads data.
  //
  // lock() would wait here without checking Stop. Use short try_lock waits so a
  // package list worker can stop while another Base operation is still running.
  std::unique_lock<std::shared_mutex> lock(base_mutex, std::defer_lock);
  while (!lock.try_lock()) {
    throw_if_base_operation_cancelled(cancel_requested, "Package query was cancelled.");
    // Avoid busy waiting while another worker owns BaseManager.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  throw_if_base_operation_cancelled(cancel_requested, "Package query was cancelled.");
  if (!base_ptr) {
    ensure_base_initialized(cancel_requested);
  }
  throw_if_base_operation_cancelled(cancel_requested, "Package query was cancelled.");
  if (!base_ptr) {
    // Never return a null Base reference.
    throw std::runtime_error("DNF backend not initialized (Base is null).");
  }

  return { *base_ptr, BaseGuard(std::move(lock)), generation.load(std::memory_order_relaxed) };
}

// -----------------------------------------------------------------------------
// Return serialized access to a temporary Base that reads only the local rpmdb.
// -----------------------------------------------------------------------------
TemporaryBaseRead
BaseManager::acquire_system_only_read()
{
  std::unique_lock<std::shared_mutex> lock(base_mutex);
  auto built_base = build_initialized_system_only_base();
  if (!built_base) {
    throw std::runtime_error("System-only backend initialization failed (Base is null).");
  }

  return TemporaryBaseRead(std::move(built_base), BaseGuard(std::move(lock)));
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
// Build a temporary Base that includes repository changelog metadata.
// -----------------------------------------------------------------------------
std::shared_ptr<libdnf5::Base>
BaseManager::build_changelog_base()
{
  BuiltBase built = build_base_with_offline_fallback(BaseRefreshMode::NORMAL, nullptr, {}, true);
  if (!built.base) {
    throw std::runtime_error("Changelog backend initialization failed (Base is null).");
  }

  return built.base;
}

// -----------------------------------------------------------------------------
// Rebuild the cached Base after repository refresh or transaction work.
// -----------------------------------------------------------------------------
BaseRepoState
BaseManager::rebuild(BaseRefreshMode refresh_mode,
                     std::shared_ptr<std::atomic<bool>> cancel_requested,
                     BaseProgressCallback progress_callback)
{
  // Allow only one Base rebuild at a time.
  std::unique_lock lock(base_mutex);

  // Build the replacement first so a refresh failure does not discard the last
  // usable Base. Offline fallback keeps the UI query paths working from cached
  // metadata or, as a last resort, from the local rpmdb only.
  BuiltBase rebuilt =
      build_base_with_offline_fallback(refresh_mode, std::move(cancel_requested), std::move(progress_callback));
  if (!rebuilt.base) {
    throw std::runtime_error("Repository rebuild failed (Base is null).");
  }

  base_ptr = rebuilt.base;
  repo_state = rebuilt.repo_state;

  // Publish the generation change only after the new Base is ready so readers
  // never drop their cached results without a replacement snapshot to use.
  base_epoch.fetch_add(1, std::memory_order_relaxed);
  generation.fetch_add(1, std::memory_order_relaxed);
  return rebuilt.repo_state;
}

// -----------------------------------------------------------------------------
// Force a local-only rebuild that loads only the installed-package view from the rpmdb.
// This keeps remove-only transaction flows independent of remote repository availability.
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
  base_epoch.fetch_add(1, std::memory_order_relaxed);
  generation.fetch_add(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// Drop the cached Base so memory-heavy metadata does not stay resident after short-lived backend work.
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
    base_epoch.fetch_add(1, std::memory_order_relaxed);
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
    base_epoch.fetch_add(1, std::memory_order_relaxed);
  }
}

// -----------------------------------------------------------------------------
// Build a Base that reads only the local installed package database.
// -----------------------------------------------------------------------------
std::shared_ptr<libdnf5::Base>
BaseManager::build_initialized_system_only_base()
{
  return build_base_for_mode(RepoLoadMode::SYSTEM_ONLY, BaseRefreshMode::NORMAL, nullptr, {}).base;
}

// -----------------------------------------------------------------------------
// Create the shared Base while the caller holds the write lock.
// -----------------------------------------------------------------------------
void
BaseManager::ensure_base_initialized(std::shared_ptr<std::atomic<bool>> cancel_requested)
{
  if (!base_ptr) {
    BuiltBase built = build_base_with_offline_fallback(BaseRefreshMode::NORMAL, std::move(cancel_requested));
    base_ptr = built.base;
    repo_state = built.repo_state;
    base_epoch.fetch_add(1, std::memory_order_relaxed);
  }
}

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Return true when a cached Base exists.
// -----------------------------------------------------------------------------
bool
BaseManager::has_cached_base_for_tests() const
{
  std::shared_lock<std::shared_mutex> shared(base_mutex);
  return base_ptr != nullptr;
}

// -----------------------------------------------------------------------------
// Clear cached Base state between tests.
// -----------------------------------------------------------------------------
void
BaseManager::reset_for_tests()
{
  std::unique_lock<std::shared_mutex> unique(base_mutex);
  base_ptr.reset();
  generation.store(0, std::memory_order_relaxed);
  base_epoch.store(0, std::memory_order_relaxed);
}
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
