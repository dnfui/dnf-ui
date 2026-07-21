// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_history.cpp
// Read-only transaction history queries
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_backend.hpp"

#include "dnf_backend/base_manager.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#include <libdnf5/transaction/transaction.hpp>
#include <libdnf5/transaction/transaction_history.hpp>
#include <libdnf5/transaction/transaction_item.hpp>
#include <libdnf5/transaction/transaction_item_action.hpp>

namespace {

constexpr size_t kHistoryTransactionChunkSize = 128;

// -----------------------------------------------------------------------------
// Stop history loading when the caller has cancelled the worker task.
// -----------------------------------------------------------------------------
void
throw_if_history_cancelled(GCancellable *cancellable)
{
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    throw std::runtime_error(_("History load was cancelled."));
  }
}

// -----------------------------------------------------------------------------
// Convert libdnf5 transaction action to the backend value model.
// -----------------------------------------------------------------------------
TransactionHistoryAction
history_action_from_libdnf(libdnf5::transaction::TransactionItemAction action)
{
  using libdnf5::transaction::TransactionItemAction;

  switch (action) {
  case TransactionItemAction::INSTALL:
    return TransactionHistoryAction::INSTALL;
  case TransactionItemAction::UPGRADE:
    return TransactionHistoryAction::UPGRADE;
  case TransactionItemAction::DOWNGRADE:
    return TransactionHistoryAction::DOWNGRADE;
  case TransactionItemAction::REINSTALL:
    return TransactionHistoryAction::REINSTALL;
  case TransactionItemAction::REMOVE:
    return TransactionHistoryAction::REMOVE;
  case TransactionItemAction::REPLACED:
    return TransactionHistoryAction::REPLACED;
  case TransactionItemAction::REASON_CHANGE:
    return TransactionHistoryAction::REASON_CHANGE;
  default:
    return TransactionHistoryAction::OTHER;
  }
}

// -----------------------------------------------------------------------------
// Return true when libdnf5 marks the transaction as completed successfully.
// -----------------------------------------------------------------------------
bool
history_transaction_succeeded(libdnf5::transaction::Transaction &transaction)
{
  return transaction.get_state() == libdnf5::transaction::TransactionState::OK;
}

// -----------------------------------------------------------------------------
// Convert one matching libdnf5 history package to the UI-facing row model.
// -----------------------------------------------------------------------------
TransactionHistoryPackageRow
make_history_row(libdnf5::transaction::Transaction &transaction,
                 libdnf5::transaction::Package &package,
                 bool succeeded,
                 TransactionHistoryAction action,
                 const std::string &description)
{
  TransactionHistoryPackageRow row;
  row.transaction_id = transaction.get_id();
  row.started_at = transaction.get_dt_start();
  // Historical package items can keep STARTED as their item state even when the transaction completed successfully.
  row.succeeded = succeeded;
  row.action = action;
  row.package_id = package.to_string();
  row.name = package.get_name();
  row.repo = package.get_repoid();
  row.description = description;
  return row;
}

