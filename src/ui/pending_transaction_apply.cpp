// -----------------------------------------------------------------------------
// pending_transaction_apply.cpp
// Pending transaction preview and apply helpers
//
// Keeps service preview, service apply, and post-transaction refresh code separate from package action button handlers.
// -----------------------------------------------------------------------------
#include "pending_transaction_apply.hpp"

#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "package_query_controller.hpp"
#include "pending_transaction_controller.hpp"
#include "pending_transaction_request.hpp"
#include "pending_transaction_view.hpp"
#include "transaction_progress.hpp"
#include "transaction_review_dialog.hpp"
#include "transaction_service_client.hpp"
#include "ui_helpers.hpp"
#include "widgets.hpp"
#include "widgets_internal.hpp"

#include <utility>

// Data owned by the apply task.
// The worker uses transaction_path to call the service, and progress_window receives text lines while the service
// applies.
struct ApplyTaskData {
  std::string transaction_path;
  TransactionProgressWindow *progress_window;
};

// Data passed to the transaction preview worker.
struct PreviewTaskData {
  TransactionRequest request;
  TransactionPreview preview;
  std::string transaction_path;
  bool transaction_path_transferred = false;
};

// -----------------------------------------------------------------------------
// Free data owned by one apply task.
// -----------------------------------------------------------------------------
static void
apply_task_data_free(gpointer p)
{
  ApplyTaskData *d = static_cast<ApplyTaskData *>(p);
  if (!d) {
    return;
  }

  if (!d->transaction_path.empty()) {
    transaction_service_client_release_request_async(d->transaction_path);
  }

  // Drop the task reference. Queued progress callbacks retain the window state
  // separately until their GTK update has run.
  transaction_progress_release(d->progress_window);
  delete d;
}

// -----------------------------------------------------------------------------
// Free data owned by one preview task.
// -----------------------------------------------------------------------------
static void
preview_task_data_free(gpointer p)
{
  PreviewTaskData *d = static_cast<PreviewTaskData *>(p);
  if (d && !d->transaction_path.empty() && !d->transaction_path_transferred) {
    transaction_service_client_release_request_async(d->transaction_path);
  }
  delete d;
}

// -----------------------------------------------------------------------------
// Release any prepared service preview because the pending actions changed.
// -----------------------------------------------------------------------------
void
pending_transaction_invalidate_service_preview(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  if (!widgets->transaction.preview_transaction_path.empty()) {
    // Drop the prepared service request without waiting on D-Bus from GTK.
    transaction_service_client_release_request_async(widgets->transaction.preview_transaction_path);
  }

  widgets->transaction.preview_transaction_path.clear();
  widgets->transaction.preview_upgrade_all = false;
}

// -----------------------------------------------------------------------------
// Return the status text shown while a preview request is running.
// -----------------------------------------------------------------------------
const char *
pending_transaction_preview_busy_message()
{
  return _("Wait for the current transaction preview to finish.");
}

// -----------------------------------------------------------------------------
// Return true when a preview request is running.
// -----------------------------------------------------------------------------
bool
pending_transaction_preview_is_busy(SearchWidgets *widgets)
{
  return widgets && widgets->transaction.preview_request_in_progress;
}

// -----------------------------------------------------------------------------
// Return the status text shown while an apply request is running.
// -----------------------------------------------------------------------------
const char *
pending_transaction_apply_busy_message()
{
  return _("Wait for the current transaction to finish.");
}

// -----------------------------------------------------------------------------
// Return true when an apply request is running.
// -----------------------------------------------------------------------------
bool
pending_transaction_apply_is_busy(SearchWidgets *widgets)
{
  return widgets && widgets->transaction.apply_in_progress;
}

// -----------------------------------------------------------------------------
// Enable or disable the controls that can start a new transaction preview.
// -----------------------------------------------------------------------------
void
pending_transaction_set_preview_controls_sensitive(SearchWidgets *widgets, bool sensitive)
{
  if (!widgets) {
    return;
  }

  if (widgets->transaction.pending_list) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.pending_list), sensitive);
  }
  if (widgets->transaction.upgrade_all_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.upgrade_all_button), sensitive);
  }

  if (!sensitive) {
    if (widgets->transaction.apply_button) {
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.apply_button), FALSE);
    }
    if (widgets->transaction.clear_pending_button) {
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.clear_pending_button), FALSE);
    }
    return;
  }

  pending_transaction_refresh_pending_tab(widgets);
}

// -----------------------------------------------------------------------------
// Enable or disable controls while a preview request is running.
// -----------------------------------------------------------------------------
static void
set_preview_request_busy_state(SearchWidgets *widgets, bool busy)
{
  if (!widgets) {
    return;
  }

  widgets->transaction.preview_request_in_progress = busy;
  pending_transaction_set_preview_controls_sensitive(widgets, !busy);

  if (busy) {
    ui_helpers_set_icon_button(widgets->transaction.apply_button, "object-select-symbolic", _("Preparing Preview..."));
    return;
  }
}

