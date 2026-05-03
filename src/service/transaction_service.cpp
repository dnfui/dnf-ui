// -----------------------------------------------------------------------------
// src/service/transaction_service.cpp
// Privileged D-Bus transaction service
// Owns transaction request objects on the bus, resolves previews, authorizes
// apply calls through Polkit, runs package transactions, and reports progress
// and final state back to the GUI client.
// -----------------------------------------------------------------------------
#include "transaction_service.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "service/transaction_service_dbus.hpp"
#include "service/transaction_service_introspection.hpp"
#include "service/transaction_service_preview_formatter.hpp"
#include "service/transaction_service_request_parser.hpp"
#include "transaction_request.hpp"

#include <gio/gio.h>
#include <glib-unix.h>
#include <polkit/polkit.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
// Transaction service D-Bus names
// -----------------------------------------------------------------------------
constexpr const char *kServiceName = kTransactionServiceName;
constexpr const char *kManagerObjectPath = kTransactionServiceManagerPath;
constexpr const char *kManagerInterface = kTransactionServiceManagerInterface;
constexpr const char *kTransactionInterface = kTransactionServiceRequestInterface;
constexpr const char *kApplyActionId = "com.fedora.dnfui.apply-transactions";
constexpr size_t kMaxLiveTransactionSessions = 32;
constexpr size_t kMaxLiveTransactionSessionsPerClient = 8;
constexpr unsigned kMaxPreviewWorkers = 2;

// -----------------------------------------------------------------------------
// Transaction service runtime state
// -----------------------------------------------------------------------------
struct TransactionService;

enum class TransactionStage {
  PREVIEW_RUNNING,
  PREVIEW_READY,
  PREVIEW_FAILED,
  APPLY_RUNNING,
  APPLY_SUCCEEDED,
  APPLY_FAILED,
  CANCELLED,
};

struct TransactionSession {
  TransactionService *service = nullptr;
  guint registration_id = 0;
  std::string object_path;
  // Protects preview, stage, success, details, and pending_apply_invocation.
  std::mutex state_mutex;
  TransactionRequest request;
  TransactionPreview preview;
  std::atomic<bool> finished { false };
  std::atomic<bool> cancelled { false };
  std::atomic<bool> release_requested { false };
  TransactionStage stage = TransactionStage::PREVIEW_RUNNING;
  bool success = false;
  std::string details;
  GDBusMethodInvocation *pending_apply_invocation = nullptr;
  std::string owner_name;
  guint owner_watch_id = 0;
};

struct TransactionService {
  GMainLoop *loop = nullptr;
  GMainContext *main_context = nullptr;
  GDBusConnection *connection = nullptr;
  GDBusNodeInfo *manager_node_info = nullptr;
  GDBusNodeInfo *transaction_node_info = nullptr;
  GBusType bus_type = G_BUS_TYPE_SESSION;
  guint owner_id = 0;
  guint manager_registration_id = 0;
  guint next_transaction_id = 1;
  std::atomic<bool> apply_running { false };
  std::atomic<bool> shutting_down { false };
  bool keep_alive_until_exit = false;
  std::map<std::string, std::unique_ptr<TransactionSession>> transactions;
  std::atomic<unsigned> preview_workers { 0 };
};

// -----------------------------------------------------------------------------
// Transaction session signal helpers
// -----------------------------------------------------------------------------
static void emit_transaction_progress(TransactionSession *session, const std::string &line);
static void emit_transaction_finished(TransactionSession *session,
                                      TransactionStage stage,
                                      bool success,
                                      const std::string &details);
static void queue_transaction_release(TransactionSession *session);

// -----------------------------------------------------------------------------
// Transaction preview formatting
// -----------------------------------------------------------------------------
static const char *transaction_stage_name(TransactionStage stage);

// -----------------------------------------------------------------------------
// Transaction authorization helpers
// -----------------------------------------------------------------------------
static void on_apply_authorization_result(GObject *source_object, GAsyncResult *res, gpointer user_data);
static bool
start_authorize_apply_request(TransactionSession *session, GDBusMethodInvocation *invocation, std::string &error_out);
static void complete_apply_request(TransactionSession *session, GDBusMethodInvocation *invocation);

// -----------------------------------------------------------------------------
// Transaction execution helpers
// -----------------------------------------------------------------------------
static gboolean start_transaction_preview(gpointer user_data);
static gboolean start_transaction_apply(gpointer user_data);
static bool validate_transaction_request_for_service(const TransactionRequest &request, std::string &error_out);
static bool get_invocation_sender(GDBusMethodInvocation *invocation, std::string &sender_out, std::string &error_out);
static bool transaction_apply_should_stop_before_work(TransactionSession *session, std::string &details_out);
static bool transaction_request_needs_available_repos(const TransactionRequest &request);

