// -----------------------------------------------------------------------------
// src/transaction/transaction_request.hpp
// Shared transaction request model
// Carries the package specs the user explicitly marked in the GUI.
// Dependency driven upgrades and downgrades are resolved later when the preview is built.
// -----------------------------------------------------------------------------
#pragma once

#include "i18n.hpp"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

constexpr size_t kTransactionRequestMaxSpecLength = 4096;

// -----------------------------------------------------------------------------
// Transaction request shared by the GUI and transaction client.
// -----------------------------------------------------------------------------
struct TransactionRequest {
  // Upgrade all installed packages.
  bool upgrade_all = false;
  // Package specs explicitly marked for install.
  std::vector<std::string> install;
  // Package specs explicitly marked for upgrade.
  std::vector<std::string> upgrade;
  // Package specs explicitly marked for downgrade.
  std::vector<std::string> downgrade;
  // Package specs explicitly marked for removal.
  std::vector<std::string> remove;
  // Package specs explicitly marked for reinstall.
  std::vector<std::string> reinstall;

  // -----------------------------------------------------------------------------
  // Return true when no package actions have been queued.
  // -----------------------------------------------------------------------------
  bool empty() const
  {
    return !upgrade_all && install.empty() && upgrade.empty() && downgrade.empty() && remove.empty() &&
        reinstall.empty();
  }

  // -----------------------------------------------------------------------------
  // Reject empty, invalid, duplicate, or conflicting requests before they reach the transaction client.
  // -----------------------------------------------------------------------------
  bool validate(std::string &error_out) const
  {
    error_out.clear();

    if (empty()) {
      error_out = _("Transaction request is empty.");
      return false;
    }

    // Upgrade all is a separate request type and must not include selected packages.
    if (upgrade_all &&
        (!install.empty() || !upgrade.empty() || !downgrade.empty() || !remove.empty() || !reinstall.empty())) {
      error_out = _("Upgrade all cannot be combined with other package actions.");
      return false;
    }

    // Validate one action list and reject repeated specs in that list.
    auto validate_specs = [&](const std::vector<std::string> &specs, const char *kind) {
      std::set<std::string> seen;
      for (const auto &spec : specs) {
        if (spec.empty()) {
          error_out = dnfui_i18n_format(_("Transaction request contains an empty %s package spec."), kind);
          return false;
        }
        if (spec.size() > kTransactionRequestMaxSpecLength) {
          error_out = _("Transaction request contains a package spec that is too long.");
          return false;
        }
        if (!seen.insert(spec).second) {
          error_out = dnfui_i18n_format(_("Transaction request contains a duplicate %s package spec."), kind);
          return false;
        }
      }
      return true;
    };

    // Check each action list before comparing the lists with each other.
    if (!validate_specs(install, _("install")) || !validate_specs(upgrade, _("upgrade")) ||
        !validate_specs(downgrade, _("downgrade")) || !validate_specs(remove, _("remove")) ||
        !validate_specs(reinstall, _("reinstall"))) {
      return false;
    }

    // A package spec must not appear in more than one action list.
    std::set<std::string> requested_specs;
    auto has_conflict = [&](const std::vector<std::string> &specs) {
      for (const auto &spec : specs) {
        if (!requested_specs.insert(spec).second) {
          return true;
        }
      }
      return false;
    };

    // Reject requests that ask for two different actions for the same package spec.
    if (has_conflict(install) || has_conflict(upgrade) || has_conflict(downgrade) || has_conflict(remove) ||
        has_conflict(reinstall)) {
      error_out = _("Transaction request contains conflicting package actions.");
      return false;
    }

    return true;
  }
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
