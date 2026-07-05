// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_history.cpp
// Read-only transaction history queries
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_backend.hpp"

#include "dnf_backend/base_manager.hpp"
#include "i18n.hpp"

#include <algorithm>

#include <libdnf5/transaction/transaction.hpp>
#include <libdnf5/transaction/transaction_history.hpp>
#include <libdnf5/transaction/transaction_item.hpp>
#include <libdnf5/transaction/transaction_item_action.hpp>

namespace {

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
// Return recent package changes from the libdnf5 transaction history database.
// The limit is counted in transactions, not package rows.
// -----------------------------------------------------------------------------
std::vector<TransactionHistoryPackageRow>
dnf_backend_list_transaction_history_rows(size_t max_transactions)
{
  auto read = BaseManager::instance().acquire_system_only_read();
  auto history = read.base->get_transaction_history();

  std::vector<int64_t> transaction_ids = history->list_transaction_ids();
  if (max_transactions > 0 && transaction_ids.size() > max_transactions) {
    transaction_ids.erase(transaction_ids.begin(), transaction_ids.end() - static_cast<long>(max_transactions));
  }

  std::vector<libdnf5::transaction::Transaction> transactions = history->list_transactions(transaction_ids);
  std::sort(transactions.begin(), transactions.end(), [](const auto &left, const auto &right) {
    if (left.get_dt_start() != right.get_dt_start()) {
      return left.get_dt_start() > right.get_dt_start();
    }
    return left.get_id() > right.get_id();
  });

  std::vector<TransactionHistoryPackageRow> rows;
  for (auto &transaction : transactions) {
    for (auto &package : transaction.get_packages()) {
      rows.push_back(make_history_row(transaction, package));
    }
  }

  return rows;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
