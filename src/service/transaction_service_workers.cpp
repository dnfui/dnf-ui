// -----------------------------------------------------------------------------
// transaction_service_workers.cpp
// Runs preview and apply work outside the service main loop.
// Talks to the backend, refreshes package state, and reports progress back to
// request objects.
// -----------------------------------------------------------------------------
#include "transaction_service_internal.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "service/transaction_service_preview_formatter.hpp"

#include <stdexcept>

namespace {

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Force one preview worker exception in test builds so the service smoke test
// can verify that the request still ends in a final failed state.
// -----------------------------------------------------------------------------
static void
throw_if_test_preview_exception_requested()
{
  const char *force_exception = g_getenv("DNFUI_TEST_FORCE_PREVIEW_WORKER_EXCEPTION");
  if (force_exception && g_strcmp0(force_exception, "1") == 0) {
    throw std::runtime_error("Forced transaction preview worker exception.");
  }
}

// -----------------------------------------------------------------------------
// In test builds, allow one preview worker to announce that it has started and
// then wait for a short time. This lets one integration test stop the service
// while the client is blocked waiting for the preview result.
// -----------------------------------------------------------------------------
static void
run_test_preview_wait_hook_if_requested()
{
  const char *started_file = g_getenv("DNFUI_TEST_PREVIEW_STARTED_FILE");
  if (started_file && *started_file) {
    g_file_set_contents(started_file, "", 0, nullptr);
  }

  const char *delay_ms_text = g_getenv("DNFUI_TEST_PREVIEW_DELAY_MS");
  if (!delay_ms_text || !*delay_ms_text) {
    return;
  }

  gchar *end = nullptr;
  guint64 delay_ms = g_ascii_strtoull(delay_ms_text, &end, 10);
  if (end == delay_ms_text || (end && *end != '\0') || delay_ms == 0) {
    return;
  }

  g_usleep(delay_ms * 1000);
}

// -----------------------------------------------------------------------------
// In test builds, allow one upgrade-all preview to finish as an empty
// transaction without touching the real package state.
// -----------------------------------------------------------------------------
static bool
test_force_empty_upgrade_all_preview_requested(const TransactionRequest &request)
{
  const char *force_empty = g_getenv("DNFUI_TEST_FORCE_EMPTY_UPGRADE_ALL_PREVIEW");
  return request.upgrade_all && force_empty && g_strcmp0(force_empty, "1") == 0;
}
#else
// -----------------------------------------------------------------------------
// Do nothing when preview failure injection is not compiled in.
// -----------------------------------------------------------------------------
static void
throw_if_test_preview_exception_requested()
{
}

// -----------------------------------------------------------------------------
// Do nothing when preview delay injection is not compiled in.
// -----------------------------------------------------------------------------
static void
run_test_preview_wait_hook_if_requested()
{
}

// -----------------------------------------------------------------------------
// Do not force empty upgrade-all previews in production builds.
// -----------------------------------------------------------------------------
static bool
test_force_empty_upgrade_all_preview_requested(const TransactionRequest &)
{
  return false;
}
#endif

} // namespace

