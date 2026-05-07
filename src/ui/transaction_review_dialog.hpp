// -----------------------------------------------------------------------------
// Transaction review dialog helpers
//
// Shows the resolved transaction summary before apply and shows copyable error
// details when preview or apply fails.
// -----------------------------------------------------------------------------
#pragma once

#include <string>

struct SearchWidgets;
struct TransactionPreview;

using TransactionApplyCallback = void (*)(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// Show a transaction error dialog with optional details.
// -----------------------------------------------------------------------------
void transaction_review_show_error_dialog(SearchWidgets *widgets,
                                          const char *title,
                                          const char *intro,
                                          const std::string &details);
// -----------------------------------------------------------------------------
// Show the resolved transaction summary before apply.
// -----------------------------------------------------------------------------
void transaction_review_show_summary_dialog(SearchWidgets *widgets,
                                            const TransactionPreview &preview,
                                            TransactionApplyCallback on_apply,
                                            TransactionApplyCallback on_cancel);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
