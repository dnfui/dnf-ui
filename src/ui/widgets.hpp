// -----------------------------------------------------------------------------
// src/ui/widgets.hpp
// Shared GTK widget state
//
// Groups pointers to the main window controls so split controller modules can
// cooperate without owning each other's widgets.
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>
#include <memory>

#include <gtk/gtk.h>

#include "dnf_backend/dnf_backend.hpp"
#include "ui/package_query_state.hpp"
#include "ui/pending_transaction_state.hpp"

// -----------------------------------------------------------------------------
// Query controls and status widgets shared by search and package-list actions
// -----------------------------------------------------------------------------
struct PackageQueryWidgets {
  GtkEntry *entry = nullptr;
  GtkListBox *history_list = nullptr;
  GtkSpinner *spinner = nullptr;
  GtkButton *search_button = nullptr;
  GtkButton *list_button = nullptr;
  GtkButton *list_available_button = nullptr;
  GtkButton *list_upgradeable_button = nullptr;
  GtkLabel *status_label = nullptr;
  GtkCheckButton *desc_checkbox = nullptr;
  GtkCheckButton *exact_checkbox = nullptr;
};

// -----------------------------------------------------------------------------
// Package list view, details notebook, and current selection state
// -----------------------------------------------------------------------------
struct PackageResultsWidgets {
  GtkListBox *listbox = nullptr;
  GtkScrolledWindow *list_scroller = nullptr;
  GtkPaned *inner_paned = nullptr;
  // Text buffers owned by the details notebook text views.
  GtkTextBuffer *details_buffer = nullptr;
  GtkTextBuffer *files_buffer = nullptr;
  GtkTextBuffer *deps_buffer = nullptr;
  GtkTextBuffer *changelog_buffer = nullptr;
  GtkLabel *count_label = nullptr;
  std::string selected_nevra;
};

// -----------------------------------------------------------------------------
// Top-level window close state shared by the main app and widget controllers
// -----------------------------------------------------------------------------
struct MainWindowState {
  // Allow the next window close after the user confirms discarding pending changes.
  bool allow_close_with_pending = false;
  // Prevent opening multiple quit-confirmation dialogs for the same pending state.
  bool pending_quit_dialog_open = false;
  // Passive bottom-bar label used for quiet startup backend status.
  GtkLabel *backend_warmup_label = nullptr;
  // Cancellable owned by the startup backend warm up task.
  GCancellable *backend_warmup_cancellable = nullptr;
  // Set when the main window is being destroyed.
  bool destroyed = false;
};

// -----------------------------------------------------------------------------
// Shared widget state bag passed between the split controller modules
// -----------------------------------------------------------------------------
struct SearchWidgets : std::enable_shared_from_this<SearchWidgets> {
  PackageQueryWidgets query;
  PackageResultsWidgets results;
  PendingTransactionWidgets transaction;
  PackageQueryState query_state;
  MainWindowState window_state;
};

// -----------------------------------------------------------------------------
// Handle the refresh repositories button click.
// -----------------------------------------------------------------------------
void widgets_on_refresh_button_clicked(GtkButton *, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
