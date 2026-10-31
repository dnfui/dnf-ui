// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_history.cpp
// Read-only transaction history queries
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_backend.hpp"

#include "dnf_backend/base_manager.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <mutex>
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
// Return UTF-8 case-folded text for history filtering.
// -----------------------------------------------------------------------------
std::string
history_filter_text(const std::string &text)
{
  char *folded = g_utf8_casefold(text.c_str(), -1);
  if (!folded) {
    return text;
  }

  std::string result = folded;
  g_free(folded);
  return result;
}

// -----------------------------------------------------------------------------
// Normalize filter text once before scanning history rows.
// -----------------------------------------------------------------------------
TransactionHistoryFilter
normalize_history_filter(TransactionHistoryFilter filter)
{
  filter.package_text = history_filter_text(filter.package_text);
  filter.detail_text = history_filter_text(filter.detail_text);
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

// -----------------------------------------------------------------------------
// Return one page from the matching rows discovered so far.
// The caller scans until one row beyond the requested page is available or history is exhausted.
// -----------------------------------------------------------------------------
TransactionHistoryPage
history_page_from_rows(const std::vector<TransactionHistoryPackageRow> &rows,
                       bool scan_complete,
                       TransactionHistoryCursor cursor,
                       size_t max_package_rows)
{
  TransactionHistoryPage page;
  page.next_cursor.row_offset = cursor.row_offset;
  if (max_package_rows == 0 || cursor.row_offset >= rows.size()) {
    page.has_more = !scan_complete && max_package_rows > 0;
    return page;
  }

  size_t available_rows = rows.size() - cursor.row_offset;
  size_t page_size = std::min(max_package_rows, available_rows);
  auto page_begin =
      rows.begin() + static_cast<std::vector<TransactionHistoryPackageRow>::difference_type>(cursor.row_offset);
  auto page_end = page_begin + static_cast<std::vector<TransactionHistoryPackageRow>::difference_type>(page_size);
  page.rows.assign(page_begin, page_end);
  page.next_cursor.row_offset = cursor.row_offset + page.rows.size();
  page.has_more = page.next_cursor.row_offset < rows.size() || !scan_complete;
  return page;
}

}

// -----------------------------------------------------------------------------
// Reusable state for one applied transaction history filter.
// Only backend workers access libdnf5 objects. Matching rows are plain values.
// -----------------------------------------------------------------------------
class TransactionHistoryQuery {
  public:
  TransactionHistoryQuery(std::shared_ptr<libdnf5::Base> query_base,
                          TransactionHistoryFilter query_filter,
                          std::vector<int64_t> ids)
      : base(std::move(query_base))
      , filter(std::move(query_filter))
      , transaction_ids(std::move(ids))
  {
  }

  TransactionHistoryPage page(TransactionHistoryCursor cursor, size_t max_package_rows, GCancellable *cancellable)
  {
    std::lock_guard<std::mutex> lock(mutex);
    throw_if_history_cancelled(cancellable);
    if (max_package_rows == 0) {
      return history_page_from_rows(rows, scan_complete, cursor, max_package_rows);
    }

    size_t required_rows = cursor.row_offset;
    size_t remaining = std::numeric_limits<size_t>::max() - required_rows;
    size_t requested_rows = max_package_rows < remaining ? max_package_rows + 1 : remaining;
    required_rows += requested_rows;
    scan_until(required_rows, cancellable);
    return history_page_from_rows(rows, scan_complete, cursor, max_package_rows);
  }

  private:
  void load_next_transaction_chunk(GCancellable *cancellable)
  {
    throw_if_history_cancelled(cancellable);
    if (transaction_offset >= transaction_ids.size()) {
      scan_complete = true;
      return;
    }

    const size_t chunk_end = transaction_ids.size() - transaction_offset;
    loaded_chunk_size = std::min(kHistoryTransactionChunkSize, chunk_end);
    const size_t chunk_begin = chunk_end - loaded_chunk_size;
    std::vector<int64_t> chunk_ids(
        transaction_ids.begin() + static_cast<std::vector<int64_t>::difference_type>(chunk_begin),
        transaction_ids.begin() + static_cast<std::vector<int64_t>::difference_type>(chunk_end));

    auto history = base->get_transaction_history();
    auto loaded_transactions = history->list_transactions(chunk_ids);
    throw_if_history_cancelled(cancellable);
    sort_history_transactions_newest_first(loaded_transactions);
    transactions = std::move(loaded_transactions);
    transaction_index = 0;
  }

