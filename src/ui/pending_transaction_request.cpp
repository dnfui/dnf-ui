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

// -----------------------------------------------------------------------------
// Return the daemon-facing spec for one pending action.
// Older pending-action state only had a NEVRA; keep that as the fallback.
// -----------------------------------------------------------------------------
static const std::string &
pending_action_transaction_spec(const PendingAction &action)
{
  return action.transaction_spec.empty() ? action.nevra : action.transaction_spec;
}

}

// -----------------------------------------------------------------------------
// Split the pending queue into install, upgrade, remove, and reinstall transaction specs.
// -----------------------------------------------------------------------------
static bool
build_pending_transaction_specs(const std::vector<PendingAction> &actions,
                                std::vector<std::string> &install,
                                std::vector<std::string> &upgrade,
                                std::vector<std::string> &remove,
                                std::vector<std::string> &reinstall,
                                std::string &error_out)
{
  install.clear();
  upgrade.clear();
  remove.clear();
  reinstall.clear();
  error_out.clear();

  install.reserve(actions.size());
  upgrade.reserve(actions.size());
  remove.reserve(actions.size());
  reinstall.reserve(actions.size());

  for (const auto &action : actions) {
    const std::string &spec = pending_action_transaction_spec(action);
    switch (action.type) {
    case PendingAction::INSTALL:
      install.push_back(spec);
      break;
    case PendingAction::UPGRADE:
      upgrade.push_back(spec);
      break;
    case PendingAction::REMOVE:
      remove.push_back(spec);
      break;
    case PendingAction::REINSTALL:
      reinstall.push_back(spec);
      break;
    default:
      install.clear();
      upgrade.clear();
      remove.clear();
      reinstall.clear();
      error_out = _("Unknown pending package action.");
      return false;
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
// Convert marked UI actions into a transaction request.
// -----------------------------------------------------------------------------
bool
pending_transaction_build_request(const std::vector<PendingAction> &actions,
                                  TransactionRequest &request,
                                  std::string &error_out)
{
  request.upgrade_all = false;
  return build_pending_transaction_specs(
      actions, request.install, request.upgrade, request.remove, request.reinstall, error_out);
}

// -----------------------------------------------------------------------------
// Reject any transaction that would modify the package owning the running GUI.
// The UI disables those actions already, but this keeps apply safe
// even if a protected item somehow reaches the pending queue.
// -----------------------------------------------------------------------------
bool
pending_transaction_validate_request(const TransactionRequest &request, std::string &error_out)
{
  PendingRequestBaseDropGuard base_drop_guard;

  for (const auto &spec : request.install) {
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot modify the package that owns the running application while it is running.");
      return false;
    }
  }

  for (const auto &spec : request.upgrade) {
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot upgrade the package that owns the running application while it is running.");
      return false;
    }
  }

  for (const auto &spec : request.remove) {
    // Re-check remove specs so stale UI state or bypassed button sensitivity cannot remove the running app.
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot remove the package that owns the running application. Close DNF UI and remove it "
                    "from another tool.");
      return false;
    }
  }

  for (const auto &spec : request.reinstall) {
    // Re-check reinstall specs so stale UI state or bypassed button sensitivity
    // cannot reinstall the running app.
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