// -----------------------------------------------------------------------------
// src/ui/history/transaction_history_view.hpp
// Read-only transaction history window
// -----------------------------------------------------------------------------
#pragma once

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Open the read-only transaction history window.
// -----------------------------------------------------------------------------
void transaction_history_show_window(GtkWindow *parent);

// -----------------------------------------------------------------------------
// Close the transaction history window if it is open.
// -----------------------------------------------------------------------------
void transaction_history_close_window();

// -----------------------------------------------------------------------------
// Disable or restore the transaction history window while a package transaction runs.
// -----------------------------------------------------------------------------
void transaction_history_set_transaction_busy(bool busy);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