// -----------------------------------------------------------------------------
// Main loop dispatch helpers
// -----------------------------------------------------------------------------
struct QueuedProgressMessage {
  TransactionSession *session = nullptr;
  std::string line;
};

struct QueuedFinishedResult {
  TransactionSession *session = nullptr;
  TransactionStage stage = TransactionStage::PREVIEW_FAILED;
  bool success = false;
  std::string details;
};

struct QueuedSessionRelease {
  TransactionService *service = nullptr;
  std::string object_path;
};

// -----------------------------------------------------------------------------
// Return true when request object limits are already reached.
// -----------------------------------------------------------------------------
static bool
service_request_limit_reached(TransactionService *service, const std::string &owner_name, std::string &error_out)
{
  if (!service) {
    error_out = _("Transaction service is not ready.");
    return true;
  }

  if (service->transactions.size() >= kMaxLiveTransactionSessions) {
    error_out = _("The transaction service has too many active requests.");
    return true;
  }

  size_t owner_count = 0;
  for (const auto &[path, session] : service->transactions) {
    (void)path;
    if (session && session->owner_name == owner_name) {
      owner_count++;
    }
  }

  if (owner_count >= kMaxLiveTransactionSessionsPerClient) {
    error_out = _("This client has too many active transaction requests.");
    return true;
  }

  return false;
}

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

