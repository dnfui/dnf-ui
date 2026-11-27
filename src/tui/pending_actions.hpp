// -----------------------------------------------------------------------------
// src/tui/pending_actions.hpp
// -----------------------------------------------------------------------------
#pragma once

#include "transaction/pending_action.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"
#include "upgrade_query.hpp"

namespace tui {

PendingTransactionSelectionActions pending_choices(const DisplayRow &row);
PendingAction::Type install_side_type(const PendingTransactionActionRows &rows);
const char *pending_action_name(PendingAction::Type type);
struct PendingMark {
  PendingAction action;
  DisplayRow source;
  std::string issue;
  std::string preview_issue {};

  bool has_issue() const;
  std::string issue_text() const;
};

// A failed or incomplete resolve must not erase the last known preview mismatch.
void update_pending_preview_issues(std::vector<PendingMark> &marks, std::vector<std::string> issues);

bool mark_pending_action(std::vector<PendingMark> &marks, const DisplayRow &row, PendingAction::Type type);

// Check installed state and any current daemon snapshot without doing I/O.
std::string pending_snapshot_issue(const PendingMark &mark, const DaemonUpgradeSnapshot &snapshot);

// Refresh installed state, daemon targets and exact repository availability.
// A nonempty result entry means that the corresponding mark cannot yet be used.
std::vector<std::string> validate_pending_marks(const std::vector<PendingMark> &marks,
                                                GCancellable *cancel,
                                                const std::shared_ptr<std::atomic<bool>> &cancel_flag);

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
