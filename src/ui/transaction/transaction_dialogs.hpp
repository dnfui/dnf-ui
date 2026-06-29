// -----------------------------------------------------------------------------
// Transaction dialog helpers
//
// Shows the resolved transaction summary before apply and shows copyable error
// details when preview or apply fails.
// -----------------------------------------------------------------------------
#pragma once

#include <string>

struct MainWindowUiState;
struct TransactionKeyImportRequest;
struct TransactionPreview;

using TransactionApplyCallback = void (*)(MainWindowUiState *widgets);

// -----------------------------------------------------------------------------
// Show a transaction error dialog with optional details.
// -----------------------------------------------------------------------------
void transaction_dialogs_show_error_dialog(MainWindowUiState *widgets,
                                           const char *title,
                                           const char *intro,
                                           const std::string &details);
// -----------------------------------------------------------------------------
// Show the resolved transaction summary before apply.
// -----------------------------------------------------------------------------
void transaction_dialogs_show_summary_dialog(MainWindowUiState *widgets,
                                             const TransactionPreview &preview,
                                             TransactionApplyCallback on_apply,
                                             TransactionApplyCallback on_cancel);
// -----------------------------------------------------------------------------
// Ask the user whether dnf5daemon may import one repository signing key.
// -----------------------------------------------------------------------------
bool transaction_dialogs_confirm_key_import(MainWindowUiState *widgets, const TransactionKeyImportRequest &request);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