// -----------------------------------------------------------------------------
// Copy one queued progress message onto the main loop and emit it on D-Bus.
// -----------------------------------------------------------------------------
static gboolean
dispatch_transaction_progress(gpointer user_data)
{
  std::unique_ptr<QueuedProgressMessage> message(static_cast<QueuedProgressMessage *>(user_data));
  if (!message || !message->session) {
    return G_SOURCE_REMOVE;
  }

  emit_transaction_progress(message->session, message->line);
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Copy one queued finished result onto the main loop and publish it on D-Bus.
// -----------------------------------------------------------------------------
static gboolean
dispatch_transaction_finished(gpointer user_data)
{
  std::unique_ptr<QueuedFinishedResult> result(static_cast<QueuedFinishedResult *>(user_data));
  if (!result || !result->session) {
    return G_SOURCE_REMOVE;
  }

  emit_transaction_finished(result->session, result->stage, result->success, result->details);
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Remove one finished transaction request object from the live service map.
// -----------------------------------------------------------------------------
static gboolean
dispatch_transaction_release(gpointer user_data)
{
  std::unique_ptr<QueuedSessionRelease> release(static_cast<QueuedSessionRelease *>(user_data));
  if (!release || !release->service) {
    return G_SOURCE_REMOVE;
  }

  auto it = release->service->transactions.find(release->object_path);
  if (it == release->service->transactions.end()) {
    return G_SOURCE_REMOVE;
  }

  TransactionSession *session = it->second.get();
  session->release_requested = true;

  // Stop watching the client's bus name since the session is being released.
  if (session->owner_watch_id != 0) {
    g_bus_unwatch_name(session->owner_watch_id);
    session->owner_watch_id = 0;
  }

  // A disconnected client cannot complete a pending authorization request.
  {
    std::lock_guard<std::mutex> lock(session->state_mutex);
    if (session->pending_apply_invocation) {
      g_object_unref(session->pending_apply_invocation);
      session->pending_apply_invocation = nullptr;
    }
  }

  // Keep the session alive until running preview or apply work has reached a
  // finished state. Worker threads still hold a raw session pointer.
  if (!session->finished.load()) {
    session->cancelled = true;
    return G_SOURCE_REMOVE;
  }

  if (release->service->connection && session->registration_id != 0) {
    g_dbus_connection_unregister_object(release->service->connection, session->registration_id);
  }

  release->service->transactions.erase(it);
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Emit one progress line for a live transaction request object.
// -----------------------------------------------------------------------------
static void
emit_transaction_progress(TransactionSession *session, const std::string &line)
{
  if (!session || !session->service || !session->service->connection || session->finished.load()) {
    return;
  }

  g_dbus_connection_emit_signal(session->service->connection,
                                nullptr,
                                session->object_path.c_str(),
                                kTransactionInterface,
                                "Progress",
                                g_variant_new("(s)", line.c_str()),
                                nullptr);
}

// -----------------------------------------------------------------------------
// Publish the final state for one transaction request object.
// -----------------------------------------------------------------------------
static void
emit_transaction_finished(TransactionSession *session, TransactionStage stage, bool success, const std::string &details)
{
  if (!session || !session->service || !session->service->connection) {
    return;
  }

  bool expected = false;
  if (!session->finished.compare_exchange_strong(expected, true)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(session->state_mutex);
    session->stage = stage;
    session->success = success;
    session->details = details;
  }
  g_dbus_connection_emit_signal(session->service->connection,
                                nullptr,
                                session->object_path.c_str(),
                                kTransactionInterface,
                                "Finished",
                                g_variant_new("(sbs)", transaction_stage_name(stage), success, details.c_str()),
                                nullptr);

  // A disconnected client cannot call Release after the running work ends, so
  // finish processing is responsible for completing the deferred cleanup.
  if (session->release_requested.load()) {
    queue_transaction_release(session);
  }
}

// -----------------------------------------------------------------------------
// Queue one transaction progress line back onto the service main loop.
// -----------------------------------------------------------------------------
static void
queue_transaction_progress(TransactionSession *session, const std::string &line)
{
  if (!session || line.empty() || session->finished.load()) {
    return;
  }

  auto *message = new QueuedProgressMessage();
  message->session = session;
  message->line = line;
  g_main_context_invoke(session->service->main_context, dispatch_transaction_progress, message);
}

// -----------------------------------------------------------------------------
// Queue the final state update for one transaction request object.
// -----------------------------------------------------------------------------
static void
queue_transaction_finished(TransactionSession *session,
                           TransactionStage stage,
                           bool success,
                           const std::string &details)
{
  if (!session) {
    return;
  }

  auto *result = new QueuedFinishedResult();
  result->session = session;
  result->stage = stage;
  result->success = success;
  result->details = details;
  g_main_context_invoke(session->service->main_context, dispatch_transaction_finished, result);
}

// -----------------------------------------------------------------------------
// Queue cleanup of one finished transaction request object.
// -----------------------------------------------------------------------------
static void
queue_transaction_release(TransactionSession *session)
{
  if (!session || !session->service) {
    return;
  }

  auto *release = new QueuedSessionRelease();
  release->service = session->service;
  release->object_path = session->object_path;
  g_main_context_invoke(session->service->main_context, dispatch_transaction_release, release);
}

// -----------------------------------------------------------------------------
// Copy one preview section into a D-Bus string array builder.
// -----------------------------------------------------------------------------
static void
append_transaction_preview_array(GVariantBuilder &builder, const std::vector<std::string> &items)
{
  for (const auto &item : items) {
    g_variant_builder_add(&builder, "s", item.c_str());
  }
}

// -----------------------------------------------------------------------------
// Map one internal transaction stage to its D-Bus state string.
// -----------------------------------------------------------------------------
static const char *
transaction_stage_name(TransactionStage stage)
{
  switch (stage) {
  case TransactionStage::PREVIEW_RUNNING:
    return "preview-running";
  case TransactionStage::PREVIEW_READY:
    return "preview-ready";
  case TransactionStage::PREVIEW_FAILED:
    return "preview-failed";
  case TransactionStage::APPLY_RUNNING:
    return "apply-running";
  case TransactionStage::APPLY_SUCCEEDED:
    return "apply-succeeded";
  case TransactionStage::APPLY_FAILED:
    return "apply-failed";
  case TransactionStage::CANCELLED:
    return "cancelled";
  }

  return "unknown";
}

// -----------------------------------------------------------------------------
// Reset one transaction request object to a running state before new work starts.
// -----------------------------------------------------------------------------
static void
set_transaction_running(TransactionSession *session, TransactionStage stage)
{
  if (!session) {
    return;
  }

  std::lock_guard<std::mutex> lock(session->state_mutex);
  session->stage = stage;
  session->finished = false;
  session->success = false;
  session->details.clear();
}

// -----------------------------------------------------------------------------
// Complete the Apply request after authorization succeeds.
// Called either immediately on session bus or from async callback on system bus.
// -----------------------------------------------------------------------------
static void
complete_apply_request(TransactionSession *session, GDBusMethodInvocation *invocation)
{
  if (!session || !invocation) {
    return;
  }

  bool expected = false;
  if (!session->service->apply_running.compare_exchange_strong(expected, true)) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Another transaction apply is already running."));
    return;
  }

  session->cancelled = false;
  set_transaction_running(session, TransactionStage::APPLY_RUNNING);
  g_dbus_method_invocation_return_value(invocation, nullptr);
  g_idle_add_full(G_PRIORITY_DEFAULT, start_transaction_apply, session, nullptr);
}

// -----------------------------------------------------------------------------
// Context passed to the polkit authorization callback.
// Stores the object path and service pointer so the callback can look up the
// session after authorization finishes or ignore the result during shutdown.
// -----------------------------------------------------------------------------
struct AuthorizationCallbackContext {
  TransactionService *service = nullptr;
  std::string object_path;
};

// -----------------------------------------------------------------------------
// Finish the polkit authorization check for Apply.
// The service and session are looked up again because the client may have
// released the request or the service may have started shutdown while
// authorization was still running.
// -----------------------------------------------------------------------------
static void
on_apply_authorization_result(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  std::unique_ptr<AuthorizationCallbackContext> context(static_cast<AuthorizationCallbackContext *>(user_data));
  if (!context || !context->service) {
    return;
  }

  // Ignore the result after service shutdown has started.
  if (context->service->shutting_down.load()) {
    DNFUI_TRACE("Transaction service authorization callback ignored during shutdown path=%s",
                context->object_path.c_str());
    return;
  }

  // The request may have been released while authorization was running.
  auto it = context->service->transactions.find(context->object_path);
  if (it == context->service->transactions.end()) {
    DNFUI_TRACE("Transaction service authorization callback session not found path=%s", context->object_path.c_str());
    return;
  }

  TransactionSession *session = it->second.get();
  GDBusMethodInvocation *invocation = nullptr;
  bool preview_ready = false;
  bool preview_empty = false;
  {
    std::lock_guard<std::mutex> lock(session->state_mutex);
    if (!session->pending_apply_invocation) {
      return;
    }
    invocation = session->pending_apply_invocation;
    session->pending_apply_invocation = nullptr;
    preview_ready = session->stage == TransactionStage::PREVIEW_READY && session->finished.load() && session->success;
    preview_empty = session->preview.empty();
  }

  // The request must still be ready for apply after authorization completes.
  if (!preview_ready) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Transaction state changed during authorization."));
    g_object_unref(invocation);
    return;
  }

  if (preview_empty) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("No package changes are available."));
    g_object_unref(invocation);
    return;
  }

  GError *error = nullptr;
  PolkitAuthority *authority = POLKIT_AUTHORITY(source_object);
  PolkitAuthorizationResult *result = polkit_authority_check_authorization_finish(authority, res, &error);

  if (!result) {
    std::string error_msg = error ? error->message : _("Authorization check failed.");
    DNFUI_TRACE("Transaction service apply authorization failed path=%s error=%s",
                session->object_path.c_str(),
                error_msg.c_str());
    g_clear_error(&error);
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "%s", error_msg.c_str());
    g_object_unref(invocation);
    return;
  }

  bool authorized = polkit_authorization_result_get_is_authorized(result);
  g_object_unref(result);

  if (!authorized) {
    DNFUI_TRACE("Transaction service apply authorization denied path=%s", session->object_path.c_str());
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "%s", _("Not authorized to apply package transactions."));
    g_object_unref(invocation);
    return;
  }

  DNFUI_TRACE("Transaction service apply authorization granted path=%s", session->object_path.c_str());
  complete_apply_request(session, invocation);
  g_object_unref(invocation);
}

