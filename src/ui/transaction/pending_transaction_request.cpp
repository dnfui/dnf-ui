// -----------------------------------------------------------------------------
// pending_transaction_request.cpp
// Pending transaction request helpers
// Keeps transaction request construction and validation separate from GTK pending action callbacks.
// -----------------------------------------------------------------------------
#include "ui/transaction/pending_transaction_request.hpp"

#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"

#include <exception>
#include <set>
#include <utility>

namespace {

struct PendingRequestBaseDropGuard {
  ~PendingRequestBaseDropGuard()
  {
    BaseManager::instance().drop_cached_base();
  }
};

// -----------------------------------------------------------------------------
// Return true when one pending action is queued by the install button path.
// -----------------------------------------------------------------------------
static bool
pending_action_is_install_side(PendingAction::Type type)
{
  return type == PendingAction::INSTALL || type == PendingAction::UPGRADE || type == PendingAction::DOWNGRADE;
}

// -----------------------------------------------------------------------------
// Return true when one pending action changes an installed package directly.
// -----------------------------------------------------------------------------
static bool
pending_action_is_installed_side(PendingAction::Type type)
{
  return type == PendingAction::REMOVE || type == PendingAction::REINSTALL;
}

}

// -----------------------------------------------------------------------------
// Split the pending queue into transaction specs by requested action.
// -----------------------------------------------------------------------------
static bool
build_pending_transaction_specs(const std::vector<PendingAction> &actions,
                                TransactionRequest &request,
                                std::string &error_out)
{
  error_out.clear();

  request.install.reserve(actions.size());
  request.upgrade.reserve(actions.size());
  request.downgrade.reserve(actions.size());
  request.remove.reserve(actions.size());
  request.reinstall.reserve(actions.size());

  std::set<std::string> install_side_keys;
  std::set<std::string> installonly_install_keys;
  std::set<std::string> installed_side_keys;

  for (const auto &action : actions) {
    if (!pending_action_is_install_side(action.type) && !pending_action_is_installed_side(action.type)) {
      error_out = _("Unknown pending package action.");
      return false;
    }

    if (action.transaction_spec.empty()) {
      error_out = _("Pending package action is missing its transaction spec.");
      return false;
    }

    if (action.package_key.empty()) {
      error_out = _("Pending package action is missing its package identity.");
      return false;
    }

    if (pending_action_is_install_side(action.type)) {
      const bool installonly_install =
          action.type == PendingAction::INSTALL && action.installonly && action.nevra == action.transaction_spec;
      if (installed_side_keys.count(action.package_key) > 0 ||
          (installonly_install && install_side_keys.count(action.package_key) > 0) ||
          (!installonly_install &&
           (installonly_install_keys.count(action.package_key) > 0 ||
            !install_side_keys.insert(action.package_key).second))) {
        error_out = _("Pending package actions contain conflicting package identities.");
        return false;
      }
      if (installonly_install) {
        installonly_install_keys.insert(action.package_key);
      }
    } else if (pending_action_is_installed_side(action.type)) {
      if (install_side_keys.count(action.package_key) > 0 || installonly_install_keys.count(action.package_key) > 0) {
        error_out = _("Pending package actions contain conflicting package identities.");
        return false;
      }
      installed_side_keys.insert(action.package_key);
    }

    switch (action.type) {
    case PendingAction::INSTALL:
      request.install.push_back(action.transaction_spec);
      break;
    case PendingAction::UPGRADE:
      request.upgrade.push_back(action.transaction_spec);
      break;
    case PendingAction::DOWNGRADE:
      request.downgrade.push_back(action.transaction_spec);
      break;
    case PendingAction::REMOVE:
      request.remove.push_back(action.transaction_spec);
      break;
    case PendingAction::REINSTALL:
      request.reinstall.push_back(action.transaction_spec);
      break;
    default:
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
  request = {};

  TransactionRequest built_request;
  if (!build_pending_transaction_specs(actions, built_request, error_out)) {
    return false;
  }

  request = std::move(built_request);
  return true;
}

// -----------------------------------------------------------------------------
// Reject direct downgrade, remove, or reinstall requests for the package owning the running GUI.
// Selected upgrades are allowed here and checked again after dnf5daemon resolves the preview.
// -----------------------------------------------------------------------------
bool
pending_transaction_validate_request(const TransactionRequest &request, std::string &error_out)
{
  const bool needs_direct_validation =
      !request.downgrade.empty() || !request.remove.empty() || !request.reinstall.empty();
  if (!needs_direct_validation) {
    // Install and upgrade requests are validated by dnf5daemon preview resolution.
    return true;
  }

  PendingRequestBaseDropGuard base_drop_guard;

  try {
    for (const auto &spec : request.downgrade) {
      if (dnf_backend_is_self_protected_transaction_spec(spec)) {
        error_out = _("DNF UI cannot downgrade the package that owns the running application while it is running.");
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