// -----------------------------------------------------------------------------
// Finish the post-transaction repository rebuild.
// -----------------------------------------------------------------------------
static void
rebuild_after_tx_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  GError *error = nullptr;
  gboolean ok = g_task_propagate_boolean(task, &error);

  if (!ok && error) {
    ui_helpers_set_status(widgets->query.status_label, error->message, "red");
    g_error_free(error);
    return;
  }

  // Transaction follow-up rebuilds produce a new Base generation.
  // Cached search result rows must be discarded before the next search.
  package_query_clear_search_cache();

  // Repopulate the currently visible package view so rows removed by the transaction disappear without a manual reload.
  package_query_reload_current_view(widgets);
}

// -----------------------------------------------------------------------------
// Rebuild repository data after a transaction completes.
// -----------------------------------------------------------------------------
static void
rebuild_after_tx_async(SearchWidgets *widgets)
{
  // Once the post-transaction rebuild begins, stop serving cached search results from the pre-transaction Base
  // generation.
  package_query_clear_search_cache();

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, rebuild_after_tx_finished);
  g_task_run_in_thread(task, widgets_on_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start applying the transaction after the user confirms the summary.
// The service streams progress back through the callback passed to the client.
// -----------------------------------------------------------------------------
static void
start_apply_transaction(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  if (widgets->transaction.preview_transaction_path.empty()) {
    ui_helpers_set_status(widgets->query.status_label, _("No prepared transaction request is available."), "red");
    return;
  }

  widgets->transaction.apply_in_progress = true;
  pending_transaction_set_preview_controls_sensitive(widgets, false);

  ApplyTaskData *td = new ApplyTaskData;
  // Apply now owns this dnf5daemon session path.
  // Pending action changes must not release it while the daemon is applying the transaction.
  td->transaction_path = std::move(widgets->transaction.preview_transaction_path);
  widgets->transaction.preview_transaction_path.clear();
  size_t pending_count = widgets->transaction.preview_upgrade_all ? 0 : widgets->transaction.actions.size();
  td->progress_window = transaction_progress_create_window(widgets, pending_count);
  // Keep the progress state alive while the apply worker may still receive service progress.
  // Closing the window only removes the GTK widgets.
  transaction_progress_retain(td->progress_window);

  transaction_progress_append(td->progress_window, _("Queued transaction request."));
  const char *status_message = widgets->transaction.preview_upgrade_all
      ? _("Applying package upgrades. See transaction window for details.")
      : _("Applying pending changes. See transaction window for details.");
  ui_helpers_set_status(widgets->query.status_label, status_message, "blue");
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(
      widgets, c, +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        ApplyTaskData *td = static_cast<ApplyTaskData *>(g_task_get_task_data(task));
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        if (widgets_task_should_skip_completion(task, widgets)) {
          return;
        }

        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);

        // Release this task's spinner slot.
        widgets_spinner_release(widgets->query.spinner);
        widgets->transaction.apply_in_progress = false;

        transaction_progress_finish(td ? td->progress_window : nullptr, success, "");

        if (success) {
          pending_transaction_invalidate_service_preview(widgets);
          // Clear pending actions and restore the transaction controls.
          widgets->transaction.actions.clear();
          pending_transaction_set_preview_controls_sensitive(widgets, true);

          ui_helpers_set_status(widgets->query.status_label, _("Transaction successful."), "green");

          // Rebuild repository data and refresh package state in the background.
          rebuild_after_tx_async(widgets);
        } else {
          pending_transaction_invalidate_service_preview(widgets);
          pending_transaction_set_preview_controls_sensitive(widgets, true);
          std::string details = error ? error->message : _("Transaction failed.");
          ui_helpers_set_status(widgets->query.status_label, details.c_str(), "red");
          // Show the full backend error in a copyable dialog instead of only in the status bar.
          transaction_review_show_error_dialog(widgets,
                                               _("Transaction Failed"),
                                               _("The transaction could not be completed. Review the details below."),
                                               details);
          if (error) {
            g_error_free(error);
          }
        }
      });

  g_task_set_task_data(task, td, apply_task_data_free);

  g_task_run_in_thread(
      task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *) {
        ApplyTaskData *td = static_cast<ApplyTaskData *>(task_data);
        std::string err;
        // This callback runs on the apply worker thread when the client receives a service Progress signal.
        // transaction_progress_append queues the actual GTK update onto the main thread.
        bool ok = transaction_service_client_apply_started_request(
            td->transaction_path,
            [td](const std::string &message) { transaction_progress_append(td->progress_window, message); },
            err);
        if (ok) {
          g_task_return_boolean(t, TRUE);
        } else {
          g_task_return_error(t, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, err.c_str()));
        }
      });

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Prepare a service-backed transaction preview and show the confirmation dialog.
// -----------------------------------------------------------------------------
static void
start_preview_request(SearchWidgets *widgets, TransactionRequest request)
{
  pending_transaction_invalidate_service_preview(widgets);
  widgets->transaction.preview_upgrade_all = request.upgrade_all;
  ui_helpers_set_status(widgets->query.status_label, _("Preparing transaction preview..."), "blue");
  widgets_spinner_acquire(widgets->query.spinner);
  set_preview_request_busy_state(widgets, true);

  PreviewTaskData *td = new PreviewTaskData();
  td->request = std::move(request);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(
      widgets, c, +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        PreviewTaskData *td = static_cast<PreviewTaskData *>(g_task_get_task_data(task));

        if (widgets_task_should_skip_completion(task, widgets)) {
          return;
        }

        widgets_spinner_release(widgets->query.spinner);
        set_preview_request_busy_state(widgets, false);

        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);
        if (!success || !td) {
          const char *status_message =
              error && error->message ? error->message : _("Unable to prepare transaction preview.");
          widgets->transaction.preview_upgrade_all = false;
          ui_helpers_set_status(widgets->query.status_label, status_message, "red");
          transaction_review_show_error_dialog(widgets,
                                               _("Transaction Preview Failed"),
                                               _("The transaction could not be prepared. Review the details below."),
                                               status_message);
          if (error) {
            g_error_free(error);
          }
          return;
        }

        if (td->preview.empty()) {
          if (!td->transaction_path.empty()) {
            transaction_service_client_release_request_async(td->transaction_path);
            td->transaction_path.clear();
          }
          widgets->transaction.preview_transaction_path.clear();
          widgets->transaction.preview_upgrade_all = false;
          ui_helpers_set_status(widgets->query.status_label, _("All packages are already up to date."), "green");
          return;
        }

        widgets->transaction.preview_transaction_path = td->transaction_path;
        widgets->transaction.preview_upgrade_all = td->request.upgrade_all;
        td->transaction_path_transferred = true;
        transaction_review_show_summary_dialog(
            widgets, td->preview, start_apply_transaction, pending_transaction_invalidate_service_preview);
      });

  g_task_set_task_data(task, td, preview_task_data_free);
  g_task_run_in_thread(
      task, +[](GTask *task, gpointer, gpointer task_data, GCancellable *) {
        PreviewTaskData *td = static_cast<PreviewTaskData *>(task_data);
        std::string error;
        bool ok = false;
        if (td && td->request.upgrade_all) {
          ok = transaction_service_client_preview_upgrade_all_request(td->preview, td->transaction_path, error);
        } else if (td) {
          ok = transaction_service_client_preview_request(td->request, td->preview, td->transaction_path, error);
        }

        if (!ok) {
          g_task_return_new_error(task,
                                  G_IO_ERROR,
                                  G_IO_ERROR_FAILED,
                                  "%s",
                                  error.empty() ? _("Unable to prepare transaction preview.") : error.c_str());
          return;
        }

        g_task_return_boolean(task, TRUE);
      });

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Prepare a transaction preview for all package upgrades.
// -----------------------------------------------------------------------------
void
pending_transaction_on_upgrade_all_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  if (pending_transaction_apply_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_apply_busy_message(), "blue");
    return;
  }

  if (!widgets->transaction.actions.empty()) {
    ui_helpers_set_status(
        widgets->query.status_label, _("Clear pending package actions before upgrading all packages."), "blue");
    return;
  }

  TransactionRequest request;
  request.upgrade_all = true;
  std::string error;
  if (!request.validate(error)) {
    ui_helpers_set_status(widgets->query.status_label, error.c_str(), "red");
    return;
  }

  start_preview_request(widgets, std::move(request));
}

// -----------------------------------------------------------------------------
// Prepare a transaction preview and ask the user to confirm it.
// -----------------------------------------------------------------------------
void
pending_transaction_on_apply_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  if (pending_transaction_apply_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_apply_busy_message(), "blue");
    return;
  }

  if (widgets->transaction.actions.empty()) {
    ui_helpers_set_status(widgets->query.status_label, _("No pending changes."), "gray");
    return;
  }

  TransactionRequest request;
  std::string error;
  if (!pending_transaction_build_request(widgets->transaction.actions, request, error)) {
    ui_helpers_set_status(widgets->query.status_label, error.c_str(), "red");
    return;
  }

  // Refuse self-protected transactions before asking the service to preview them.
  if (!pending_transaction_validate_request(request, error)) {
    ui_helpers_set_status(widgets->query.status_label, error.c_str(), "red");
    return;
  }

  start_preview_request(widgets, std::move(request));
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