// -----------------------------------------------------------------------------
// Try to reserve one preview worker slot.
// -----------------------------------------------------------------------------
static bool
try_acquire_preview_worker(TransactionService *service)
{
  if (!service) {
    return false;
  }

  unsigned current = service->preview_workers.load();
  while (current < kMaxPreviewWorkers) {
    if (service->preview_workers.compare_exchange_weak(current, current + 1)) {
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
// Release one previously reserved preview worker slot.
// -----------------------------------------------------------------------------
static void
release_preview_worker(TransactionService *service)
{
  if (!service) {
    return;
  }

  service->preview_workers.fetch_sub(1);
}

struct PreviewWorkerGuard {
  TransactionService *service = nullptr;

  ~PreviewWorkerGuard()
  {
    release_preview_worker(service);
  }
};

struct BackendBaseDropGuard {
  ~BackendBaseDropGuard()
  {
    BaseManager::instance().drop_cached_base();
  }
};

static bool transaction_apply_should_stop_before_work(TransactionSession *session, std::string &details_out);

// -----------------------------------------------------------------------------
// Transaction execution
// -----------------------------------------------------------------------------
// Reject requests that passed shared validation but are unsafe for the service to run.
// -----------------------------------------------------------------------------
bool
validate_transaction_request_for_service(const TransactionRequest &request, std::string &error_out)
{
  if (request.remove.empty() && request.reinstall.empty()) {
    return true;
  }

  BackendBaseDropGuard base_drop_guard;

  try {
    BaseManager::instance().ensure_system_only_initialized_if_needed();
    dnf_backend_refresh_installed_nevras();
  } catch (const std::exception &e) {
    error_out = std::string(_("Unable to validate protected installed packages: ")) + e.what();
    return false;
  }

  for (const auto &spec : request.remove) {
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot remove the package that owns the running application.");
      return false;
    }
  }

  for (const auto &spec : request.reinstall) {
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot reinstall the package that owns the running application while it is running.");
      return false;
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
// Return true when the transaction may need available-repo metadata instead of
// the local rpmdb alone.
// -----------------------------------------------------------------------------
static bool
transaction_request_needs_available_repos(const TransactionRequest &request)
{
  return request.upgrade_all || !request.install.empty() || !request.reinstall.empty();
}

// -----------------------------------------------------------------------------
// Resolve the requested install, remove, and reinstall changes in a worker thread.
// -----------------------------------------------------------------------------
static void
run_transaction_preview(TransactionSession *session)
{
  if (!session) {
    return;
  }

  BackendBaseDropGuard base_drop_guard;

  try {
    TransactionPreview preview;
    std::string error_out;
    queue_transaction_progress(session, _("Loading package base..."));
    // The backend knows only about this callback. The service callback turns
    // backend progress text into Progress signals for the GUI.
    auto progress_cb = [session](const std::string &line) { queue_transaction_progress(session, line); };

    DNFUI_TRACE("Transaction service preview start path=%s", session->object_path.c_str());
    throw_if_test_preview_exception_requested();
    run_test_preview_wait_hook_if_requested();

    if (test_force_empty_upgrade_all_preview_requested(session->request)) {
      // Finish with an empty preview so tests can cover the no-updates path.
      queue_transaction_progress(session, _("No package updates are available."));
      {
        std::lock_guard<std::mutex> lock(session->state_mutex);
        session->preview = preview;
      }
      queue_transaction_finished(
          session, TransactionStage::PREVIEW_READY, true, format_transaction_preview_details(preview));
      return;
    }

    try {
      if (transaction_request_needs_available_repos(session->request)) {
        // The transaction service is a long-lived process, so packages installed or
        // removed outside the GUI can leave its cached Base out of date. Rebuild it
        // before each preview so resolve and apply requests always use the current
        // rpmdb and repository metadata snapshot.
        queue_transaction_progress(session, _("Refreshing backend state..."));
        BaseManager::instance().rebuild();
      } else {
        // Remove-only requests are local-first and should stay usable without
        // waiting on remote repository availability.
        queue_transaction_progress(session, _("Refreshing installed package state..."));
        BaseManager::instance().rebuild_system_only();
      }
    } catch (const std::exception &e) {
      DNFUI_TRACE(
          "Transaction service preview refresh failed path=%s error=%s", session->object_path.c_str(), e.what());
      queue_transaction_progress(session,
                                 _("Backend refresh failed; retrying preview with currently available package state."));
      if (!transaction_request_needs_available_repos(session->request)) {
        BaseManager::instance().ensure_system_only_initialized_if_needed();
      }
    }

    bool ok = dnf_backend_preview_transaction(session->request.install,
                                              session->request.remove,
                                              session->request.reinstall,
                                              preview,
                                              error_out,
                                              progress_cb,
                                              session->request.upgrade_all);

    if (session->cancelled.load()) {
      DNFUI_TRACE("Transaction service preview cancelled path=%s", session->object_path.c_str());
      queue_transaction_finished(session, TransactionStage::CANCELLED, false, _("Transaction preview was cancelled."));
      return;
    }

    if (!ok) {
      DNFUI_TRACE("Transaction service preview failed path=%s", session->object_path.c_str());
      queue_transaction_finished(session, TransactionStage::PREVIEW_FAILED, false, error_out);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      session->preview = preview;
    }
    DNFUI_TRACE("Transaction service preview done path=%s items=%zu",
                session->object_path.c_str(),
                preview.install.size() + preview.upgrade.size() + preview.downgrade.size() + preview.reinstall.size() +
                    preview.remove.size());
    queue_transaction_finished(
        session, TransactionStage::PREVIEW_READY, true, format_transaction_preview_details(preview));
  } catch (const std::exception &e) {
    DNFUI_TRACE("Transaction service preview exception path=%s error=%s", session->object_path.c_str(), e.what());
    queue_transaction_finished(session, TransactionStage::PREVIEW_FAILED, false, e.what());
  } catch (...) {
    DNFUI_TRACE("Transaction service preview exception path=%s error=unknown", session->object_path.c_str());
    queue_transaction_finished(session, TransactionStage::PREVIEW_FAILED, false, _("Transaction preview failed."));
  }
}

// -----------------------------------------------------------------------------
// Start the transaction preview worker for one new request object.
// -----------------------------------------------------------------------------
gboolean
start_transaction_preview(gpointer user_data)
{
  TransactionSession *session = static_cast<TransactionSession *>(user_data);
  if (!session || !session->service || session->finished.load()) {
    return G_SOURCE_REMOVE;
  }

  if (!try_acquire_preview_worker(session->service)) {
    queue_transaction_finished(
        session, TransactionStage::PREVIEW_FAILED, false, _("Too many transaction previews are already running."));
    return G_SOURCE_REMOVE;
  }

  GThread *thread = g_thread_new(
      "dnf-ui-preview",
      +[](gpointer data) -> gpointer {
        TransactionSession *session = static_cast<TransactionSession *>(data);
        PreviewWorkerGuard guard { session ? session->service : nullptr };

        run_transaction_preview(session);
        return nullptr;
      },
      session);
  g_thread_unref(thread);
  return G_SOURCE_REMOVE;
}

// RAII guard that clears the service-wide apply_running flag when it goes out
// of scope, ensuring the flag is reset even if the apply worker exits early.
struct ApplyGuard {
  std::atomic<bool> &flag;
  // -----------------------------------------------------------------------------
  // Clear the apply-running flag when the apply worker exits.
  // -----------------------------------------------------------------------------
  ~ApplyGuard()
  {
    flag = false;
  }
};

// -----------------------------------------------------------------------------
// Run the authorized package transaction in a worker thread.
// -----------------------------------------------------------------------------
static void
run_transaction_apply(TransactionSession *session)
{
  if (!session || !session->service) {
    return;
  }

  ApplyGuard apply_guard { session->service->apply_running };
  BackendBaseDropGuard base_drop_guard;

  // Stop if the request was released after the worker started but before
  // backend package work begins.
  std::string stop_details;
  if (transaction_apply_should_stop_before_work(session, stop_details)) {
    queue_transaction_finished(session, TransactionStage::CANCELLED, false, stop_details);
    return;
  }

  try {
    std::string error_out;
    queue_transaction_progress(session, _("Loading package base..."));
    auto progress_cb = [session](const std::string &line) { queue_transaction_progress(session, line); };

    DNFUI_TRACE("Transaction service apply start path=%s", session->object_path.c_str());
    bool ok = dnf_backend_apply_transaction(session->request.install,
                                            session->request.remove,
                                            session->request.reinstall,
                                            error_out,
                                            progress_cb,
                                            session->request.upgrade_all);

    std::string details;
    TransactionStage stage = TransactionStage::APPLY_FAILED;
    bool success = false;

    if (ok) {
      details = _("Transaction applied successfully.");
      stage = TransactionStage::APPLY_SUCCEEDED;
      success = true;

      try {
        if (transaction_request_needs_available_repos(session->request)) {
          queue_transaction_progress(session, _("Refreshing backend state..."));
          BaseManager::instance().rebuild();
        } else {
          queue_transaction_progress(session, _("Refreshing installed package state..."));
          BaseManager::instance().rebuild_system_only();
        }
      } catch (const std::exception &e) {
        details += "\n";
        details += _("Backend refresh failed: ");
        details += e.what();
      }
    } else {
      details = error_out;
    }

    DNFUI_TRACE("Transaction service apply done path=%s success=%d", session->object_path.c_str(), success ? 1 : 0);
    queue_transaction_finished(session, stage, success, details);
  } catch (const std::exception &e) {
    DNFUI_TRACE("Transaction service apply exception path=%s error=%s", session->object_path.c_str(), e.what());
    queue_transaction_finished(session, TransactionStage::APPLY_FAILED, false, e.what());
  } catch (...) {
    DNFUI_TRACE("Transaction service apply exception path=%s error=unknown", session->object_path.c_str());
    queue_transaction_finished(session, TransactionStage::APPLY_FAILED, false, _("Transaction apply failed."));
  }
}

// -----------------------------------------------------------------------------
// Start the transaction apply worker after authorization succeeds.
// -----------------------------------------------------------------------------
gboolean
start_transaction_apply(gpointer user_data)
{
  TransactionSession *session = static_cast<TransactionSession *>(user_data);
  if (!session || !session->service) {
    return G_SOURCE_REMOVE;
  }

  // Stop if the request was released before the apply worker could be started.
  // No ApplyGuard exists yet, so clear the service-wide apply flag here.
  std::string stop_details;
  if (transaction_apply_should_stop_before_work(session, stop_details)) {
    session->service->apply_running = false;
    queue_transaction_finished(session, TransactionStage::CANCELLED, false, stop_details);
    return G_SOURCE_REMOVE;
  }

  GThread *thread = g_thread_new(
      "dnf-ui-apply",
      +[](gpointer data) -> gpointer {
        run_transaction_apply(static_cast<TransactionSession *>(data));
        return nullptr;
      },
      session);
  g_thread_unref(thread);
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Return true when apply should stop before package work begins.
// Release and client disconnect handling already mark the session cancelled.
// -----------------------------------------------------------------------------
static bool
transaction_apply_should_stop_before_work(TransactionSession *session, std::string &details_out)
{
  details_out.clear();

  if (session->service->shutting_down.load()) {
    details_out = _("Transaction service is shutting down.");
    return true;
  }

  if (session->release_requested.load() || session->cancelled.load()) {
    details_out = _("Transaction apply was cancelled before it started.");
    return true;
  }

  return false;
}