// -----------------------------------------------------------------------------
// Start authorization for an Apply request.
// The session bus skips polkit for development and tests. The system bus starts
// a polkit check and lets the callback reply to the D-Bus method call.
// -----------------------------------------------------------------------------
static bool
start_authorize_apply_request(TransactionSession *session, GDBusMethodInvocation *invocation, std::string &error_out)
{
  error_out.clear();

  if (!session || !session->service || !invocation) {
    error_out = _("Transaction service authorization state is not available.");
    return false;
  }

  // Session bus mode remains available for local development and Docker tests.
  if (session->service->bus_type != G_BUS_TYPE_SYSTEM) {
    DNFUI_TRACE("Transaction service apply authorization skipped (session bus) path=%s", session->object_path.c_str());
    return true;
  }

  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!sender || !*sender) {
    error_out = _("Could not determine the caller identity.");
    return false;
  }

  GError *error = nullptr;
  PolkitAuthority *authority = polkit_authority_get_sync(nullptr, &error);
  if (!authority) {
    error_out = error ? error->message : _("Failed to contact the authorization service.");
    g_clear_error(&error);
    return false;
  }

  PolkitSubject *subject = polkit_system_bus_name_new(sender);

  // Reserve the pending authorization slot before starting the async request.
  // This keeps concurrent Apply calls from starting two authorization checks for
  // the same transaction request object.
  {
    std::lock_guard<std::mutex> lock(session->state_mutex);
    if (session->pending_apply_invocation) {
      error_out = _("Apply authorization is already in progress.");
      g_object_unref(subject);
      g_object_unref(authority);
      return false;
    }
    session->pending_apply_invocation = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
  }

  // Pass enough data for the callback to find the request again.
  auto *callback_context = new AuthorizationCallbackContext();
  callback_context->service = session->service;
  callback_context->object_path = session->object_path;

  DNFUI_TRACE("Transaction service apply authorization start (async) path=%s", session->object_path.c_str());
  polkit_authority_check_authorization(authority,
                                       subject,
                                       kApplyActionId,
                                       nullptr,
                                       POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                       nullptr,
                                       on_apply_authorization_result,
                                       callback_context);

  g_object_unref(subject);
  g_object_unref(authority);

  // Authorization started successfully. The callback will reply to the caller.
  return true;
}

