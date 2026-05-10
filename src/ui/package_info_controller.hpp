// -----------------------------------------------------------------------------
// src/ui/package_info_controller.hpp
// Package details controller entry points
//
// Handles package selection, details loading, and action-button sensitivity.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Clear the selected package state and disable package actions.
// -----------------------------------------------------------------------------
void package_info_clear_selected_package_state(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Start loading details for the selected package.
// -----------------------------------------------------------------------------
void package_info_load_selected_package_info(SearchWidgets *widgets, const PackageRow &selected);
// -----------------------------------------------------------------------------
// Reset the package details notebook to its empty state.
// -----------------------------------------------------------------------------
void package_info_reset_details_view(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Load repository file metadata for the selected package when requested.
// -----------------------------------------------------------------------------
void package_info_on_load_file_list_clicked(GtkButton *, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
