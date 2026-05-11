// -----------------------------------------------------------------------------
// pending_transaction_apply.hpp
// Pending transaction preview and apply helpers
//
// Owns the UI side of preparing a service preview, applying it, and refreshing
// package state after the service finishes.
// -----------------------------------------------------------------------------
#pragma once

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Release any prepared service preview because the pending actions changed.
// -----------------------------------------------------------------------------
void pending_transaction_invalidate_service_preview(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Return the status text shown while a preview request is running.
// -----------------------------------------------------------------------------
const char *pending_transaction_preview_busy_message();
// -----------------------------------------------------------------------------
// Return true when a preview request is running.
// -----------------------------------------------------------------------------
bool pending_transaction_preview_is_busy(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