// -----------------------------------------------------------------------------
// Transaction execution
// -----------------------------------------------------------------------------
// Reject requests that passed shared validation but are unsafe for the service to run.
// -----------------------------------------------------------------------------
static bool
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
static gboolean
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
static gboolean
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

// -----------------------------------------------------------------------------
// Per transaction object handling
// -----------------------------------------------------------------------------
// Handle one D-Bus method call for a live transaction request object.
// -----------------------------------------------------------------------------
static void
on_transaction_method_call(GDBusConnection *,
                           const gchar *,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *method_name,
                           GVariant *,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data)
{
  (void)object_path;

  TransactionSession *session = static_cast<TransactionSession *>(user_data);
  if (!session) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Transaction session is not available."));
    return;
  }

  if (g_strcmp0(interface_name, kTransactionInterface) != 0) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", _("Unknown method."));
    return;
  }

  if (g_strcmp0(method_name, "Cancel") == 0) {
    DNFUI_TRACE("Transaction service cancel path=%s", object_path);

    bool has_pending_apply = false;
    TransactionStage stage = TransactionStage::PREVIEW_RUNNING;
    bool finished = false;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      has_pending_apply = session->pending_apply_invocation != nullptr;
      stage = session->stage;
      finished = session->finished.load();
    }

    if (has_pending_apply) {
      g_dbus_method_invocation_return_error(
          invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Cannot cancel while authorization is pending."));
      return;
    }

    if (stage == TransactionStage::APPLY_RUNNING) {
      g_dbus_method_invocation_return_error(invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_NOT_SUPPORTED,
                                            "%s",
                                            _("Cancellation is not supported while apply is running."));
      return;
    }

    // A transaction that has already reached a final state other than PREVIEW_READY
    // (e.g. PREVIEW_FAILED, CANCELLED, APPLY_SUCCEEDED, APPLY_FAILED) has nothing left to cancel.
    // Return success without re-emitting signals to avoid confusing the client.
    if (finished && stage != TransactionStage::PREVIEW_READY) {
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }

    session->cancelled = true;

    // If the preview worker already finished (PREVIEW_READY), clear the flag so
    // emit_transaction_progress can send the cancellation line before the Finished signal.
    // The reset is safe here: PREVIEW_READY is only reached after the worker exits,
    // so no thread is concurrently modifying session->finished.
    if (finished) {
      session->finished = false;
    }

    emit_transaction_progress(session, _("Transaction preview was cancelled."));
    emit_transaction_finished(session, TransactionStage::CANCELLED, false, _("Transaction preview was cancelled."));
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }

  if (g_strcmp0(method_name, "Apply") == 0) {
    bool preview_ready = false;
    bool preview_empty = false;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      preview_ready = session->stage == TransactionStage::PREVIEW_READY && session->finished.load() && session->success;
      preview_empty = session->preview.empty();
    }

    if (!preview_ready) {
      g_dbus_method_invocation_return_error(invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "%s",
                                            _("Transaction preview must succeed before apply can start."));
      return;
    }

    if (preview_empty) {
      g_dbus_method_invocation_return_error(
          invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("No package changes are available."));
      return;
    }

    std::string error_out;
    if (!start_authorize_apply_request(session, invocation, error_out)) {
      DNFUI_TRACE(
          "Transaction service apply authorization start failed path=%s error=%s", object_path, error_out.c_str());
      g_dbus_method_invocation_return_error(
          invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "%s", error_out.c_str());
      return;
    }

    // Session bus: authorization succeeded immediately, proceed with apply.
    // System bus: async authorization in progress, callback will complete the request.
    if (session->service->bus_type != G_BUS_TYPE_SYSTEM) {
      complete_apply_request(session, invocation);
    }

    return;
  }

  if (g_strcmp0(method_name, "Release") == 0) {
    bool has_pending_apply = false;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      has_pending_apply = session->pending_apply_invocation != nullptr;
    }

    if (has_pending_apply) {
      g_dbus_method_invocation_return_error(invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "%s",
                                            _("Transaction request cannot be released while authorization is "
                                              "pending."));
      return;
    }

    if (!session->finished.load()) {
      g_dbus_method_invocation_return_error(invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "%s",
                                            _("Transaction request cannot be released while work is still running."));
      return;
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
    queue_transaction_release(session);
    return;
  }

  if (g_strcmp0(method_name, "GetPreview") == 0) {
    TransactionPreview preview_copy;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);

      if (session->stage != TransactionStage::PREVIEW_READY || !session->finished.load() || !session->success) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Transaction preview is not available."));
        return;
      }

      preview_copy = session->preview;
    }

    GVariantBuilder install_builder;
    GVariantBuilder upgrade_builder;
    GVariantBuilder downgrade_builder;
    GVariantBuilder reinstall_builder;
    GVariantBuilder remove_builder;

    g_variant_builder_init(&install_builder, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&upgrade_builder, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&downgrade_builder, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&reinstall_builder, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&remove_builder, G_VARIANT_TYPE("as"));

    append_transaction_preview_array(install_builder, preview_copy.install);
    append_transaction_preview_array(upgrade_builder, preview_copy.upgrade);
    append_transaction_preview_array(downgrade_builder, preview_copy.downgrade);
    append_transaction_preview_array(reinstall_builder, preview_copy.reinstall);
    append_transaction_preview_array(remove_builder, preview_copy.remove);
    const gint64 disk_space_delta = static_cast<gint64>(preview_copy.disk_space_delta);

    // Return the preview arrays in the same order as the GetPreview D-Bus signature.
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(asasasasasx)",
                                                        &install_builder,
                                                        &upgrade_builder,
                                                        &downgrade_builder,
                                                        &reinstall_builder,
                                                        &remove_builder,
                                                        disk_space_delta));
    return;
  }

  if (g_strcmp0(method_name, "GetResult") == 0) {
    const char *stage_name = nullptr;
    bool finished = false;
    bool success = false;
    std::string details;

    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      stage_name = transaction_stage_name(session->stage);
      finished = session->finished.load();
      success = session->success;
      details = session->details;
    }

    // Return stage, finished, success, and details in the GetResult reply order.
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(sbbs)", stage_name, finished, success, details.c_str()));
    return;
  }

  g_dbus_method_invocation_return_error(
      invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", _("Unknown method."));
}

