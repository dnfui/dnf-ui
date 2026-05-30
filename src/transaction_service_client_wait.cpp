// -----------------------------------------------------------------------------
// transaction_service_client_wait.cpp
// Wait and progress signal handling for the GUI-side transaction service client.
// This file owns Finished and Progress signal subscriptions so the public client
// flow can stay focused on preview and apply behavior.
// -----------------------------------------------------------------------------
#include "transaction_service_client_internal.hpp"

#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "service/transaction_service_dbus.hpp"

#include <glib.h>

namespace {

// -----------------------------------------------------------------------------
// State shared between wait_for_transaction_stage and its Finished signal handler.
// -----------------------------------------------------------------------------
struct FinishedWaitState {
  bool received = false;
  bool service_disappeared = false;
  std::string stage;
  bool success = false;
  std::string details;
};

// -----------------------------------------------------------------------------
// Receive one Progress signal from the service and pass its text to the UI
// callback supplied by the apply task.
// -----------------------------------------------------------------------------
void
on_transaction_progress_signal(GDBusConnection *,
                               const gchar *,
                               const gchar *,
                               const gchar *,
                               const gchar *,
                               GVariant *parameters,
                               gpointer user_data)
{
  auto *forwarder = static_cast<TransactionServiceProgressForwarder *>(user_data);
  if (!forwarder || !forwarder->progress_callback || !*forwarder->progress_callback) {
    return;
  }

  const gchar *line = nullptr;
  g_variant_get(parameters, "(&s)", &line);
  if (!line || !*line) {
    return;
  }

  DNFUI_TRACE("Transaction service client progress line=%s", line);
  (*forwarder->progress_callback)(line);
}

} // namespace

// -----------------------------------------------------------------------------
// Wait until the service transaction request leaves the given running stage.
// Subscribes to the Finished D-Bus signal so the wait wakes up immediately
// when the service reports a final state.
// The context must already be the thread-default context of the calling thread.
// That lets signal callbacks run on the same context that is being iterated.
// -----------------------------------------------------------------------------
bool
transaction_service_client_wait_for_transaction_stage(GDBusConnection *connection,
                                                      const std::string &transaction_path,
                                                      const char *running_stage,
                                                      GMainContext *context,
                                                      TransactionServiceResult &result_out,
                                                      std::string &error_out)
{
  error_out.clear();

  // Subscribe to Finished before the first state poll.
  // This covers the short gap where the service could finish after the request
  // starts but before the client begins waiting for the result.
  FinishedWaitState wait_state;
  guint finished_id = g_dbus_connection_signal_subscribe(
      connection,
      kTransactionServiceName,
      kTransactionServiceRequestInterface,
      "Finished",
      transaction_path.c_str(),
      nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE,
      +[](GDBusConnection *,
          const gchar *,
          const gchar *,
          const gchar *,
          const gchar *,
          GVariant *parameters,
          gpointer user_data) {
        auto *state = static_cast<FinishedWaitState *>(user_data);
        const gchar *stage = nullptr;
        gboolean success = FALSE;
        const gchar *details = nullptr;
        g_variant_get(parameters, "(&sbs)", &stage, &success, &details);
        state->stage = stage ? stage : "";
        state->success = success;
        state->details = details ? details : "";
        state->received = true;
      },
      &wait_state,
      nullptr);

  guint name_owner_changed_id = g_dbus_connection_signal_subscribe(
      connection,
      "org.freedesktop.DBus",
      "org.freedesktop.DBus",
      "NameOwnerChanged",
      "/org/freedesktop/DBus",
      kTransactionServiceName,
      G_DBUS_SIGNAL_FLAGS_NONE,
      +[](GDBusConnection *,
          const gchar *,
          const gchar *,
          const gchar *,
          const gchar *,
          GVariant *parameters,
          gpointer user_data) {
        auto *state = static_cast<FinishedWaitState *>(user_data);
        const gchar *name = nullptr;
        const gchar *old_owner = nullptr;
        const gchar *new_owner = nullptr;
        g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
        if (name && g_strcmp0(name, kTransactionServiceName) == 0 && old_owner && *old_owner &&
            (!new_owner || !*new_owner)) {
          state->service_disappeared = true;
        }
      },
      &wait_state,
      nullptr);

  // One initial GetResult call handles the case where the service already
  // reached a final state before our Finished subscription was registered.
  if (!transaction_service_client_get_transaction_result(connection, transaction_path, result_out, error_out)) {
    g_dbus_connection_signal_unsubscribe(connection, name_owner_changed_id);
    g_dbus_connection_signal_unsubscribe(connection, finished_id);
    return false;
  }

  // Block on context until the Finished signal fires or the initial poll
  // already shows a final state. Any other signals pending on context
  // (such as Progress callbacks in the apply path) are also dispatched here.
  while (!wait_state.received && !wait_state.service_disappeared && result_out.stage == running_stage &&
         !result_out.finished) {
    g_main_context_iteration(context, TRUE);
  }

  g_dbus_connection_signal_unsubscribe(connection, name_owner_changed_id);
  g_dbus_connection_signal_unsubscribe(connection, finished_id);

  // Return a normal client error if the service disappears while the client
  // is still waiting for the final state signal.
  if (wait_state.service_disappeared && !wait_state.received) {
    error_out = _("Transaction service disappeared while waiting for the result.");
    return false;
  }

  // Fill the result from the final state signal and avoid a second GetResult call.
  if (wait_state.received) {
    result_out.stage = wait_state.stage;
    result_out.finished = true;
    result_out.success = wait_state.success;
    result_out.details = wait_state.details;
  }

  DNFUI_TRACE("Transaction service client stage path=%s stage=%s finished=%d success=%d",
              transaction_path.c_str(),
              result_out.stage.c_str(),
              result_out.finished ? 1 : 0,
              result_out.success ? 1 : 0);

  return true;
}

