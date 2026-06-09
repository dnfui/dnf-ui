// -----------------------------------------------------------------------------
// src/ui/widgets.cpp
// Repository refresh and shared widget helpers
// Handles refresh callbacks and helper code shared by the split widget
// controller modules.
// -----------------------------------------------------------------------------
#include "widgets.hpp"
#include "base_manager.hpp"
#include "i18n.hpp"
#include "package_query_controller.hpp"
#include "package_query_controller_internal.hpp"
#include "pending_transaction_apply.hpp"
#include "ui_helpers.hpp"
#include "widgets_internal.hpp"

#include <atomic>

namespace {

constexpr const char *kTaskSearchWidgetsHoldKey = "dnfui-task-search-widgets-hold";
constexpr const char *kTaskStartedAtUsKey = "dnfui-task-started-at-us";

// Prevent starting more than one repository refresh task at the same time.
std::atomic<bool> repository_refresh_running { false };

}

// -----------------------------------------------------------------------------
// Shared cancellable helper used by background widget tasks.
// -----------------------------------------------------------------------------
GCancellable *
widgets_make_task_cancellable_for(GtkWidget *w)
{
  GCancellable *c = g_cancellable_new();
  if (w) {
    g_signal_connect_object(w, "destroy", G_CALLBACK(g_cancellable_cancel), c, G_CONNECT_SWAPPED);
  }
  return c;
}

// -----------------------------------------------------------------------------
// Keep the shared widget state alive while a task can still use it.
// -----------------------------------------------------------------------------
static void
hold_search_widgets_for_task(GTask *task, SearchWidgets *widgets)
{
  if (!task || !widgets) {
    return;
  }

  auto *held_widgets = new std::shared_ptr<SearchWidgets>(widgets->shared_from_this());
  g_object_set_data_full(G_OBJECT(task), kTaskSearchWidgetsHoldKey, held_widgets, [](gpointer p) {
    delete static_cast<std::shared_ptr<SearchWidgets> *>(p);
  });
}

// -----------------------------------------------------------------------------
// Create a task that keeps SearchWidgets alive until its completion callback returns.
// -----------------------------------------------------------------------------
GTask *
widgets_task_new_for_search_widgets(SearchWidgets *widgets, GCancellable *c, GAsyncReadyCallback callback)
{
  GTask *task = g_task_new(nullptr, c, callback, widgets);
  hold_search_widgets_for_task(task, widgets);
  return task;
}

// -----------------------------------------------------------------------------
// Return true when a task result should not update the window.
// -----------------------------------------------------------------------------
bool
widgets_task_should_skip_completion(GTask *task, SearchWidgets *widgets)
{
  if (!widgets || widgets->window_state.destroyed) {
    return true;
  }

  GCancellable *c = task ? g_task_get_cancellable(task) : nullptr;
  return c && g_cancellable_is_cancelled(c);
}

// -----------------------------------------------------------------------------
// Count active spinner users so one task cannot hide another task's spinner.
// -----------------------------------------------------------------------------
static GQuark
spinner_quark()
{
  static GQuark q = 0;
  if (G_UNLIKELY(q == 0)) {
    q = g_quark_from_static_string("spinner-count");
  }

  return q;
}

// -----------------------------------------------------------------------------
// Show the spinner for one active task.
// -----------------------------------------------------------------------------
void
widgets_spinner_acquire(GtkSpinner *spinner)
{
  if (!spinner) {
    return;
  }

  GQuark q = spinner_quark();
  int count = GPOINTER_TO_INT(g_object_get_qdata(G_OBJECT(spinner), q));
  count++;
  g_object_set_qdata(G_OBJECT(spinner), q, GINT_TO_POINTER(count));

  if (count == 1) {
    gtk_widget_set_visible(GTK_WIDGET(spinner), TRUE);
    gtk_spinner_start(spinner);
  }
}

// -----------------------------------------------------------------------------
// Release one active task's spinner slot.
// -----------------------------------------------------------------------------
void
widgets_spinner_release(GtkSpinner *spinner)
{
  if (!spinner) {
    return;
  }

  GQuark q = spinner_quark();
  int count = GPOINTER_TO_INT(g_object_get_qdata(G_OBJECT(spinner), q));
  if (count > 0) {
    count--;
    g_object_set_qdata(G_OBJECT(spinner), q, GINT_TO_POINTER(count));
  }

  if (count == 0) {
    gtk_spinner_stop(spinner);
    gtk_widget_set_visible(GTK_WIDGET(spinner), FALSE);
    g_object_set_qdata(G_OBJECT(spinner), q, nullptr);
  }
}