static const GDBusInterfaceVTable kTransactionVTable = {
  on_transaction_method_call,
  nullptr,
  nullptr,
  nullptr,
};

// -----------------------------------------------------------------------------
// Client disconnect handling
// -----------------------------------------------------------------------------
// Release transaction requests when their owning client disconnects.
// -----------------------------------------------------------------------------
static void
on_client_name_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  (void)connection;

  TransactionService *service = static_cast<TransactionService *>(user_data);
  if (!service || !name) {
    return;
  }

  DNFUI_TRACE("Transaction service client disconnected name=%s", name);

  // Collect matching paths before releasing sessions from the map.
  std::vector<std::string> orphaned_paths;
  for (const auto &[path, session] : service->transactions) {
    if (session->owner_name == name) {
      orphaned_paths.push_back(path);
    }
  }

  for (const auto &path : orphaned_paths) {
    DNFUI_TRACE("Transaction service auto-releasing orphaned session path=%s owner=%s", path.c_str(), name);
    queue_transaction_release(service->transactions[path].get());
  }
}

// -----------------------------------------------------------------------------
// Read the unique bus name of the caller that invoked StartTransaction.
// -----------------------------------------------------------------------------
static bool
get_invocation_sender(GDBusMethodInvocation *invocation, std::string &sender_out, std::string &error_out)
{
  sender_out.clear();
  const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
  if (!sender || !*sender) {
    error_out = _("Could not determine the client bus name.");
    return false;
  }

  sender_out = sender;
  return true;
}

// -----------------------------------------------------------------------------
// Create and register one new transaction request object on the bus.
// Watches the client's unique bus name to auto-release the session if the
// client disconnects without calling Release.
// -----------------------------------------------------------------------------
static TransactionSession *
create_transaction_session(TransactionService *service,
                           const TransactionRequest &request,
                           const std::string &owner_name,
                           std::string &error_out)
{
  error_out.clear();
  if (!service || !service->connection || !service->transaction_node_info) {
    error_out = _("Transaction service is not ready.");
    return nullptr;
  }

  auto session = std::make_unique<TransactionSession>();
  session->service = service;
  session->request = request;
  session->object_path =
      std::string(kManagerObjectPath) + "/requests/" + std::to_string(service->next_transaction_id++);
  session->owner_name = owner_name;

  if (service_request_limit_reached(service, session->owner_name, error_out)) {
    return nullptr;
  }

  GError *error = nullptr;
  session->registration_id = g_dbus_connection_register_object(service->connection,
                                                               session->object_path.c_str(),
                                                               service->transaction_node_info->interfaces[0],
                                                               &kTransactionVTable,
                                                               session.get(),
                                                               nullptr,
                                                               &error);
  if (session->registration_id == 0) {
    error_out = error ? error->message : _("Failed to register transaction object.");
    g_clear_error(&error);
    return nullptr;
  }

  // Watch for client disconnects on the system bus.
  // Session bus tests often use a fresh connection per call, so they opt out.
  // SERVICE_TEST_DISABLE_AUTO_RELEASE also opts out during manual service tests.
  const char *disable_auto_release = g_getenv("SERVICE_TEST_DISABLE_AUTO_RELEASE");
  if (service->bus_type == G_BUS_TYPE_SYSTEM && (!disable_auto_release || g_strcmp0(disable_auto_release, "1") != 0)) {
    session->owner_watch_id = g_bus_watch_name_on_connection(service->connection,
                                                             session->owner_name.c_str(),
                                                             G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                             nullptr,
                                                             on_client_name_vanished,
                                                             service,
                                                             nullptr);
  }

  TransactionSession *raw = session.get();
  service->transactions.emplace(session->object_path, std::move(session));
  return raw;
}

