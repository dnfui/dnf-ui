// -----------------------------------------------------------------------------
// src/transaction/transaction_preview.hpp
// Shared transaction preview model
// Carries the resolved daemon actions shown before Apply.
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

struct TransactionPreviewPackage {
  std::string label;
  std::string name;
  std::string arch;

  std::string name_arch_key() const
  {
    return name + "\n" + arch;
  }
};

// Resolved transaction preview used by the confirmation dialog before apply.
// The model must fully describe every resolved transaction action.
// Callers must never receive a partial preview when the backend cannot represent the whole resolved transaction.
struct TransactionPreview {
  // Human-readable warnings returned with a successful daemon resolve.
  std::string resolve_warnings;
  std::vector<TransactionPreviewPackage> install;
  std::vector<TransactionPreviewPackage> upgrade;
  std::vector<TransactionPreviewPackage> downgrade;
  std::vector<TransactionPreviewPackage> reinstall;
  std::vector<TransactionPreviewPackage> remove;
  std::vector<TransactionPreviewPackage> replaced;
  long long disk_space_delta = 0;
  bool requires_offline = false;

  // -----------------------------------------------------------------------------
  // Return true when the preview contains no resolved package actions.
  // -----------------------------------------------------------------------------
  bool empty() const
  {
    return install.empty() && upgrade.empty() && downgrade.empty() && reinstall.empty() && remove.empty() &&
        replaced.empty();
  }
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
