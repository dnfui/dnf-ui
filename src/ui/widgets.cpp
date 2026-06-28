// -----------------------------------------------------------------------------
// src/ui/widgets.cpp
// Shared widget task helpers
//
// Keeps small GTK task lifetime and spinner helpers shared by controller files.
// Repository refresh workflow lives in repository_refresh_controller.cpp.
// -----------------------------------------------------------------------------
#include "widgets.hpp"

#include "widgets_internal.hpp"

namespace {

constexpr const char *kTaskSearchWidgetsHoldKey = "dnfui-task-search-widgets-hold";

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
// EOF
// -----------------------------------------------------------------------------
