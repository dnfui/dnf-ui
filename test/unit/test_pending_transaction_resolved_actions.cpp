// -----------------------------------------------------------------------------
// test_pending_transaction_resolved_actions.cpp
// Resolved transaction markings are display-only and identify exact packages.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "transaction/transaction_preview.hpp"
#include "ui/transaction/pending_transaction_resolved_actions.hpp"

// -----------------------------------------------------------------------------
// Preserve daemon action types without treating replaced packages as removals.
// -----------------------------------------------------------------------------
TEST_CASE("Resolved transaction markings preserve daemon action types")
{
  TransactionPreview preview;
  preview.install.push_back({ "dependency-1-1.x86_64", "dependency", "x86_64" });
  preview.upgrade.push_back({ "library-2-1.x86_64", "library", "x86_64" });
  preview.downgrade.push_back({ "older-1-1.x86_64", "older", "x86_64" });
  preview.reinstall.push_back({ "again-1-1.x86_64", "again", "x86_64" });
  preview.remove.push_back({ "dependent-1-1.x86_64", "dependent", "x86_64" });
  preview.replaced.push_back({ "library-1-1.x86_64", "library", "x86_64" });

  const PendingTransactionResolvedActions actions = pending_transaction_resolved_actions_from_preview(preview);

  REQUIRE(actions.by_nevra.at("dependency-1-1.x86_64") == PendingAction::INSTALL);
  REQUIRE(actions.by_nevra.at("library-2-1.x86_64") == PendingAction::UPGRADE);
  REQUIRE(actions.by_nevra.at("older-1-1.x86_64") == PendingAction::DOWNGRADE);
  REQUIRE(actions.by_nevra.at("again-1-1.x86_64") == PendingAction::REINSTALL);
  REQUIRE(actions.by_nevra.at("dependent-1-1.x86_64") == PendingAction::REMOVE);
  REQUIRE(actions.by_nevra.count("library-1-1.x86_64") == 0);
  REQUIRE(actions.by_name_arch.at("dependency\nx86_64") == PendingAction::INSTALL);
}

// -----------------------------------------------------------------------------
// Project exact install-only versions only when the package stream is unique.
// -----------------------------------------------------------------------------
TEST_CASE("Resolved transaction markings do not project multiple versions")
{
  TransactionPreview preview;
  preview.install.push_back({ "kernel-1-1.x86_64", "kernel", "x86_64" });
  preview.install.push_back({ "kernel-2-1.x86_64", "kernel", "x86_64" });
  preview.install.push_back({ "other-1-1.x86_64", "other", "x86_64" });

  const PendingTransactionResolvedActions actions = pending_transaction_resolved_actions_from_preview(preview);

  REQUIRE(actions.by_nevra.count("kernel-1-1.x86_64") == 1);
  REQUIRE(actions.by_nevra.count("kernel-2-1.x86_64") == 1);
  REQUIRE(actions.by_name_arch.count("kernel\nx86_64") == 0);
  REQUIRE(actions.by_name_arch.at("other\nx86_64") == PendingAction::INSTALL);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
