// -----------------------------------------------------------------------------
// pending_transaction_apply.cpp
// Pending transaction preview and apply helpers
//
// Keeps service preview, service apply, and post-transaction refresh code separate from package action button handlers.
// -----------------------------------------------------------------------------
#include "ui/transaction/pending_transaction_apply.hpp"

#include "dnf_backend/base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "transaction/transaction_preview.hpp"
#include "ui/details/package_details_controller.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/history/transaction_history_view.hpp"
#include "ui/package_query/package_query_controller.hpp"
#include "ui/package_query/package_query_controller_internal.hpp"
#include "ui/transaction/pending_transaction_controller.hpp"
#include "ui/transaction/pending_transaction_request.hpp"
#include "ui/transaction/pending_transaction_view.hpp"
#include "ui/refresh/repository_refresh_controller.hpp"
#include "ui/transaction/transaction_progress.hpp"
#include "ui/transaction/transaction_dialogs.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <utility>
#include <vector>

// Data owned by the apply task.
// The worker uses transaction_path to call the service, and progress_window receives text lines while the service
// applies.
struct ApplyTaskData {
  MainWindowUiState *widgets = nullptr;
  std::string transaction_path;
  TransactionProgressWindow *progress_window;
  bool transaction_started = false;
  bool offline = false;
};

// Data passed to the transaction preview worker.
struct PreviewTaskData {
  MainWindowUiState *widgets = nullptr;
  TransactionRequest request;
  TransactionPreview preview;
  std::string transaction_path;
  bool transaction_path_transferred = false;
};

#ifdef DNFUI_DEBUG_TRACE
static long long
elapsed_ms_since(gint64 started_at_us)
{
  return static_cast<long long>((g_get_monotonic_time() - started_at_us) / 1000);
}
#endif

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
// Disable the main window while the daemon applies a transaction.
// The progress window is not modal, so the history browser can still be closed.
// -----------------------------------------------------------------------------
static void
set_main_window_sensitive_for_apply(MainWindowUiState *widgets, bool sensitive)
{
  if (!widgets || !widgets->query.entry) {
    return;
  }

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
  if (root && GTK_IS_WINDOW(root)) {
    gtk_widget_set_sensitive(GTK_WIDGET(root), sensitive);
  }
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
pending_transaction_invalidate_service_preview(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  if (!widgets->transaction_state.preview_transaction_path.empty()) {
    // Drop the prepared service request without waiting on D-Bus from GTK.
    transaction_service_client_release_request_async(widgets->transaction_state.preview_transaction_path);
  }

  widgets->transaction_state.preview_transaction_path.clear();
  widgets->transaction_state.preview_upgrade_all = false;
  widgets->transaction_state.preview_requires_offline = false;
}

// -----------------------------------------------------------------------------
// Close a prepared preview from the summary dialog without applying it.
// -----------------------------------------------------------------------------
static void
pending_transaction_cancel_service_preview(MainWindowUiState *widgets)
{
  pending_transaction_invalidate_service_preview(widgets);
  if (widgets) {
    ui_helpers_set_status(widgets->query.status_label, _("Ready."), "gray");
  }
}

// -----------------------------------------------------------------------------
// Treat local package state as uncertain after Apply has been sent to dnf5daemon.
// -----------------------------------------------------------------------------
static void
mark_package_state_uncertain_after_apply(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  DaemonUpgradeState::instance().mark_stale();
  package_query_refresh_upgrade_indicator(widgets);
  package_query_clear_search_cache();
  package_query_clear_displayed_upgradeable_table(widgets);
}

// -----------------------------------------------------------------------------
// Clear package data that may describe the system before an attempted Apply.
// -----------------------------------------------------------------------------
static void
clear_package_view_after_apply_refresh_failure(MainWindowUiState *widgets, const char *status_message)
{
  if (!widgets) {
    return;
  }

  BaseManager::instance().drop_cached_base();
  package_query_clear_search_cache();
  package_query_clear_duration_label(widgets);
  widgets->query_state.reload_selected_nevra.clear();
  widgets->results.selected_nevra.clear();
  package_table_fill_package_view(widgets, std::vector<PackageRow> {});
  package_details_reset_details_view(widgets);
  pending_transaction_update_action_button_labels_for_selection(widgets, "", "", "", false);
  ui_helpers_set_status(widgets->query.status_label, status_message, "red");
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
// Return the status text shown when the daemon resolved no transaction changes.
// -----------------------------------------------------------------------------
static const char *
empty_preview_status_message(const TransactionRequest &request)
{
  if (request.upgrade_all) {
    return _("All packages are already up to date.");
  }

  return _("No transaction changes were returned.");
}

// -----------------------------------------------------------------------------
// Return true when a preview request is running.
// -----------------------------------------------------------------------------
bool
pending_transaction_preview_is_busy(MainWindowUiState *widgets)
{
  return widgets && widgets->transaction_state.preview_request_in_progress;
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
pending_transaction_apply_is_busy(MainWindowUiState *widgets)
{
  return widgets && widgets->transaction_state.apply_in_progress;
}

// -----------------------------------------------------------------------------
// Enable or disable the controls that can start a new transaction preview.
// -----------------------------------------------------------------------------
void
pending_transaction_set_preview_controls_sensitive(MainWindowUiState *widgets, bool sensitive)
{
  if (!widgets) {
    return;
  }

  if (widgets->transaction_widgets.pending_list) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.pending_list), sensitive);
  }
  if (widgets->transaction_widgets.upgrade_all_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.upgrade_all_button), sensitive);
  }
  if (widgets->transaction_widgets.mark_listed_upgrades_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.mark_listed_upgrades_button), sensitive);
  }

  if (!sensitive) {
    if (widgets->transaction_widgets.apply_button) {
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.apply_button), FALSE);
    }
    if (widgets->transaction_widgets.clear_pending_button) {
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.clear_pending_button), FALSE);
    }
    return;
  }

  pending_transaction_refresh_pending_tab(widgets);
}

