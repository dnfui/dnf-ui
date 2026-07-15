// -----------------------------------------------------------------------------
// pending_transaction_view.hpp
// Pending transaction tab helpers
//
// Owns the small UI helpers for the Pending Actions tab and the package action
// list stored in MainWindowUiState.
// -----------------------------------------------------------------------------
#pragma once

#include "ui/transaction/pending_transaction_state.hpp"

#include <string>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Rebuild the Pending Actions tab from the current pending actions.
// -----------------------------------------------------------------------------
void pending_transaction_refresh_pending_tab(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Remove one pending action by package ID.
// -----------------------------------------------------------------------------
bool pending_transaction_remove_action(MainWindowUiState *widgets, const std::string &nevra);
// -----------------------------------------------------------------------------
// Remove all pending actions for one package name and architecture.
// -----------------------------------------------------------------------------
bool pending_transaction_remove_package_key(MainWindowUiState *widgets, const std::string &package_key);
// -----------------------------------------------------------------------------
// Return the pending action type for one package ID.
// -----------------------------------------------------------------------------
bool pending_transaction_get_action_type(MainWindowUiState *widgets,
                                         const std::string &nevra,
                                         PendingAction::Type &out_type);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
