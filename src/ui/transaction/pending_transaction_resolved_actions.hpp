// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_resolved_actions.hpp
// Resolved package actions shown alongside explicit marks in the package table.
// -----------------------------------------------------------------------------
#pragma once

#include "ui/transaction/pending_transaction_state.hpp"

#include <map>
#include <string>

struct TransactionPreview;

// These are display-only results from dnf5daemon, not new transaction requests.
struct PendingTransactionResolvedActions {
  std::map<std::string, PendingAction::Type> by_nevra;
  std::map<std::string, PendingAction::Type> by_name_arch;
};

// -----------------------------------------------------------------------------
// Convert a complete daemon preview into package table action lookups.
// Ambiguous package streams are not projected onto a compact table row.
// -----------------------------------------------------------------------------
PendingTransactionResolvedActions pending_transaction_resolved_actions_from_preview(const TransactionPreview &preview);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
