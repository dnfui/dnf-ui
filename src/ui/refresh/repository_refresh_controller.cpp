// -----------------------------------------------------------------------------
// src/ui/refresh/repository_refresh_controller.cpp
// Repository refresh controller
//
// Keeps manual repository refresh and post-transaction repository rebuild work
// separate from shared widget task helpers.
// -----------------------------------------------------------------------------
#include "ui/refresh/repository_refresh_controller.hpp"

#include "dnf_backend/base_manager.hpp"
#include "debug_trace.hpp"
#include "i18n.hpp"
#include "ui/details/package_details_controller.hpp"
#include "ui/package_query/package_query_controller.hpp"
#include "ui/package_query/package_query_controller_internal.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/transaction/pending_transaction_apply.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <atomic>

namespace {

constexpr const char *kTaskStartedAtUsKey = "dnfui-task-started-at-us";

// Prevent starting more than one repository refresh task at the same time.
std::atomic<bool> repository_refresh_running { false };
GCancellable *repository_refresh_operation_cancellable = nullptr;
std::shared_ptr<std::atomic<bool>> repository_refresh_cancel_requested;
bool repository_refresh_spinner_owned = false;

struct RepositoryRefreshTaskData {
  std::shared_ptr<std::atomic<bool>> cancel_requested;
  GCancellable *operation_cancellable = nullptr;
  GtkLabel *progress_label = nullptr;
};

struct RepositoryRefreshPhaseLabelUpdate {
  GtkLabel *label = nullptr;
  std::string message;
};

}

