// -----------------------------------------------------------------------------
// src/tui/transaction_preview.hpp
// -----------------------------------------------------------------------------
#pragma once

#include "pending_actions.hpp"
#include "transaction/transaction_preview.hpp"

#include <set>

#ifdef DNFUI_BUILD_TESTS
struct TransactionRequest;
#endif

namespace tui {

struct PreviewResult {
  TransactionPreview preview;
  std::vector<std::string> pending_issues;
  std::string error;
#ifdef DNFUI_BUILD_TESTS
  std::string released_session_path;
#endif
};

// Return one issue per mark. Only verified installonly targets may resolve as installs.
std::vector<std::string> preview_mark_issues(const TransactionPreview &preview,
                                             const std::vector<PendingMark> &marks,
                                             const std::set<std::string> &installonly_nevras);

// Resolve a preview and release its daemon session. This cannot apply packages.
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