// -----------------------------------------------------------------------------
// Manager object handling
// -----------------------------------------------------------------------------
// Handle StartTransaction calls on the transaction service manager object.
// -----------------------------------------------------------------------------
static void
on_manager_method_call(GDBusConnection *,
                       const gchar *,
                       const gchar *,
                       const gchar *interface_name,
                       const gchar *method_name,
                       GVariant *parameters,
                       GDBusMethodInvocation *invocation,
                       gpointer user_data)
{
  TransactionService *service = static_cast<TransactionService *>(user_data);
  if (!service) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", _("Service is not available."));
    return;
  }

  if (g_strcmp0(interface_name, kManagerInterface) != 0 ||
      (g_strcmp0(method_name, "StartTransaction") != 0 && g_strcmp0(method_name, "StartUpgradeAllTransaction") != 0)) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", _("Unknown method."));
    return;
  }

  TransactionRequest request;
  if (g_strcmp0(method_name, "StartUpgradeAllTransaction") == 0) {
    request.upgrade_all = true;
  } else {
    request = transaction_service_request_from_variant(parameters);
  }
  std::string error_out;

  std::string owner_name;
  if (!get_invocation_sender(invocation, owner_name, error_out)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", error_out.c_str());
    return;
  }

  if (!request.validate(error_out)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "%s", error_out.c_str());
    return;
  }

  if (service_request_limit_reached(service, owner_name, error_out)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", error_out.c_str());
    return;
  }

  if (!validate_transaction_request_for_service(request, error_out)) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "%s", error_out.c_str());
    return;
  }

  DNFUI_TRACE("Transaction service start install=%zu remove=%zu reinstall=%zu upgrade_all=%d",
              request.install.size(),
              request.remove.size(),
              request.reinstall.size(),
              request.upgrade_all ? 1 : 0);

  TransactionSession *session = create_transaction_session(service, request, owner_name, error_out);
  if (!session) {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s", error_out.c_str());
    return;
  }

  g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", session->object_path.c_str()));
  // Queue the preview start after the request object is returned to the caller.
  g_idle_add_full(G_PRIORITY_DEFAULT, start_transaction_preview, session, nullptr);
}

static const GDBusInterfaceVTable kManagerVTable = {
  on_manager_method_call,
  nullptr,
  nullptr,
  nullptr,
};

