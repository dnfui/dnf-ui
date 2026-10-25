// -----------------------------------------------------------------------------
// transaction_key_dialog.hpp
// Repository signing-key confirmation used during preview and apply
// -----------------------------------------------------------------------------
#pragma once

#include <string>

typedef struct _GCancellable GCancellable;

struct MainWindowUiState;
struct TransactionKeyImportRequest;

// -----------------------------------------------------------------------------
// Show the GTK trust dialog and wait for the user's answer on the worker thread.
// Cancellation rejects the key and closes the dialog on the GTK thread.
// -----------------------------------------------------------------------------
bool transaction_dialogs_confirm_key_import(MainWindowUiState *widgets,
                                            const TransactionKeyImportRequest &request,
                                            GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Format the key details shown to the user, without requiring a GTK window.
// -----------------------------------------------------------------------------
std::string transaction_key_dialog_details_text(const TransactionKeyImportRequest &request);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