// -----------------------------------------------------------------------------
// Return true while the repository refresh worker is running.
// -----------------------------------------------------------------------------
bool
repository_refresh_is_running()
{
  return repository_refresh_running.load(std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// Ask an active repository refresh to stop.
// Used by Stop and by main window cleanup.
// -----------------------------------------------------------------------------
void
repository_refresh_cancel_active()
{
  if (repository_refresh_cancel_requested) {
    repository_refresh_cancel_requested->store(true, std::memory_order_relaxed);
  }
  if (repository_refresh_operation_cancellable) {
    g_cancellable_cancel(repository_refresh_operation_cancellable);
  }
}

// -----------------------------------------------------------------------------
// Free data owned by one repository refresh task.
// -----------------------------------------------------------------------------
static void
repository_refresh_task_data_free(gpointer p)
{
  RepositoryRefreshTaskData *data = static_cast<RepositoryRefreshTaskData *>(p);
  if (!data) {
    return;
  }

  if (data->operation_cancellable) {
    g_object_unref(data->operation_cancellable);
  }
  if (data->progress_label) {
    g_object_unref(data->progress_label);
  }
  delete data;
}

// -----------------------------------------------------------------------------
// Show one refresh phase in the lower-right status area.
// Worker threads must queue GTK updates on the main thread.
// -----------------------------------------------------------------------------
static gboolean
repository_refresh_phase_label_update_on_main(gpointer user_data)
{
  RepositoryRefreshPhaseLabelUpdate *update = static_cast<RepositoryRefreshPhaseLabelUpdate *>(user_data);
  if (update && update->label && !update->message.empty()) {
    gtk_label_set_text(update->label, update->message.c_str());
    gtk_widget_set_visible(GTK_WIDGET(update->label), TRUE);
  }

  if (update && update->label) {
    g_object_unref(update->label);
  }
  delete update;
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Queue one refresh phase for the lower-right status area.
// Keep the label alive because the worker may finish before GTK shows the text.
// -----------------------------------------------------------------------------
static void
queue_repository_refresh_phase_label(GtkLabel *label, const std::string &message)
{
  if (!label || message.empty()) {
    return;
  }

  RepositoryRefreshPhaseLabelUpdate *update = new RepositoryRefreshPhaseLabelUpdate;
  update->label = GTK_LABEL(g_object_ref(label));
  update->message = message;
  g_main_context_invoke(nullptr, repository_refresh_phase_label_update_on_main, update);
}

// -----------------------------------------------------------------------------
// Release the spinner slot owned by user-triggered repository refresh.
// -----------------------------------------------------------------------------
static void
repository_refresh_release_spinner(MainWindowUiState *widgets)
{
  if (!repository_refresh_spinner_owned) {
    return;
  }

  repository_refresh_spinner_owned = false;
  widgets_spinner_release(widgets ? widgets->query.spinner : nullptr);
}

// -----------------------------------------------------------------------------
// Return the Refresh Repositories button to its normal label.
// -----------------------------------------------------------------------------
static void
repository_refresh_set_button_idle(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  ui_helpers_set_icon_button(widgets->query.refresh_button, "view-refresh-symbolic", _("Refresh Repositories"));
}

// -----------------------------------------------------------------------------
// Make the Refresh Repositories button show Stop while refresh is running.
// -----------------------------------------------------------------------------
static void
repository_refresh_set_button_stop(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  ui_helpers_set_icon_button(widgets->query.refresh_button, "process-stop-symbolic", _("Stop"));
}

// -----------------------------------------------------------------------------
// Refresh repositories on a worker thread so the window stays responsive.
// -----------------------------------------------------------------------------
void
repository_refresh_on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    BaseRepoState refresh_state = BaseManager::instance().rebuild();
    // GTask completion transfers this heap value back to the GTK thread.
    // repository_refresh_on_rebuild_task_finished() deletes it after reading the result.
    g_task_return_pointer(
        task, new BaseRepoState(refresh_state), [](gpointer p) { delete static_cast<BaseRepoState *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Force repository metadata refresh on a worker thread.
// Used only when the user clicks Refresh Repositories.
// -----------------------------------------------------------------------------
static void
repository_refresh_on_force_rebuild_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  auto *refresh_data = static_cast<RepositoryRefreshTaskData *>(task_data);
  GCancellable *operation_cancellable = refresh_data ? refresh_data->operation_cancellable : nullptr;

  try {
    DNFUI_TRACE("Repository refresh worker start");
    queue_repository_refresh_phase_label(refresh_data ? refresh_data->progress_label : nullptr,
                                         _("Refreshing dnf5daemon metadata..."));
    std::string daemon_error;
    if (!transaction_service_client_refresh_repositories(daemon_error, operation_cancellable)) {
      if (operation_cancellable && g_cancellable_is_cancelled(operation_cancellable)) {
        throw BaseOperationCancelled(daemon_error.empty() ? _("Repository refresh was cancelled.") : daemon_error);
      }
      if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        throw BaseOperationCancelled(_("Repository refresh was cancelled."));
      }
      throw std::runtime_error(daemon_error);
    }

    DNFUI_TRACE("Repository refresh daemon metadata done");
    queue_repository_refresh_phase_label(refresh_data ? refresh_data->progress_label : nullptr,
                                         _("Loading refreshed metadata..."));
    auto progress_callback = [refresh_data](const std::string &message) {
      queue_repository_refresh_phase_label(refresh_data ? refresh_data->progress_label : nullptr, message);
    };
    BaseRepoState refresh_state =
        BaseManager::instance().rebuild(BaseRefreshMode::FORCE_METADATA_CHECK,
                                        refresh_data ? refresh_data->cancel_requested : nullptr,
                                        progress_callback);
    DNFUI_TRACE("Repository refresh worker done state=%d", static_cast<int>(refresh_state));
    // GTask completion transfers this heap value back to the GTK thread.
    // The force refresh completion handler deletes it after reading the result.
    g_task_return_pointer(
        task, new BaseRepoState(refresh_state), [](gpointer p) { delete static_cast<BaseRepoState *>(p); });
  } catch (const BaseOperationCancelled &e) {
    DNFUI_TRACE("Repository refresh worker stopped: %s", e.what());
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, e.what()));
  } catch (const std::exception &e) {
    DNFUI_TRACE("Repository refresh worker failed: %s", e.what());
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish repository refresh on the GTK thread.
// -----------------------------------------------------------------------------
void
repository_refresh_on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    repository_refresh_running = false;
    return;
  }

  GError *error = nullptr;
  // When rebuild succeeds, this returns the heap value from repository_refresh_on_rebuild_task().
  // This handler receives that pointer and deletes it after use.
  BaseRepoState *refresh_state = static_cast<BaseRepoState *>(g_task_propagate_pointer(task, &error));

  // Re-enable shared controls before status updates and view reload so the window returns to normal after refresh.
  package_query_set_idle_controls_sensitive(widgets, true);
  if (widgets->results.list_scroller) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->results.list_scroller), TRUE);
  }
  pending_transaction_set_preview_controls_sensitive(widgets, true);

  repository_refresh_running = false;

  if (refresh_state) {
    // Search caches are bound to the old Base generation and must be dropped
    // before the user can query against freshly refreshed repositories.
    package_query_clear_search_cache();
    bool cleared_upgradeable_table = package_query_clear_displayed_upgradeable_table(widgets);
    if (*refresh_state == BaseRepoState::INSTALLED_ONLY) {
      ui_helpers_set_status(
          widgets->query.status_label, _("Repository refresh failed. Showing installed packages only."), "blue");
    } else if (cleared_upgradeable_table) {
      ui_helpers_set_status(widgets->query.status_label,
                            _("Repositories refreshed. Press List Upgradable to load updated upgrades."),
                            "green");
    } else {
      ui_helpers_set_status(widgets->query.status_label, _("Repositories refreshed."), "green");
    }
    if (!cleared_upgradeable_table) {
      package_query_reload_current_view(widgets);
    }
    delete refresh_state;
  } else {
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : _("Repo refresh failed."), "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Finish user-triggered repository refresh and release its spinner slot.
// -----------------------------------------------------------------------------
static void
repository_refresh_on_force_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  DNFUI_TRACE("Repository refresh completion start");
  if (widgets_task_should_skip_completion(task, widgets)) {
    DNFUI_TRACE("Repository refresh completion skipped");
    repository_refresh_running = false;
    repository_refresh_release_spinner(nullptr);
    if (repository_refresh_operation_cancellable) {
      g_object_unref(repository_refresh_operation_cancellable);
      repository_refresh_operation_cancellable = nullptr;
    }
    repository_refresh_cancel_requested.reset();
    return;
  }

  GError *error = nullptr;
  BaseRepoState *refresh_state = static_cast<BaseRepoState *>(g_task_propagate_pointer(task, &error));

  if (!widgets || widgets->window_state.destroyed) {
    repository_refresh_release_spinner(nullptr);
    if (repository_refresh_operation_cancellable) {
      g_object_unref(repository_refresh_operation_cancellable);
      repository_refresh_operation_cancellable = nullptr;
    }
    repository_refresh_cancel_requested.reset();
    repository_refresh_running = false;
    if (refresh_state) {
      delete refresh_state;
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  package_query_set_idle_controls_sensitive(widgets, true);
  if (widgets->results.list_scroller) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->results.list_scroller), TRUE);
  }
  pending_transaction_set_preview_controls_sensitive(widgets, true);

  repository_refresh_set_button_idle(widgets);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.refresh_button), TRUE);
  repository_refresh_release_spinner(widgets);
  repository_refresh_running = false;

  if (repository_refresh_operation_cancellable) {
    g_object_unref(repository_refresh_operation_cancellable);
    repository_refresh_operation_cancellable = nullptr;
  }
  repository_refresh_cancel_requested.reset();

  if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    DNFUI_TRACE("Repository refresh completion stopped");
    package_query_clear_duration_label(widgets);
    ui_helpers_set_status(widgets->query.status_label, _("Repository refresh stopped."), "gray");
    g_error_free(error);
    return;
  }

  if (refresh_state) {
    DNFUI_TRACE("Repository refresh completion done state=%d", static_cast<int>(*refresh_state));
    // Search caches are bound to the old Base generation and must be dropped
    // before the user can query against freshly refreshed repositories.
    package_query_clear_search_cache();
    bool cleared_upgradeable_table = package_query_clear_displayed_upgradeable_table(widgets);
    if (*refresh_state == BaseRepoState::LIVE_METADATA) {
      if (cleared_upgradeable_table) {
        ui_helpers_set_status(widgets->query.status_label,
                              _("Repositories refreshed. Press List Upgradable to load updated upgrades."),
                              "green");
      } else {
        ui_helpers_set_status(widgets->query.status_label, _("Repositories refreshed."), "green");
      }
    } else if (*refresh_state == BaseRepoState::CACHED_METADATA) {
      ui_helpers_set_status(
          widgets->query.status_label, _("Live repo refresh failed. Using cached repository metadata."), "blue");
    } else {
      ui_helpers_set_status(
          widgets->query.status_label, _("Live repo refresh failed. Showing installed packages only."), "blue");
    }
    if (!cleared_upgradeable_table) {
      package_query_reload_current_view(widgets);
    }
    delete refresh_state;
  } else {
    DNFUI_TRACE("Repository refresh completion failed: %s", error ? error->message : "unknown error");
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : _("Repo refresh failed."), "red");
    if (error) {
      g_error_free(error);
    }
    return;
  }

  const gint64 *started_at_us = static_cast<const gint64 *>(g_object_get_data(G_OBJECT(task), kTaskStartedAtUsKey));
  package_query_show_duration_label(widgets, _("Refresh Repositories"), started_at_us ? *started_at_us : 0);
}

