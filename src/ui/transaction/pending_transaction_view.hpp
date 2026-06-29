// -----------------------------------------------------------------------------
// pending_transaction_view.hpp
// Pending transaction tab helpers
//
// Owns the small UI helpers for the Pending Actions tab and the package action
// list stored in SearchWidgets.
// -----------------------------------------------------------------------------
#pragma once

#include "ui/transaction/pending_transaction_state.hpp"

#include <string>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Rebuild the Pending Actions tab from the current pending actions.
// -----------------------------------------------------------------------------
void pending_transaction_refresh_pending_tab(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Remove one pending action by package ID.
// -----------------------------------------------------------------------------
bool pending_transaction_remove_action(SearchWidgets *widgets, const std::string &nevra);
// -----------------------------------------------------------------------------
// Return the pending action type for one package ID.
// -----------------------------------------------------------------------------
bool
pending_transaction_get_action_type(SearchWidgets *widgets, const std::string &nevra, PendingAction::Type &out_type);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