// -----------------------------------------------------------------------------
// Main loop and bus callbacks
// -----------------------------------------------------------------------------
// Stop the service main loop when the process receives a quit signal.
// -----------------------------------------------------------------------------
static gboolean
on_quit_signal(gpointer user_data)
{
  GMainLoop *loop = static_cast<GMainLoop *>(user_data);
  if (loop) {
    g_main_loop_quit(loop);
  }
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Register the manager object after the service acquires its D-Bus name.
// -----------------------------------------------------------------------------
static void
on_bus_acquired(GDBusConnection *connection, const gchar *, gpointer user_data)
{
  TransactionService *service = static_cast<TransactionService *>(user_data);
  if (!service) {
    return;
  }

  service->connection = G_DBUS_CONNECTION(g_object_ref(connection));
  GError *error = nullptr;
  service->manager_registration_id = g_dbus_connection_register_object(service->connection,
                                                                       kManagerObjectPath,
                                                                       service->manager_node_info->interfaces[0],
                                                                       &kManagerVTable,
                                                                       service,
                                                                       nullptr,
                                                                       &error);

  if (service->manager_registration_id == 0) {
    std::fputs(dnfui_i18n_format(_("Failed to register transaction service object: %s\n"),
                                 error ? error->message : _("unknown"))
                   .c_str(),
               stderr);
    g_clear_error(&error);
    if (service->loop) {
      g_main_loop_quit(service->loop);
    }
    return;
  }

  DNFUI_TRACE("Transaction service bus ready");
}

// -----------------------------------------------------------------------------
// Stop the service if it loses its D-Bus name.
// -----------------------------------------------------------------------------
static void
on_name_lost(GDBusConnection *, const gchar *, gpointer user_data)
{
  TransactionService *service = static_cast<TransactionService *>(user_data);
  DNFUI_TRACE("Transaction service name lost");
  if (service && service->loop) {
    g_main_loop_quit(service->loop);
  }
}

// -----------------------------------------------------------------------------
// Service cleanup
// -----------------------------------------------------------------------------
// Unregister service objects and release GLib resources.
// Pending authorization requests are answered before sessions are destroyed.
// -----------------------------------------------------------------------------
static void
cleanup_service(TransactionService &service)
{
  // Signal that shutdown is in progress to prevent async callbacks from accessing freed memory.
  service.shutting_down = true;
  bool keep_alive_until_exit = false;

  // Reply to any pending authorization requests with an error before destroying sessions.
  for (auto &[path, session] : service.transactions) {
    GDBusMethodInvocation *pending_apply_invocation = nullptr;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      pending_apply_invocation = session->pending_apply_invocation;
      session->pending_apply_invocation = nullptr;
    }

    if (pending_apply_invocation) {
      keep_alive_until_exit = true;
      DNFUI_TRACE("Transaction service cancelling pending authorization during shutdown path=%s", path.c_str());
      g_dbus_method_invocation_return_error(pending_apply_invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "%s",
                                            _("Transaction service is shutting down."));
      g_object_unref(pending_apply_invocation);
    }

    if (!session->finished.load()) {
      keep_alive_until_exit = true;
      session->cancelled = true;
    }
  }

  for (auto &[path, session] : service.transactions) {
    if (session->owner_watch_id != 0) {
      g_bus_unwatch_name(session->owner_watch_id);
      session->owner_watch_id = 0;
    }

    if (service.connection && session->registration_id != 0) {
      g_dbus_connection_unregister_object(service.connection, session->registration_id);
    }
  }

  // Detached worker threads and authorization callbacks still use raw service
  // or session pointers. During shutdown, keep that state allocated until
  // process exit instead of freeing it during teardown.
  if (keep_alive_until_exit) {
    service.keep_alive_until_exit = true;
  } else {
    service.transactions.clear();
  }

  if (service.connection && service.manager_registration_id != 0) {
    g_dbus_connection_unregister_object(service.connection, service.manager_registration_id);
  }

  if (service.owner_id != 0) {
    g_bus_unown_name(service.owner_id);
  }

  if (service.connection) {
    g_object_unref(service.connection);
    service.connection = nullptr;
  }

  if (service.manager_node_info) {
    g_dbus_node_info_unref(service.manager_node_info);
    service.manager_node_info = nullptr;
  }

  if (service.transaction_node_info) {
    g_dbus_node_info_unref(service.transaction_node_info);
    service.transaction_node_info = nullptr;
  }

  if (service.loop) {
    g_main_loop_unref(service.loop);
    service.loop = nullptr;
  }
}

// -----------------------------------------------------------------------------
// Transaction service entrypoint
// -----------------------------------------------------------------------------
// Build the service runtime state, own the bus name, and run the main loop.
// -----------------------------------------------------------------------------
int
transaction_service_run(const TransactionServiceOptions &options)
{
  auto service = std::make_unique<TransactionService>();
  service->bus_type = options.bus_type;
  service->loop = g_main_loop_new(nullptr, FALSE);
  service->main_context = g_main_loop_get_context(service->loop);

  GError *error = nullptr;
  service->manager_node_info = g_dbus_node_info_new_for_xml(kTransactionServiceManagerIntrospectionXml, &error);
  if (!service->manager_node_info) {
    std::fputs(
        dnfui_i18n_format(_("Failed to parse manager introspection XML: %s\n"), error ? error->message : _("unknown"))
            .c_str(),
        stderr);
    g_clear_error(&error);
    cleanup_service(*service);
    return 1;
  }

  service->transaction_node_info = g_dbus_node_info_new_for_xml(kTransactionServiceRequestIntrospectionXml, &error);
  if (!service->transaction_node_info) {
    std::fputs(dnfui_i18n_format(_("Failed to parse transaction introspection XML: %s\n"),
                                 error ? error->message : _("unknown"))
                   .c_str(),
               stderr);
    g_clear_error(&error);
    cleanup_service(*service);
    return 1;
  }

  service->owner_id = g_bus_own_name(options.bus_type,
                                     kServiceName,
                                     G_BUS_NAME_OWNER_FLAGS_NONE,
                                     on_bus_acquired,
                                     nullptr,
                                     on_name_lost,
                                     service.get(),
                                     nullptr);

  g_unix_signal_add(SIGINT, on_quit_signal, service->loop);
  g_unix_signal_add(SIGTERM, on_quit_signal, service->loop);

  DNFUI_TRACE("Transaction service run loop start");
  g_main_loop_run(service->loop);
  DNFUI_TRACE("Transaction service run loop stop");

  cleanup_service(*service);
  if (service->keep_alive_until_exit) {
    (void)service.release();
  }
  return 0;
}
