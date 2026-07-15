// -----------------------------------------------------------------------------
// src/ui/window/main_window_layout.hpp
// Main window layout handles
//
// Defines the temporary widget handle set used while building the main window.
// The main window module turns these handles into MainWindowUiState and connects
// behavior after layout construction is finished.
// -----------------------------------------------------------------------------
#pragma once

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Internal UI handles used only during application setup.
// Keeps widget construction readable without extending MainWindowUiState.
// -----------------------------------------------------------------------------
struct AppWidgets {
  GtkWidget *window = NULL;

  GtkWidget *vbox_root = NULL;
  GtkWidget *vbox_main = NULL;
  GtkWidget *vbox_history = NULL;

  GtkWidget *history_list = NULL;

  GtkWidget *entry = NULL;
  GtkWidget *search_button = NULL;
  GtkWidget *desc_checkbox = NULL;
  GtkWidget *exact_checkbox = NULL;
  GtkWidget *latest_checkbox = NULL;
  GtkWidget *spinner = NULL;

  GtkWidget *list_button = NULL;
  GtkWidget *list_available_button = NULL;
  GtkWidget *list_upgradeable_button = NULL;
  GtkWidget *refresh_button = NULL;

  GtkWidget *install_button = NULL;
  GtkWidget *reinstall_button = NULL;
  GtkWidget *remove_button = NULL;
  GtkWidget *upgrade_all_button = NULL;
  GtkWidget *mark_listed_upgrades_button = NULL;
  GtkWidget *apply_button = NULL;
  GtkWidget *clear_pending_button = NULL;

  GtkWidget *status_label = NULL;
  GtkWidget *inner_paned = NULL;

  GtkWidget *scrolled_list = NULL;
  GtkWidget *listbox = NULL;
  GtkWidget *details_panel = NULL;
  GtkWidget *details_stack = NULL;

  GtkTextBuffer *details_buffer = NULL;
  GtkTextBuffer *files_buffer = NULL;
  GtkTextBuffer *deps_buffer = NULL;
  GtkTextBuffer *changelog_buffer = NULL;
  GtkWidget *pending_list = NULL;

  GtkWidget *count_label = NULL;
  GtkWidget *warmup_label = NULL;
  GtkWidget *query_duration_label = NULL;
};

// -----------------------------------------------------------------------------
// Build all GTK widgets and fill AppWidgets with the handles used later.
// -----------------------------------------------------------------------------
void main_window_build_layout(AppWidgets *ui);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
