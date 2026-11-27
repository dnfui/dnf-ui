// -----------------------------------------------------------------------------
// src/tui/transaction_preview.hpp
// -----------------------------------------------------------------------------
#pragma once

#include "pending_actions.hpp"
#include "transaction_apply.hpp"
#include "transaction/transaction_preview.hpp"

#include <set>

#ifdef DNFUI_BUILD_TESTS
struct TransactionRequest;
#endif

namespace tui {

struct PreviewResult {
  TransactionPreview preview;
  std::vector<std::string> pending_issues;
  // Populated only after a complete resolution and comparison with the marked actions.
  std::vector<std::string> preview_issues;
  std::string error;
  std::unique_ptr<PreparedTransaction> prepared;
  OfflineState offline_state = OfflineState::UNKNOWN;
#ifdef DNFUI_BUILD_TESTS
  std::string session_path_for_tests;
#endif
};

// Return one issue per mark. Only verified installonly targets may resolve as installs.
std::vector<std::string> preview_mark_issues(const TransactionPreview &preview,
                                             const std::vector<PendingMark> &marks,
                                             const std::set<std::string> &installonly_nevras);

// Resolve a preview and retain the session only when all marked actions match.
PreviewResult preview_pending_marks(const std::vector<PendingMark> &marks,
                                    GCancellable *cancel,
                                    const std::shared_ptr<std::atomic<bool>> &cancel_flag);

std::string format_preview(const TransactionPreview &preview);

#ifdef DNFUI_BUILD_TESTS
// Exercise rejection and session cleanup with a real resolve and deliberately mismatched marks.
PreviewResult resolve_preview_for_tests(const TransactionRequest &request, const std::vector<PendingMark> &marks);
#endif

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
