// -----------------------------------------------------------------------------
// src/ui/refresh/repository_refresh_controller.hpp
// Repository refresh controller
//
// Owns the Refresh Repositories button workflow and repository rebuild tasks.
// Generic task and spinner helpers stay in widgets_internal.hpp.
// -----------------------------------------------------------------------------
#pragma once

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Handle the refresh repositories button click.
// -----------------------------------------------------------------------------
void repository_refresh_on_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Return true while the repository refresh worker is running.
// -----------------------------------------------------------------------------
bool repository_refresh_is_running();
// -----------------------------------------------------------------------------
// Ask an active repository refresh to stop.
// -----------------------------------------------------------------------------
void repository_refresh_cancel_active();
// -----------------------------------------------------------------------------
// Rebuild repositories on a background task thread.
// -----------------------------------------------------------------------------
void repository_refresh_on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
