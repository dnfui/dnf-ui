// -----------------------------------------------------------------------------
// src/ui/widgets_internal.hpp
// Internal shared widget helpers
//
// Provides task lifetime and spinner helpers used by UI controllers.
// -----------------------------------------------------------------------------
#pragma once

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Create a cancellable that is cancelled when the widget is destroyed.
// -----------------------------------------------------------------------------
GCancellable *widgets_make_task_cancellable_for(GtkWidget *w);
// -----------------------------------------------------------------------------
// Create a task that keeps SearchWidgets alive until completion handling ends.
// -----------------------------------------------------------------------------
GTask *widgets_task_new_for_search_widgets(SearchWidgets *widgets, GCancellable *c, GAsyncReadyCallback callback);
// -----------------------------------------------------------------------------
// Return true when an async widget task should skip completion handling.
// -----------------------------------------------------------------------------
bool widgets_task_should_skip_completion(GTask *task, SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Increment the active task count for a spinner.
// -----------------------------------------------------------------------------
void widgets_spinner_acquire(GtkSpinner *spinner);
// -----------------------------------------------------------------------------
// Decrement the active task count for a spinner.
// -----------------------------------------------------------------------------
void widgets_spinner_release(GtkSpinner *spinner);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
