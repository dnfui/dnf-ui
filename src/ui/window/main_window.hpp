// src/ui/window/main_window.hpp
// Main application window
//
// Owns construction and wiring of the primary GTK window.
// Application startup stays in the application module.
#pragma once

#include <gtk/gtk.h>

struct MainWindowUiState;

struct MainWindow {
  GtkWidget *window = nullptr;
  // Non-owning pointer used by startup code before window destruction.
  MainWindowUiState *widgets = nullptr;
  // Caller owns this reference and should release it after scheduling startup work.
  GCancellable *startup_cancellable = nullptr;
};

// -----------------------------------------------------------------------------
// Build the main window and return its startup handles.
// -----------------------------------------------------------------------------
MainWindow main_window_create(GtkApplication *app);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
