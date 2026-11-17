// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_resolved_actions.cpp
// Read-only package table action lookups from resolved transaction previews.
// -----------------------------------------------------------------------------
#include "ui/transaction/pending_transaction_resolved_actions.hpp"

#include "transaction/transaction_preview.hpp"

#include <set>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Record exact packages and only unambiguous name and architecture pairs.
// -----------------------------------------------------------------------------
void
append_resolved_actions(PendingTransactionResolvedActions &actions,
                        const std::vector<TransactionPreviewPackage> &packages,
                        PendingAction::Type type,
                        std::set<std::string> &ambiguous_keys)
{
  for (const auto &package : packages) {
    actions.by_nevra.emplace(package.label, type);

    const std::string key = package.name_arch_key();
    if (ambiguous_keys.count(key) != 0) {
      continue;
    }

    if (!actions.by_name_arch.emplace(key, type).second) {
      actions.by_name_arch.erase(key);
      ambiguous_keys.insert(key);
    }
  }
}

} // namespace

// -----------------------------------------------------------------------------
// Build display-only markings from actions selected by dnf5daemon.
// Replaced entries are not removal actions and remain in the final preview.
// -----------------------------------------------------------------------------
PendingTransactionResolvedActions
pending_transaction_resolved_actions_from_preview(const TransactionPreview &preview)
{
  PendingTransactionResolvedActions actions;
  std::set<std::string> ambiguous_keys;

  append_resolved_actions(actions, preview.install, PendingAction::INSTALL, ambiguous_keys);
  append_resolved_actions(actions, preview.upgrade, PendingAction::UPGRADE, ambiguous_keys);
  append_resolved_actions(actions, preview.downgrade, PendingAction::DOWNGRADE, ambiguous_keys);
  append_resolved_actions(actions, preview.reinstall, PendingAction::REINSTALL, ambiguous_keys);
  append_resolved_actions(actions, preview.remove, PendingAction::REMOVE, ambiguous_keys);

  return actions;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
