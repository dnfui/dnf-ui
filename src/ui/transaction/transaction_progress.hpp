// -----------------------------------------------------------------------------
// Transaction progress window helpers
//
// Creates the apply progress dialog and keeps it alive while service progress
// callbacks are still pending.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <string>

struct SearchWidgets;
struct TransactionProgressWindow;

// -----------------------------------------------------------------------------
// Create a transaction progress window for the pending action count.
// -----------------------------------------------------------------------------
TransactionProgressWindow *transaction_progress_create_window(SearchWidgets *widgets, size_t pending_count);
// -----------------------------------------------------------------------------
// Add one reference to a transaction progress window.
// -----------------------------------------------------------------------------
TransactionProgressWindow *transaction_progress_retain(TransactionProgressWindow *progress);
// -----------------------------------------------------------------------------
// Release one reference to a transaction progress window.
// -----------------------------------------------------------------------------
void transaction_progress_release(TransactionProgressWindow *progress);
// -----------------------------------------------------------------------------
// Append a progress message to the transaction progress window.
// -----------------------------------------------------------------------------
void transaction_progress_append(TransactionProgressWindow *progress, const std::string &message);
// -----------------------------------------------------------------------------
// Mark the transaction progress window as finished.
// -----------------------------------------------------------------------------
void transaction_progress_finish(TransactionProgressWindow *progress, bool success, const std::string &summary);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