// -----------------------------------------------------------------------------
// Wait for a started preview request and read its structured preview.
// -----------------------------------------------------------------------------
bool
transaction_service_client_wait_for_started_transaction_preview(GDBusConnection *connection,
                                                                const std::string &transaction_path,
                                                                TransactionPreview &preview_out,
                                                                std::string &error_out)
{
  TransactionServiceResult result;

  // Create a dedicated main context for the duration of the preview wait.
  // The Finished signal subscription inside wait_for_transaction_stage is
  // dispatched on the thread-default context, so it and the iterated context
  // must be the same object.
  GMainContext *wait_context = g_main_context_new();
  g_main_context_push_thread_default(wait_context);
  bool stage_ok = transaction_service_client_wait_for_transaction_stage(
      connection, transaction_path, "preview-running", wait_context, result, error_out);
  g_main_context_pop_thread_default(wait_context);
  g_main_context_unref(wait_context);

  if (!stage_ok) {
    std::string release_error;
    transaction_service_client_release_transaction_request(connection, transaction_path, release_error);
    return false;
  }

  if (result.stage != "preview-ready" || !result.finished || !result.success) {
    error_out = result.details.empty() ? _("Privileged transaction preview failed.") : result.details;
    DNFUI_TRACE(
        "Transaction service client preview failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
    std::string release_error;
    transaction_service_client_release_transaction_request(connection, transaction_path, release_error);
    return false;
  }

  if (!transaction_service_client_get_transaction_preview(connection, transaction_path, preview_out, error_out)) {
    DNFUI_TRACE(
        "Transaction service client get preview failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
    std::string release_error;
    transaction_service_client_release_transaction_request(connection, transaction_path, release_error);
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Subscribe to Progress signals for one request object.
// -----------------------------------------------------------------------------
guint
transaction_service_client_subscribe_progress(GDBusConnection *connection,
                                              const std::string &transaction_path,
                                              TransactionServiceProgressForwarder *progress_forwarder)
{
  return g_dbus_connection_signal_subscribe(connection,
                                            kTransactionServiceName,
                                            kTransactionServiceRequestInterface,
                                            "Progress",
                                            transaction_path.c_str(),
                                            nullptr,
                                            G_DBUS_SIGNAL_FLAGS_NONE,
                                            on_transaction_progress_signal,
                                            progress_forwarder,
                                            nullptr);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