// -----------------------------------------------------------------------------
// Refresh repositories on a worker thread so the window stays responsive.
// -----------------------------------------------------------------------------
void
widgets_on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    BaseRepoState refresh_state = BaseManager::instance().rebuild();
    // GTask completion transfers this heap value back to the GTK thread.
    // widgets_on_rebuild_task_finished() deletes it after reading the result.
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
void
widgets_on_force_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    BaseRepoState refresh_state = BaseManager::instance().rebuild(BaseRefreshMode::FORCE_METADATA_CHECK);
    // GTask completion transfers this heap value back to the GTK thread.
    // widgets_on_rebuild_task_finished() deletes it after reading the result.
    g_task_return_pointer(
        task, new BaseRepoState(refresh_state), [](gpointer p) { delete static_cast<BaseRepoState *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish repository refresh on the GTK thread.
// -----------------------------------------------------------------------------
void
widgets_on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    repository_refresh_running = false;
    return;
  }

  GError *error = nullptr;
  // When rebuild succeeds, this returns the heap value from widgets_on_rebuild_task().
  // This handler receives that pointer and deletes it after use.
  BaseRepoState *refresh_state = static_cast<BaseRepoState *>(g_task_propagate_pointer(task, &error));

  // Re-enable the shared controls before status updates and view reload so the
  // window returns to its normal interaction state after refresh.
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
    if (*refresh_state == BaseRepoState::LIVE_METADATA) {
      ui_helpers_set_status(widgets->query.status_label, _("Repositories refreshed."), "green");
    } else if (*refresh_state == BaseRepoState::CACHED_METADATA) {
      ui_helpers_set_status(
          widgets->query.status_label, _("Live repo refresh failed. Using cached repository metadata."), "blue");
    } else {
      ui_helpers_set_status(
          widgets->query.status_label, _("Live repo refresh failed. Showing installed packages only."), "blue");
    }
    package_query_reload_current_view(widgets);
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
void
widgets_on_force_rebuild_task_finished(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  widgets_on_rebuild_task_finished(source_object, res, user_data);

  if (!widgets || widgets->window_state.destroyed) {
    return;
  }

  widgets_spinner_release(widgets->query.spinner);

  const gint64 *started_at_us = static_cast<const gint64 *>(g_object_get_data(G_OBJECT(task), kTaskStartedAtUsKey));
  package_query_show_duration_label(widgets, _("Refresh Repositories"), started_at_us ? *started_at_us : 0);
}

// -----------------------------------------------------------------------------
// Handle the Refresh Repositories button.
// Starts a Base rebuild through the shared widget controller layer.
// -----------------------------------------------------------------------------
void
widgets_on_refresh_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (!widgets) {
    return;
  }

  // Rebuild and query workers both serialize on the shared Base.
  // Keep refresh out of the way when a normal package query is already running.
  if (package_query_has_active_package_list_request(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, _("Wait for the current package query to finish."), "blue");
    return;
  }

  // Preview preparation also uses the backend and should finish before a
  // repository rebuild starts changing the cached Base generation.
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  bool expected = false;
  if (!repository_refresh_running.compare_exchange_strong(expected, true)) {
    ui_helpers_set_status(widgets->query.status_label, _("Repository refresh is already running."), "gray");
    return;
  }

  // Once a rebuild starts, stop serving cached search results so the UI does
  // not reuse rows from repository state that is changing.
  package_query_clear_search_cache();
  ui_helpers_set_status(widgets->query.status_label, _("Refreshing repositories..."), "blue");
  widgets_spinner_acquire(widgets->query.spinner);
  // Keep the query and preview controls aligned with the backend rebuild state
  // so the user cannot queue work that only waits behind the refresh.
  package_query_set_idle_controls_sensitive(widgets, false);
  if (widgets->results.list_scroller) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->results.list_scroller), FALSE);
  }
  pending_transaction_set_preview_controls_sensitive(widgets, false);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, widgets_on_force_rebuild_task_finished);
  auto *started_at_us = new gint64(g_get_monotonic_time());
  g_object_set_data_full(
      G_OBJECT(task), kTaskStartedAtUsKey, started_at_us, [](gpointer p) { delete static_cast<gint64 *>(p); });
  g_task_run_in_thread(task, widgets_on_force_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
