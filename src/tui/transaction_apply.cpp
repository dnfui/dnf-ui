// -----------------------------------------------------------------------------
// src/tui/transaction_apply.cpp
// -----------------------------------------------------------------------------
#include "transaction_apply.hpp"

#include "dnf5daemon_client/transaction_service_client.hpp"
#include "dnf_backend/base_manager.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <gio/gio.h>

#include <exception>
#include <iostream>

namespace tui {

PreparedTransaction::~PreparedTransaction()
{
  transaction_service_client_release_request(session_path);
}

std::string &
PreparedTransaction::path_for_resolution()
{
  return session_path;
}

void
PreparedTransaction::set_offline_mode(bool value)
{
  offline = value;
}

const std::string &
PreparedTransaction::path() const
{
  return session_path;
}

bool
PreparedTransaction::requires_offline() const
{
  return offline;
}

void
TransactionProgress::set_message(const std::string &value)
{
  std::lock_guard<std::mutex> lock(mutex);
  message = value;
}

void
TransactionProgress::set_count(bool determinate, uint64_t processed, uint64_t total)
{
  std::lock_guard<std::mutex> lock(mutex);
  count = determinate && total > 0 ? std::to_string(processed) + " of " + std::to_string(total) + "  " : "";
}

std::string
TransactionProgress::text() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return count + message;
}

OfflineState
query_offline_state(GCancellable *cancel)
{
  if (g_cancellable_is_cancelled(cancel)) {
    return OfflineState::UNKNOWN;
  }
  bool prepared = false;
  std::string error;
  try {
    if (transaction_service_client_has_prepared_offline_updates(prepared, error, cancel)) {
      return prepared ? OfflineState::PREPARED : OfflineState::NOT_PREPARED;
    }
  } catch (const std::exception &exception) {
    error = exception.what();
  }
  if (!error.empty() && !g_cancellable_is_cancelled(cancel)) {
    std::cerr << "Could not check prepared offline updates: " << error << '\n';
  }
  return OfflineState::UNKNOWN;
}

const char *
offline_recovery_hint()
{
  return "Use dnf5 offline status to inspect stored changes. "
         "Restart to install prepared updates, or use sudo dnf5 offline clean to discard them.";
}

ApplyResult
apply_transaction(std::unique_ptr<PreparedTransaction> prepared, TransactionProgress &progress)
{
  ApplyResult result;
  if (!prepared || prepared->path().empty()) {
    result.message = "No approved transaction session.";
    return result;
  }

  result.offline = prepared->requires_offline();
  std::string error;
  bool transaction_started = false;
  bool key_requested = false;
  try {
    result.success = transaction_service_client_apply_started_request(
        prepared->path(),
        [&progress](const std::string &message) { progress.set_message(message); },
        [&progress](const TransactionApplyProgress &update) {
          progress.set_count(update.determinate, update.processed, update.total);
        },
        [&key_requested](const TransactionKeyImportRequest &) {
          key_requested = true;
          return false;
        },
        error,
        transaction_started,
        nullptr,
        result.offline);
  } catch (const std::exception &exception) {
    error = exception.what();
  }

  prepared.reset();
  // An unsuccessful reply does not prove that the RPM transaction never started.
  DaemonUpgradeState::instance().mark_stale();
  BaseManager::instance().drop_cached_base();
  result.offline_state = query_offline_state(nullptr);

  if (result.success) {
    result.message = result.offline ? "Changes prepared for the next reboot. No packages have been changed yet.\n\n"
                                      "Restart when ready. Use dnf5 offline status to inspect the prepared transaction."
                                    : "Transaction applied successfully.";
  } else {
    result.message = error.empty() ? "Transaction did not complete successfully." : error;
    if (key_requested) {
      result.message +=
          "\n\nA repository signing key was declined. Key import is not supported in the terminal frontend.";
    }
    if (result.offline) {
      result.message +=
          "\n\nPreparation may have left a stored transaction. Check dnf5 offline status before retrying. "
          "Use sudo dnf5 offline clean to discard prepared changes if needed.";
    } else {
      result.message += "\n\nInstalled packages may have changed. Pending actions were cleared. "
                        "Review the refreshed package list before trying again.";
      if (result.offline_state == OfflineState::PREPARED) {
        result.message += std::string("\n\n") + offline_recovery_hint();
      }
    }
  }
  return result;
}

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
