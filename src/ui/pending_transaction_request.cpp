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

#include <exception>

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
static bool
build_pending_transaction_specs(const std::vector<PendingAction> &actions,
                                std::vector<std::string> &install,
                                std::vector<std::string> &remove,
                                std::vector<std::string> &reinstall,
                                std::string &error_out)
{
  install.clear();
  remove.clear();
  reinstall.clear();
  error_out.clear();

  install.reserve(actions.size());
  remove.reserve(actions.size());
  reinstall.reserve(actions.size());

  for (const auto &action : actions) {
    switch (action.type) {
    case PendingAction::INSTALL:
    case PendingAction::UPGRADE:
      install.push_back(action.nevra);
      break;
    case PendingAction::REMOVE:
      remove.push_back(action.nevra);
      break;
    case PendingAction::REINSTALL:
      reinstall.push_back(action.nevra);
      break;
    default:
      install.clear();
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
  return build_pending_transaction_specs(actions, request.install, request.remove, request.reinstall, error_out);
}

// -----------------------------------------------------------------------------
// Reject direct remove or reinstall requests for the package owning the running GUI.
// Selected upgrades are carried as install specs, so they are allowed here and
// checked again after dnf5daemon resolves the preview.
// -----------------------------------------------------------------------------
bool
pending_transaction_validate_request(const TransactionRequest &request, std::string &error_out)
{
  PendingRequestBaseDropGuard base_drop_guard;

  try {
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
  } catch (const std::exception &e) {
    error_out = e.what();
    return false;
  } catch (...) {
    error_out = _("Could not verify whether the transaction modifies DNF UI itself.");
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
