// -----------------------------------------------------------------------------
// src/ui/details/package_details_controller.hpp
// Package details controller entry points
//
// Handles package selection, details loading, and action-button sensitivity.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

typedef struct _GParamSpec GParamSpec;
typedef struct _GtkStack GtkStack;

struct MainWindowUiState;
struct PackageTableRow;

// -----------------------------------------------------------------------------
// Clear the selected package state and disable package actions.
// -----------------------------------------------------------------------------
void package_details_clear_selected_package_state(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Show the details panel state for more than one selected package.
// -----------------------------------------------------------------------------
void package_details_show_multiple_selected_packages(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Stop the active package details load, if one is still running.
// -----------------------------------------------------------------------------
void package_details_cancel_active_load(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Start loading details for the selected package.
// -----------------------------------------------------------------------------
void package_details_load_selected_package_info(MainWindowUiState *widgets, const PackageTableRow &selected);
// -----------------------------------------------------------------------------
// Refresh the selected package action controls and Status label.
// -----------------------------------------------------------------------------
void package_details_refresh_selected_package_actions(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Load tab content that is fetched only when the tab is opened.
// -----------------------------------------------------------------------------
void package_details_on_details_page_changed(GtkStack *stack, GParamSpec *, gpointer user_data);
// -----------------------------------------------------------------------------
// Reset the package details panel to its empty state.
// -----------------------------------------------------------------------------
void package_details_reset_details_view(MainWindowUiState *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
