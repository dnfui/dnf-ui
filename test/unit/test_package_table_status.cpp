// -----------------------------------------------------------------------------
// test_package_table_status.cpp
// Package table status effect tests
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "ui/package_table_status.hpp"

// -----------------------------------------------------------------------------
// Verify that a marked package keeps its pending status even when it also
// appears in a resolved preview.
// -----------------------------------------------------------------------------
TEST_CASE("Package status effect prefers marked actions over preview actions")
{
  PendingTransactionWidgets transaction;
  transaction.actions.push_back({ PendingAction::INSTALL, "demo-1-1.x86_64" });
  transaction.prepared_preview.remove.push_back("demo-1-1.x86_64");

  REQUIRE(package_table_status_effect_for_row(transaction, "demo-1-1.x86_64", "") ==
          PackageTableStatusEffect::PENDING_INSTALL);
}

// -----------------------------------------------------------------------------
// Verify that preview effects can match the alternate package ID used by
// upgradeable rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package status effect matches preview actions by alternate package ID")
{
  PendingTransactionWidgets transaction;
  transaction.show_required_package_changes = true;
  transaction.prepared_preview.upgrade.push_back("demo-2-1.x86_64");

  REQUIRE(package_table_status_effect_for_row(transaction, "demo-1-1.x86_64", "demo-2-1.x86_64") ==
          PackageTableStatusEffect::PREVIEW_UPGRADE);
}

// -----------------------------------------------------------------------------
// Verify that required package effects stay hidden when the menu toggle is off.
// -----------------------------------------------------------------------------
TEST_CASE("Package status effect hides preview actions when required changes are disabled")
{
  PendingTransactionWidgets transaction;
  transaction.prepared_preview.install.push_back("dependency-1-1.x86_64");

  REQUIRE(package_table_status_effect_for_row(transaction, "dependency-1-1.x86_64", "") ==
          PackageTableStatusEffect::NONE);
}

// -----------------------------------------------------------------------------
// Verify that unrelated packages keep their normal status.
// -----------------------------------------------------------------------------
TEST_CASE("Package status effect ignores unrelated preview actions")
{
  PendingTransactionWidgets transaction;
  transaction.show_required_package_changes = true;
  transaction.prepared_preview.install.push_back("dependency-1-1.x86_64");

  REQUIRE(package_table_status_effect_for_row(transaction, "demo-1-1.x86_64", "") == PackageTableStatusEffect::NONE);
}