// -----------------------------------------------------------------------------
// Handle the Refresh Repositories button.
// Starts a Base rebuild through the repository refresh controller.
// -----------------------------------------------------------------------------
void
repository_refresh_on_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (!widgets) {
    return;
  }

  // Rebuild and query workers both serialize on the shared Base.
  // Keep refresh out of the way when a normal package query is already running.
  if (package_query_has_active_package_list_request(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, _("Wait for the current package query to finish."), "blue");
    return;
  }

  // Preview preparation also uses the backend.
  // It should finish before repository rebuild changes the cached Base generation.
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  // Apply can change installed package state through dnf5daemon.
  // Do not rebuild repository metadata while that transaction is active.
  if (pending_transaction_apply_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_apply_busy_message(), "blue");
    return;
  }

  bool expected = false;
  // Only the first click may start a refresh task.
  // While it is running, the button asks the daemon and Base rebuild to stop.
  if (!repository_refresh_running.compare_exchange_strong(expected, true)) {
    if (repository_refresh_cancel_requested && !repository_refresh_cancel_requested->load(std::memory_order_relaxed)) {
      DNFUI_TRACE("Repository refresh stop requested from button");
      repository_refresh_cancel_active();
      repository_refresh_set_button_idle(widgets);
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.refresh_button), FALSE);
      ui_helpers_set_status(widgets->query.status_label, _("Stopping repository refresh..."), "gray");
    } else {
      // The user already pressed Stop.
      // Reject new refreshes until the background repo load clears repository_refresh_running.
      DNFUI_TRACE("Repository refresh stop requested again");
      ui_helpers_set_status(widgets->query.status_label, _("Repository refresh is stopping."), "gray");
    }
    return;
  }

  // Once a rebuild starts, stop serving cached search results so the UI does
  // not reuse rows from repository state that is changing.
  package_query_clear_search_cache();
  DaemonUpgradeState::instance().mark_stale();
  package_query_clear_displayed_upgradeable_table(widgets);
  DNFUI_TRACE("Repository refresh start requested from button");
  package_query_clear_duration_label(widgets);
  ui_helpers_set_status(widgets->query.status_label, _("Refreshing repositories..."), "blue");
  widgets_spinner_acquire(widgets->query.spinner);
  repository_refresh_spinner_owned = true;
  repository_refresh_set_button_stop(widgets);
  // Keep the query and preview controls aligned with the backend rebuild state
  // so the user cannot queue work that only waits behind the refresh.
  package_query_set_idle_controls_sensitive(widgets, false);
  if (widgets->results.list_scroller) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->results.list_scroller), FALSE);
  }
  pending_transaction_set_preview_controls_sensitive(widgets, false);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task =
      widgets_task_new_for_main_window_ui_state(widgets, c, repository_refresh_on_force_rebuild_task_finished);
  repository_refresh_operation_cancellable = g_cancellable_new();
  repository_refresh_cancel_requested = std::make_shared<std::atomic<bool>>(false);
  auto *refresh_data = new RepositoryRefreshTaskData;
  refresh_data->cancel_requested = repository_refresh_cancel_requested;
  refresh_data->operation_cancellable = G_CANCELLABLE(g_object_ref(repository_refresh_operation_cancellable));
  if (widgets->window_state.query_duration_label) {
    refresh_data->progress_label = GTK_LABEL(g_object_ref(widgets->window_state.query_duration_label));
  }
  g_task_set_task_data(task, refresh_data, repository_refresh_task_data_free);
  auto *started_at_us = new gint64(g_get_monotonic_time());
  g_object_set_data_full(
      G_OBJECT(task), kTaskStartedAtUsKey, started_at_us, [](gpointer p) { delete static_cast<gint64 *>(p); });
  g_task_run_in_thread(task, repository_refresh_on_force_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