// -----------------------------------------------------------------------------
// Return ASCII-lowercase text for simple history filtering.
// -----------------------------------------------------------------------------
std::string
history_filter_text(std::string text)
{
  std::transform(
      text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// -----------------------------------------------------------------------------
// Normalize filter text once before scanning history rows.
// -----------------------------------------------------------------------------
TransactionHistoryFilter
normalize_history_filter(TransactionHistoryFilter filter)
{
  filter.package_text = history_filter_text(std::move(filter.package_text));
  filter.detail_text = history_filter_text(std::move(filter.detail_text));
  return filter;
}

// -----------------------------------------------------------------------------
// Return true when one transaction matches the selected result filter.
// -----------------------------------------------------------------------------
bool
history_transaction_matches_result(bool succeeded, TransactionHistoryResultFilter result)
{
  switch (result) {
  case TransactionHistoryResultFilter::OK:
    return succeeded;
  case TransactionHistoryResultFilter::FAILED:
    return !succeeded;
  case TransactionHistoryResultFilter::ALL:
  default:
    return true;
  }
}

// -----------------------------------------------------------------------------
// Return true when one text value contains the normalized filter text.
// -----------------------------------------------------------------------------
bool
history_text_contains_filter(const std::string &text, const std::string &filter_text)
{
  return history_filter_text(text).find(filter_text) != std::string::npos;
}

// -----------------------------------------------------------------------------
// Return true when one history package matches the current package-level filters.
// The transaction-level filters are checked before this function is called.
// -----------------------------------------------------------------------------
bool
history_package_matches_filter(libdnf5::transaction::Package &package,
                               const TransactionHistoryFilter &filter,
                               TransactionHistoryAction action,
                               const std::string &description)
{
  if (filter.action_filter_enabled && filter.actions.count(action) == 0) {
    return false;
  }

  if (!filter.package_text.empty()) {
    if (!history_text_contains_filter(package.get_name(), filter.package_text) &&
        !history_text_contains_filter(package.to_string(), filter.package_text)) {
      return false;
    }
  }

  if (!filter.detail_text.empty()) {
    if (!history_text_contains_filter(package.get_repoid(), filter.detail_text) &&
        !history_text_contains_filter(description, filter.detail_text) &&
        !history_text_contains_filter(package.get_arch(), filter.detail_text)) {
      return false;
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
// Sort history transactions newest first before scanning package rows.
// -----------------------------------------------------------------------------
void
sort_history_transactions_newest_first(std::vector<libdnf5::transaction::Transaction> &transactions)
{
  std::sort(transactions.begin(), transactions.end(), [](const auto &left, const auto &right) {
    if (left.get_dt_start() != right.get_dt_start()) {
      return left.get_dt_start() > right.get_dt_start();
    }
    return left.get_id() > right.get_id();
  });
}

}

// -----------------------------------------------------------------------------
// Convert one transaction history action to user-facing text.
// -----------------------------------------------------------------------------
std::string
dnf_backend_transaction_history_action_to_string(TransactionHistoryAction action)
{
  switch (action) {
  case TransactionHistoryAction::INSTALL:
    return _("Install");
  case TransactionHistoryAction::UPGRADE:
    return _("Upgrade");
  case TransactionHistoryAction::DOWNGRADE:
    return _("Downgrade");
  case TransactionHistoryAction::REINSTALL:
    return _("Reinstall");
  case TransactionHistoryAction::REMOVE:
    return _("Remove");
  case TransactionHistoryAction::REPLACED:
    return _("Replaced");
  case TransactionHistoryAction::REASON_CHANGE:
    return _("Reason changed");
  case TransactionHistoryAction::OTHER:
  default:
    return _("Other");
  }
}

// -----------------------------------------------------------------------------
// Return one page of package changes from the libdnf5 transaction history database.
// The cursor stores the first matching row offset for the requested page.
// The backend stops after the requested page and one extra matching row.
// -----------------------------------------------------------------------------
TransactionHistoryPage
dnf_backend_list_transaction_history_page(TransactionHistoryCursor cursor,
                                          const TransactionHistoryFilter &filter,
                                          size_t max_package_rows,
                                          GCancellable *cancellable)
{
  throw_if_history_cancelled(cancellable);

  auto base = BaseManager::instance().build_transaction_history_base();
  auto history = base->get_transaction_history();
  TransactionHistoryFilter normalized_filter = normalize_history_filter(filter);

  std::vector<int64_t> transaction_ids = history->list_transaction_ids();
  TransactionHistoryPage page;
  if (transaction_ids.empty() || max_package_rows == 0) {
    page.next_cursor.row_offset = cursor.row_offset;
    return page;
  }

  const size_t page_start = cursor.row_offset;
  size_t matching_row_index = 0;

  size_t transaction_offset = 0;
  while (transaction_offset < transaction_ids.size()) {
    throw_if_history_cancelled(cancellable);

    const size_t chunk_end = transaction_ids.size() - transaction_offset;
    const size_t chunk_size = std::min(kHistoryTransactionChunkSize, chunk_end);
    const size_t chunk_begin = chunk_end - chunk_size;
    std::vector<int64_t> chunk_ids(transaction_ids.begin() + static_cast<long>(chunk_begin),
                                   transaction_ids.begin() + static_cast<long>(chunk_end));

    auto transactions = history->list_transactions(chunk_ids);
    if (transactions.empty()) {
      transaction_offset += chunk_size;
      continue;
    }
    sort_history_transactions_newest_first(transactions);

    for (auto &transaction : transactions) {
      throw_if_history_cancelled(cancellable);

      const bool succeeded = history_transaction_succeeded(transaction);
      if (!history_transaction_matches_result(succeeded, normalized_filter.result)) {
        continue;
      }

      const int64_t started_at = transaction.get_dt_start();
      if (started_at < normalized_filter.from || started_at > normalized_filter.to) {
        continue;
      }

      bool description_loaded = !normalized_filter.detail_text.empty();
      std::string description = description_loaded ? transaction.get_description() : "";
      auto packages = transaction.get_packages();
      for (auto &package : packages) {
        throw_if_history_cancelled(cancellable);

        const TransactionHistoryAction action = history_action_from_libdnf(package.get_action());
        if (!history_package_matches_filter(package, normalized_filter, action, description)) {
          continue;
        }

        if (matching_row_index >= page_start && page.rows.size() < max_package_rows) {
          if (!description_loaded) {
            description = transaction.get_description();
            description_loaded = true;
          }
          TransactionHistoryPackageRow row = make_history_row(transaction, package, succeeded, action, description);
          page.rows.push_back(std::move(row));
        } else if (matching_row_index >= page_start) {
          page.has_more = true;
          page.next_cursor.row_offset = page_start + page.rows.size();
          return page;
        }
        ++matching_row_index;
      }
    }

    transaction_offset += chunk_size;
  }

  page.next_cursor.row_offset = page_start + page.rows.size();

  return page;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
