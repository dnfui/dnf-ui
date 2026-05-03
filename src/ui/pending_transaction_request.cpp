// -----------------------------------------------------------------------------
// pending_transaction_request.cpp
// Pending transaction request helpers
// Keeps transaction request construction and validation separate from GTK
// pending action callbacks.
// -----------------------------------------------------------------------------
#include "pending_transaction_request.hpp"

#include "base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"

namespace {

struct PendingRequestBaseDropGuard {
  ~PendingRequestBaseDropGuard()
  {
    BaseManager::instance().drop_cached_base();
  }
};

}

// -----------------------------------------------------------------------------
// Split the pending queue into install, remove, and reinstall transaction specs.
// -----------------------------------------------------------------------------
static void
build_pending_transaction_specs(const std::vector<PendingAction> &actions,
                                std::vector<std::string> &install,
                                std::vector<std::string> &remove,
                                std::vector<std::string> &reinstall)
{
  install.clear();
  remove.clear();
  reinstall.clear();

  install.reserve(actions.size());
  remove.reserve(actions.size());
  reinstall.reserve(actions.size());

  for (const auto &action : actions) {
    if (action.type == PendingAction::INSTALL) {
      install.push_back(action.nevra);
    } else if (action.type == PendingAction::REINSTALL) {
      reinstall.push_back(action.nevra);
    } else {
      remove.push_back(action.nevra);
    }
  }
}

// -----------------------------------------------------------------------------
// Convert marked UI actions into a transaction request.
// -----------------------------------------------------------------------------
void
pending_transaction_build_request(const std::vector<PendingAction> &actions, TransactionRequest &request)
{
  request.upgrade_all = false;
  build_pending_transaction_specs(actions, request.install, request.remove, request.reinstall);
}

// -----------------------------------------------------------------------------
// Reject any transaction that would remove or reinstall the package owning the running GUI.
// The UI disables those actions already, but this keeps apply safe
// even if a protected item somehow reaches the pending queue.
// -----------------------------------------------------------------------------
bool
pending_transaction_validate_request(const TransactionRequest &request, std::string &error_out)
{
  PendingRequestBaseDropGuard base_drop_guard;

  for (const auto &spec : request.remove) {
    // Re-check remove specs at apply time so self-protection still holds even
    // if outdated UI state or future code paths bypass button sensitivity.
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot remove the package that owns the running application. Close DNF UI and remove it "
                    "from another tool.");
      return false;
    }
  }

  for (const auto &spec : request.reinstall) {
    // Re-check reinstall specs for the same reason: the running app must never
    // ask the backend to modify the RPM that owns the current executable.
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot reinstall the package that owns the running application while it is running.");
      return false;
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
