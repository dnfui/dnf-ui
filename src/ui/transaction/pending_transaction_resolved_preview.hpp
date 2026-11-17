// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_resolved_preview.hpp
// Background daemon preview used only for package table markings.
// -----------------------------------------------------------------------------
#pragma once

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Clear old resolved markings and cancel the current background request.
// -----------------------------------------------------------------------------
void pending_transaction_clear_resolved_preview(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Resolve the latest explicit marks without blocking the GTK thread.
// -----------------------------------------------------------------------------
void pending_transaction_update_resolved_preview(MainWindowUiState *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
