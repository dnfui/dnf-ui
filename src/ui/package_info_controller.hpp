// -----------------------------------------------------------------------------
// src/ui/package_info_controller.hpp
// Package details controller entry points
//
// Handles package selection, details loading, and action-button sensitivity.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Clear the selected package state and disable package actions.
// -----------------------------------------------------------------------------
void package_info_clear_selected_package_state(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Stop the active package details load, if one is still running.
// -----------------------------------------------------------------------------
void package_info_cancel_active_load(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Start loading details for the selected package.
// -----------------------------------------------------------------------------
void package_info_load_selected_package_info(SearchWidgets *widgets, const PackageRow &selected);
// -----------------------------------------------------------------------------
// Reset the package details panel to its empty state.
// -----------------------------------------------------------------------------
void package_info_reset_details_view(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