// -----------------------------------------------------------------------------
// Enable or disable controls while a preview request is running.
// -----------------------------------------------------------------------------
static void
set_preview_request_busy_state(MainWindowUiState *widgets, bool busy)
{
  if (!widgets) {
    return;
  }

  widgets->transaction_state.preview_request_in_progress = busy;
  pending_transaction_set_preview_controls_sensitive(widgets, !busy);

  if (busy) {
    ui_helpers_set_icon_button(
        widgets->transaction_widgets.apply_button, "system-run-symbolic", _("Preparing Preview..."));
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
#ifdef DNFUI_DEBUG_TRACE
  auto *rebuild_started_at_us = static_cast<gint64 *>(g_task_get_task_data(task));
#endif
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  GError *error = nullptr;
  // repository_refresh_on_rebuild_task() returns the refreshed Base state as a pointer.
  BaseRepoState *refresh_state = static_cast<BaseRepoState *>(g_task_propagate_pointer(task, &error));

  if (!refresh_state) {
#ifdef DNFUI_DEBUG_TRACE
    if (rebuild_started_at_us) {
      DNFUI_TRACE("Post-apply Base rebuild failed elapsed_ms=%lld", elapsed_ms_since(*rebuild_started_at_us));
    }
#endif
    const char *message = error ? error->message : _("Repository refresh failed after transaction.");
    clear_package_view_after_apply_refresh_failure(widgets, message);
    if (error) {
      g_error_free(error);
    }
    package_query_maybe_start_upgrade_indicator_refresh(widgets);
    return;
  }

  delete refresh_state;

#ifdef DNFUI_DEBUG_TRACE
  if (rebuild_started_at_us) {
    DNFUI_TRACE("Post-apply Base rebuild done elapsed_ms=%lld", elapsed_ms_since(*rebuild_started_at_us));
  }
#endif

  // Transaction follow-up rebuilds produce a new Base generation.
  // Cached search result rows must be discarded before the next search.
  package_query_clear_search_cache();

  // Repopulate the currently visible package view so rows removed by the transaction disappear without a manual reload.
  package_query_reload_current_view(widgets);
  package_query_start_upgrade_indicator_refresh(widgets);
}

// -----------------------------------------------------------------------------
// Rebuild repository data after a transaction completes.
// -----------------------------------------------------------------------------
static void
rebuild_after_tx_async(MainWindowUiState *widgets)
{
  // Once the post-transaction rebuild begins, stop serving cached search results from the pre-transaction Base
  // generation.
  package_query_clear_search_cache();

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, rebuild_after_tx_finished);
#ifdef DNFUI_DEBUG_TRACE
  auto *rebuild_started_at_us = new gint64(g_get_monotonic_time());
  g_task_set_task_data(task, rebuild_started_at_us, [](gpointer p) { delete static_cast<gint64 *>(p); });
#endif
  g_task_run_in_thread(task, repository_refresh_on_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Refresh table status labels after Apply and trace how long the UI update took.
// -----------------------------------------------------------------------------
static void
refresh_package_table_statuses_after_apply(MainWindowUiState *widgets)
{
#ifdef DNFUI_DEBUG_TRACE
  const gint64 started_at_us = g_get_monotonic_time();
#endif
  package_table_refresh_statuses(widgets);
#ifdef DNFUI_DEBUG_TRACE
  DNFUI_TRACE("Post-apply package table status refresh done elapsed_ms=%lld", elapsed_ms_since(started_at_us));
#endif
}

// -----------------------------------------------------------------------------
// Start applying the transaction after the user confirms the summary.
// The service streams progress back through the callback passed to the client.
// -----------------------------------------------------------------------------
static void
start_apply_transaction(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  if (widgets->transaction_state.preview_transaction_path.empty()) {
    ui_helpers_set_status(widgets->query.status_label, _("No prepared transaction request is available."), "red");
    return;
  }

  package_details_cancel_active_load(widgets);
  transaction_history_set_transaction_busy(true);
  set_main_window_sensitive_for_apply(widgets, false);

  widgets->transaction_state.apply_in_progress = true;
  pending_transaction_set_preview_controls_sensitive(widgets, false);

  ApplyTaskData *td = new ApplyTaskData;
  td->widgets = widgets;
  td->offline = widgets->transaction_state.preview_requires_offline;
  // Apply now owns this dnf5daemon session path.
  // Pending action changes must not release it while the daemon is applying the transaction.
  td->transaction_path = std::move(widgets->transaction_state.preview_transaction_path);
  widgets->transaction_state.preview_transaction_path.clear();
  size_t pending_count = widgets->transaction_state.preview_upgrade_all ? 0 : widgets->transaction_state.actions.size();
  td->progress_window = transaction_progress_create_window(widgets, pending_count, td->offline);
  // Keep the progress state alive while the apply worker may still receive service progress.
  // Closing the window only removes the GTK widgets.
  transaction_progress_retain(td->progress_window);

  transaction_progress_append(td->progress_window, _("Queued transaction request."));
  const char *status_message = widgets->transaction_state.preview_upgrade_all
      ? _("Applying package upgrades. See transaction window for details.")
      : _("Applying pending changes. See transaction window for details.");
  if (td->offline) {
    status_message = _("Preparing changes for reboot. See transaction window for details.");
  }
  ui_helpers_set_status(widgets->query.status_label, status_message, "blue");
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(
      widgets, c, +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        ApplyTaskData *td = static_cast<ApplyTaskData *>(g_task_get_task_data(task));
        MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
        if (widgets_task_should_skip_completion(task, widgets)) {
          return;
        }

        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);

        // Release this task's spinner slot.
        widgets_spinner_release(widgets->query.spinner);
        widgets->transaction_state.apply_in_progress = false;
        transaction_history_set_transaction_busy(false);
        set_main_window_sensitive_for_apply(widgets, true);

        if (success && td && td->offline) {
          transaction_progress_finish(
              td->progress_window,
              true,
              _("No packages have been changed yet. Restart when you are ready to install the prepared changes. "
                "You can discard them from Package > Updates Prepared for Reboot."));
          pending_transaction_invalidate_service_preview(widgets);
          widgets->transaction_state.actions.clear();
          pending_transaction_set_preview_controls_sensitive(widgets, true);
          refresh_package_table_statuses_after_apply(widgets);
          package_details_refresh_selected_package_actions(widgets);
          ui_helpers_set_status(widgets->query.status_label, _("Updates prepared. Restart to install them."), "blue");
          return;
        }

        if (success) {
          transaction_progress_finish(td ? td->progress_window : nullptr, true, "");
          mark_package_state_uncertain_after_apply(widgets);
          pending_transaction_invalidate_service_preview(widgets);
          // Clear pending actions and restore the transaction controls.
          widgets->transaction_state.actions.clear();
          pending_transaction_set_preview_controls_sensitive(widgets, true);
          refresh_package_table_statuses_after_apply(widgets);
          package_details_refresh_selected_package_actions(widgets);

          ui_helpers_set_status(widgets->query.status_label, _("Transaction successful."), "green");

          // Rebuild repository data and refresh package state in the background.
          rebuild_after_tx_async(widgets);
        } else {
          std::string details = error ? error->message : _("Transaction failed.");
          if (td && td->offline) {
            details += "\n";
            details += _("Preparation did not complete normally. Check Package > Updates Prepared for Reboot "
                         "before retrying; the daemon may have stored all or part of the transaction.");
          }
          transaction_progress_finish(td ? td->progress_window : nullptr, false, details);
          mark_package_state_uncertain_after_apply(widgets);
          pending_transaction_invalidate_service_preview(widgets);
          if (td && td->transaction_started) {
            widgets->transaction_state.actions.clear();
            refresh_package_table_statuses_after_apply(widgets);
            package_details_refresh_selected_package_actions(widgets);
          }
          pending_transaction_set_preview_controls_sensitive(widgets, true);
          ui_helpers_set_status(
              widgets->query.status_label, _("Transaction failed. See the transaction window for details."), "red");
          if (error) {
            g_error_free(error);
          }

          rebuild_after_tx_async(widgets);
        }
      });

  g_task_set_task_data(task, td, apply_task_data_free);

  g_task_run_in_thread(
      task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *cancellable) {
        ApplyTaskData *td = static_cast<ApplyTaskData *>(task_data);
        std::string err;
        // This callback runs on the apply worker thread when the client receives a service Progress signal.
        // transaction_progress_append queues the actual GTK update onto the main thread.
        bool ok = transaction_service_client_apply_started_request(
            td->transaction_path,
            [td](const std::string &message) { transaction_progress_append(td->progress_window, message); },
            [td](const TransactionApplyProgress &progress) {
              transaction_progress_update(td->progress_window, progress);
            },
            [td, cancellable](const TransactionKeyImportRequest &request) {
              return transaction_dialogs_confirm_key_import(td->widgets, request, cancellable);
            },
            err,
            td->transaction_started,
            cancellable,
            td->offline);
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
start_preview_request(MainWindowUiState *widgets, TransactionRequest request)
{
  package_details_cancel_active_load(widgets);
  pending_transaction_invalidate_service_preview(widgets);
  widgets->transaction_state.preview_upgrade_all = request.upgrade_all;
  DNFUI_TRACE("Transaction preview request start upgrade_all=%d install=%zu upgrade=%zu downgrade=%zu remove=%zu "
              "reinstall=%zu",
              request.upgrade_all ? 1 : 0,
              request.install.size(),
              request.upgrade.size(),
              request.downgrade.size(),
              request.remove.size(),
              request.reinstall.size());
  ui_helpers_set_status(widgets->query.status_label, _("Preparing transaction preview..."), "blue");
  widgets_spinner_acquire(widgets->query.spinner);
  set_preview_request_busy_state(widgets, true);

  PreviewTaskData *td = new PreviewTaskData();
  td->widgets = widgets;
  td->request = std::move(request);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(
      widgets, c, +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
        PreviewTaskData *td = static_cast<PreviewTaskData *>(g_task_get_task_data(task));

        if (widgets_task_should_skip_completion(task, widgets)) {
          return;
        }

        widgets_spinner_release(widgets->query.spinner);
        set_preview_request_busy_state(widgets, false);
        package_query_maybe_start_upgrade_indicator_refresh(widgets);

        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);
        if (!success || !td) {
          const char *status_message =
              error && error->message ? error->message : _("Unable to prepare transaction preview.");
          DNFUI_TRACE("Transaction preview request failed error=%s", status_message);
          widgets->transaction_state.preview_upgrade_all = false;
          ui_helpers_set_status(widgets->query.status_label, status_message, "red");
          transaction_dialogs_show_error_dialog(widgets,
                                                _("Transaction Preview Failed"),
                                                _("The transaction could not be prepared. Review the details below."),
                                                status_message);
          if (error) {
            g_error_free(error);
          }
          return;
        }

        if (td->preview.empty()) {
          DNFUI_TRACE(
              "Transaction preview request empty upgrade_all=%d install=%zu upgrade=%zu downgrade=%zu remove=%zu "
              "reinstall=%zu path=%s",
              td->request.upgrade_all ? 1 : 0,
              td->request.install.size(),
              td->request.upgrade.size(),
              td->request.downgrade.size(),
              td->request.remove.size(),
              td->request.reinstall.size(),
              td->transaction_path.c_str());
          if (!td->transaction_path.empty()) {
            transaction_service_client_release_request_async(td->transaction_path);
            td->transaction_path.clear();
          }
          widgets->transaction_state.preview_transaction_path.clear();
          widgets->transaction_state.preview_upgrade_all = false;
          if (!td->preview.resolve_warnings.empty()) {
            transaction_dialogs_show_error_dialog(
                widgets,
                _("Transaction Preview Warning"),
                _("No package changes were returned, but dnf5daemon reported a warning."),
                td->preview.resolve_warnings);
          }
          ui_helpers_set_status(widgets->query.status_label, empty_preview_status_message(td->request), "green");
          return;
        }

        widgets->transaction_state.preview_transaction_path = td->transaction_path;
        widgets->transaction_state.preview_upgrade_all = td->request.upgrade_all;
        widgets->transaction_state.preview_requires_offline = td->preview.requires_offline;
        td->transaction_path_transferred = true;
        DNFUI_TRACE("Transaction preview request ready upgrade_all=%d path=%s",
                    td->request.upgrade_all ? 1 : 0,
                    widgets->transaction_state.preview_transaction_path.c_str());
        ui_helpers_set_status(widgets->query.status_label, _("Ready."), "gray");
        transaction_dialogs_show_summary_dialog(
            widgets, td->preview, start_apply_transaction, pending_transaction_cancel_service_preview);
      });

  g_task_set_task_data(task, td, preview_task_data_free);
  g_task_run_in_thread(
      task, +[](GTask *task, gpointer, gpointer task_data, GCancellable *cancellable) {
        PreviewTaskData *td = static_cast<PreviewTaskData *>(task_data);
        std::string error;
        bool ok = false;
        if (td && td->request.upgrade_all) {
          DNFUI_TRACE("Transaction preview worker start upgrade_all=1");
          ok = transaction_service_client_preview_upgrade_all_request(
              td->preview,
              td->transaction_path,
              error,
              [td, cancellable](const TransactionKeyImportRequest &request) {
                return transaction_dialogs_confirm_key_import(td->widgets, request, cancellable);
              },
              cancellable);
        } else if (td) {
          DNFUI_TRACE("Transaction preview worker start upgrade_all=0 install=%zu upgrade=%zu downgrade=%zu remove=%zu "
                      "reinstall=%zu",
                      td->request.install.size(),
                      td->request.upgrade.size(),
                      td->request.downgrade.size(),
                      td->request.remove.size(),
                      td->request.reinstall.size());
          if (!pending_transaction_validate_request(td->request, error)) {
            ok = false;
          } else {
            ok = transaction_service_client_preview_request(
                td->request,
                td->preview,
                td->transaction_path,
                error,
                [td, cancellable](const TransactionKeyImportRequest &request) {
                  return transaction_dialogs_confirm_key_import(td->widgets, request, cancellable);
                },
                cancellable);
          }
        }

        if (!ok) {
          DNFUI_TRACE("Transaction preview worker failed error=%s", error.c_str());
          g_task_return_new_error(task,
                                  G_IO_ERROR,
                                  G_IO_ERROR_FAILED,
                                  "%s",
                                  error.empty() ? _("Unable to prepare transaction preview.") : error.c_str());
          return;
        }

        DNFUI_TRACE("Transaction preview worker done path=%s empty=%d",
                    td ? td->transaction_path.c_str() : "",
                    td && td->preview.empty() ? 1 : 0);
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
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  if (pending_transaction_apply_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_apply_busy_message(), "blue");
    return;
  }

  if (!widgets->transaction_state.actions.empty()) {
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
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  if (pending_transaction_apply_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_apply_busy_message(), "blue");
    return;
  }

  if (widgets->transaction_state.actions.empty()) {
    ui_helpers_set_status(widgets->query.status_label, _("No pending changes."), "gray");
    return;
  }

  TransactionRequest request;
  std::string error;
  if (!pending_transaction_build_request(widgets->transaction_state.actions, request, error)) {
    ui_helpers_set_status(widgets->query.status_label, error.c_str(), "red");
    return;
  }

  start_preview_request(widgets, std::move(request));
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