  void finish_transaction_chunk()
  {
    transaction_offset += loaded_chunk_size;
    loaded_chunk_size = 0;
    transactions.clear();
    transaction_index = 0;
    if (transaction_offset >= transaction_ids.size()) {
      scan_complete = true;
    }
  }

  void scan_transaction(libdnf5::transaction::Transaction &transaction, GCancellable *cancellable)
  {
    const bool succeeded = history_transaction_succeeded(transaction);
    if (!history_transaction_matches_result(succeeded, filter.result)) {
      return;
    }

    const int64_t started_at = transaction.get_dt_start();
    if (started_at < filter.from || started_at > filter.to) {
      return;
    }

    bool description_loaded = !filter.detail_text.empty();
    std::string description = description_loaded ? transaction.get_description() : "";
    std::vector<TransactionHistoryPackageRow> transaction_rows;
    auto packages = transaction.get_packages();
    for (auto &package : packages) {
      throw_if_history_cancelled(cancellable);

      const TransactionHistoryAction action = history_action_from_libdnf(package.get_action());
      if (!history_package_matches_filter(package, filter, action, description)) {
        continue;
      }

      if (!description_loaded) {
        description = transaction.get_description();
        description_loaded = true;
      }
      transaction_rows.push_back(make_history_row(transaction, package, succeeded, action, description));
    }

    rows.insert(
        rows.end(), std::make_move_iterator(transaction_rows.begin()), std::make_move_iterator(transaction_rows.end()));
  }

  void scan_until(size_t required_rows, GCancellable *cancellable)
  {
    while (!scan_complete && rows.size() < required_rows) {
      throw_if_history_cancelled(cancellable);

      if (transactions.empty()) {
        load_next_transaction_chunk(cancellable);
        if (transactions.empty()) {
          finish_transaction_chunk();
          continue;
        }
      }

      scan_transaction(transactions[transaction_index], cancellable);
      ++transaction_index;
      if (transaction_index >= transactions.size()) {
        finish_transaction_chunk();
      }
    }
  }

  std::shared_ptr<libdnf5::Base> base;
  TransactionHistoryFilter filter;
  std::vector<int64_t> transaction_ids;
  std::vector<libdnf5::transaction::Transaction> transactions;
  std::vector<TransactionHistoryPackageRow> rows;
  size_t transaction_offset = 0;
  size_t loaded_chunk_size = 0;
  size_t transaction_index = 0;
  bool scan_complete = false;
  std::mutex mutex;
};

#ifdef DNFUI_BUILD_TESTS
bool
dnf_backend_history_text_matches_filter_for_tests(const std::string &text, const std::string &filter_text)
{
  return history_text_contains_filter(text, history_filter_text(filter_text));
}

TransactionHistoryPage
dnf_backend_transaction_history_page_for_tests(const std::vector<TransactionHistoryPackageRow> &rows,
                                               bool scan_complete,
                                               TransactionHistoryCursor cursor,
                                               size_t max_package_rows)
{
  return history_page_from_rows(rows, scan_complete, cursor, max_package_rows);
}
#endif

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
// Create one reusable transaction history query for an applied filter.
// -----------------------------------------------------------------------------
std::shared_ptr<TransactionHistoryQuery>
dnf_backend_create_transaction_history_query(const TransactionHistoryFilter &filter, GCancellable *cancellable)
{
  throw_if_history_cancelled(cancellable);

  auto base = BaseManager::instance().build_transaction_history_base();
  auto history = base->get_transaction_history();
  TransactionHistoryFilter normalized_filter = normalize_history_filter(filter);
  std::vector<int64_t> transaction_ids = history->list_transaction_ids();
  throw_if_history_cancelled(cancellable);
  return std::make_shared<TransactionHistoryQuery>(
      std::move(base), std::move(normalized_filter), std::move(transaction_ids));
}

// -----------------------------------------------------------------------------
// Return one page from a reusable transaction history query.
// -----------------------------------------------------------------------------
TransactionHistoryPage
dnf_backend_list_transaction_history_page(const std::shared_ptr<TransactionHistoryQuery> &query,
                                          TransactionHistoryCursor cursor,
                                          size_t max_package_rows,
                                          GCancellable *cancellable)
{
  if (!query) {
    throw std::invalid_argument("Transaction history query is null.");
  }
  return query->page(cursor, max_package_rows, cancellable);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
