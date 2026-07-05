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
// Convert one libdnf5 history package to the UI-facing row model.
// -----------------------------------------------------------------------------
TransactionHistoryPackageRow
make_history_row(libdnf5::transaction::Transaction &transaction, libdnf5::transaction::Package &package)
{
  TransactionHistoryPackageRow row;
  row.transaction_id = transaction.get_id();
  row.started_at = transaction.get_dt_start();
  row.ended_at = transaction.get_dt_end();
  // Historical package items can keep STARTED as their item state even when the transaction completed successfully.
  row.succeeded = transaction.get_state() == libdnf5::transaction::TransactionState::OK;
  row.action = history_action_from_libdnf(package.get_action());
  row.package_id = package.to_string();
  row.name = package.get_name();
  row.epoch = package.get_epoch();
  row.version = package.get_version();
  row.release = package.get_release();
  row.arch = package.get_arch();
  row.repo = package.get_repoid();
  row.description = transaction.get_description();
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
// Return true when one history row matches the selected result filter.
// -----------------------------------------------------------------------------
bool
history_row_matches_result(const TransactionHistoryPackageRow &row, TransactionHistoryResultFilter result)
{
  switch (result) {
  case TransactionHistoryResultFilter::OK:
    return row.succeeded;
  case TransactionHistoryResultFilter::FAILED:
    return !row.succeeded;
  case TransactionHistoryResultFilter::ALL:
  default:
    return true;
  }
}

// -----------------------------------------------------------------------------
// Return true when one history row matches the current backend history filter.
// -----------------------------------------------------------------------------
bool
history_row_matches_filter(const TransactionHistoryPackageRow &row, const TransactionHistoryFilter &filter)
{
  if (filter.action_enabled && row.action != filter.action) {
    return false;
  }

  if (!history_row_matches_result(row, filter.result)) {
    return false;
  }

  if (row.started_at < filter.from || row.started_at > filter.to) {
    return false;
  }

  if (!filter.package_text.empty()) {
    std::string package_text = row.name;
    package_text += "\n";
    package_text += row.package_id;
    if (history_filter_text(package_text).find(filter.package_text) == std::string::npos) {
      return false;
    }
  }

  if (!filter.detail_text.empty()) {
    std::string detail_text = row.repo;
    detail_text += "\n";
    detail_text += row.description;
    detail_text += "\n";
    detail_text += row.arch;
    if (history_filter_text(detail_text).find(filter.detail_text) == std::string::npos) {
      return false;
    }
  }

  return true;
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
// The cursor lets the UI continue inside a large transaction without loading all rows at once.
// -----------------------------------------------------------------------------
TransactionHistoryPage
dnf_backend_list_transaction_history_page(TransactionHistoryCursor cursor,
                                          const TransactionHistoryFilter &filter,
                                          size_t max_package_rows,
                                          GCancellable *cancellable)
{
  throw_if_history_cancelled(cancellable);

  auto read = BaseManager::instance().acquire_system_only_read();
  auto history = read.base->get_transaction_history();
  TransactionHistoryFilter normalized_filter = normalize_history_filter(filter);

  std::vector<int64_t> transaction_ids = history->list_transaction_ids();
  TransactionHistoryPage page;
  if (transaction_ids.empty() || max_package_rows == 0) {
    page.next_cursor.transaction_offset = transaction_ids.size();
    return page;
  }

  TransactionHistoryCursor scan_cursor;
  bool collect_page = cursor.transaction_offset == 0 && cursor.package_offset == 0;
  bool page_full = false;

  while (scan_cursor.transaction_offset < transaction_ids.size()) {
    throw_if_history_cancelled(cancellable);

    const size_t id_index = transaction_ids.size() - scan_cursor.transaction_offset - 1;
    auto transactions = history->list_transactions(std::vector<int64_t> { transaction_ids[id_index] });
    if (transactions.empty()) {
      ++scan_cursor.transaction_offset;
      scan_cursor.package_offset = 0;
      continue;
    }

    auto &transaction = transactions.front();
    auto packages = transaction.get_packages();
    if (scan_cursor.package_offset >= packages.size()) {
      ++scan_cursor.transaction_offset;
      scan_cursor.package_offset = 0;
      continue;
    }

    bool transaction_matches = false;
    while (scan_cursor.package_offset < packages.size()) {
      throw_if_history_cancelled(cancellable);

      if (!collect_page && scan_cursor.transaction_offset == cursor.transaction_offset &&
          scan_cursor.package_offset == cursor.package_offset) {
        collect_page = true;
      }

      TransactionHistoryPackageRow row = make_history_row(transaction, packages[scan_cursor.package_offset]);
      ++scan_cursor.package_offset;

      if (!history_row_matches_filter(row, normalized_filter)) {
        continue;
      }

      ++page.total_package_rows;
      transaction_matches = true;

      if (collect_page && !page_full) {
        page.rows.push_back(std::move(row));
        if (page.rows.size() >= max_package_rows) {
          page.next_cursor = scan_cursor;
          page_full = true;
        }
      } else if (collect_page && page_full) {
        page.has_more = true;
      }
    }

    if (transaction_matches) {
      ++page.total_transactions;
    }

    ++scan_cursor.transaction_offset;
    scan_cursor.package_offset = 0;
  }

  if (!page_full) {
    page.next_cursor = scan_cursor;
  }

  return page;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
