// -----------------------------------------------------------------------------
// src/ui/package_query/package_query_controller.hpp
// Public package query controller entry points
//
// Owns the GTK callbacks and refresh hooks for search, package listing,
// query history, and package-query cache invalidation.
// -----------------------------------------------------------------------------
#pragma once

#include <gtk/gtk.h>

#include <string>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Handle the installed packages list button click.
// -----------------------------------------------------------------------------
void package_query_on_list_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Handle the available packages list button click.
// -----------------------------------------------------------------------------
void package_query_on_list_available_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Handle the upgradeable packages list button click.
// -----------------------------------------------------------------------------
void package_query_on_list_upgradeable_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Handle the package search button click.
// -----------------------------------------------------------------------------
void package_query_on_search_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Restore and run a search from a selected history row.
// -----------------------------------------------------------------------------
void package_query_on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data);
// -----------------------------------------------------------------------------
// Clear the current package list and search status.
// -----------------------------------------------------------------------------
void package_query_on_clear_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Clear cached package query results.
// -----------------------------------------------------------------------------
void package_query_clear_search_cache();
// -----------------------------------------------------------------------------
// Reload the currently displayed package query view.
// -----------------------------------------------------------------------------
void package_query_reload_current_view(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Show one package row by exact package ID on a background task.
// -----------------------------------------------------------------------------
void package_query_show_exact_package(MainWindowUiState *widgets, const std::string &nevra);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
