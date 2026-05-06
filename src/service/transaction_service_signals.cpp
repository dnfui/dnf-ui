// -----------------------------------------------------------------------------
// transaction_service_signals.cpp
// Emits Progress and Finished signals for transaction request objects.
// Worker threads queue updates here so D-Bus signals are sent from the main loop.
// -----------------------------------------------------------------------------
#include "transaction_service_internal.hpp"

#include <memory>

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
// Send one progress line to the GUI. The GUI subscribed to Progress on this
// request object before it called Apply.
// -----------------------------------------------------------------------------
void
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
void
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
// Backend work runs on a worker thread. D-Bus signals are emitted from the
// service main loop, so progress lines are queued here first.
// -----------------------------------------------------------------------------
void
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
void
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
void
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
