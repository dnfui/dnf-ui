// -----------------------------------------------------------------------------
// src/transaction_request.hpp
// Shared transaction request model
// Carries the package specs the user explicitly marked in the GUI. Dependency
// driven upgrades and downgrades are resolved later when the preview is built.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

constexpr size_t kTransactionRequestMaxItems = 256;
constexpr size_t kTransactionRequestMaxSpecLength = 4096;

// -----------------------------------------------------------------------------
// Transaction request shared by the GUI and transaction client.
// -----------------------------------------------------------------------------
struct TransactionRequest {
  // Upgrade all installed packages.
  bool upgrade_all = false;
  // Package specs explicitly marked for install.
  std::vector<std::string> install;
  // Package specs explicitly marked for removal.
  std::vector<std::string> remove;
  // Package specs explicitly marked for reinstall.
  std::vector<std::string> reinstall;

  // -----------------------------------------------------------------------------
  // Return true when no package actions have been queued.
  // -----------------------------------------------------------------------------
  bool empty() const
  {
    return !upgrade_all && install.empty() && remove.empty() && reinstall.empty();
  }

  // -----------------------------------------------------------------------------
  // Return the total number of requested package actions.
  // -----------------------------------------------------------------------------
  size_t item_count() const
  {
    return (upgrade_all ? 1 : 0) + install.size() + remove.size() + reinstall.size();
  }

  // -----------------------------------------------------------------------------
  // Reject empty, oversized, duplicate, or conflicting requests before they reach the service.
  // -----------------------------------------------------------------------------
  bool validate(std::string &error_out) const
  {
    error_out.clear();

    if (empty()) {
      error_out = "Transaction request is empty.";
      return false;
    }

    // Upgrade all is a separate request type and must not include selected packages.
    if (upgrade_all && (!install.empty() || !remove.empty() || !reinstall.empty())) {
      error_out = "Upgrade all cannot be combined with other package actions.";
      return false;
    }

    // Keep request size limited before passing it to DNF.
    if (item_count() > kTransactionRequestMaxItems) {
      error_out = "Transaction request contains too many package actions.";
      return false;
    }

    // Validate one action list and reject repeated specs in that list.
    auto validate_specs = [&](const std::vector<std::string> &specs, const char *kind) {
      std::set<std::string> seen;
      for (const auto &spec : specs) {
        if (spec.empty()) {
          error_out = std::string("Transaction request contains an empty ") + kind + " package spec.";
          return false;
        }
        if (spec.size() > kTransactionRequestMaxSpecLength) {
          error_out = std::string("Transaction request contains a package spec that is too long.");
          return false;
        }
        if (!seen.insert(spec).second) {
          error_out = std::string("Transaction request contains a duplicate ") + kind + " package spec.";
          return false;
        }
      }
      return true;
    };

    // Check each action list before comparing the lists with each other.
    if (!validate_specs(install, "install") || !validate_specs(remove, "remove") ||
        !validate_specs(reinstall, "reinstall")) {
      return false;
    }

    // A package spec must not appear in more than one action list.
    auto has_conflict = [](const std::vector<std::string> &left, const std::vector<std::string> &right) {
      std::set<std::string> left_specs(left.begin(), left.end());
      for (const auto &spec : right) {
        if (left_specs.count(spec) > 0) {
          return true;
        }
      }
      return false;
    };

    // Reject requests that ask for two different actions for the same package spec.
    if (has_conflict(install, remove) || has_conflict(install, reinstall) || has_conflict(remove, reinstall)) {
      error_out = "Transaction request contains conflicting package actions.";
      return false;
    }

    return true;
  }
};
