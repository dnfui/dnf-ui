#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"

// -----------------------------------------------------------------------------
// Verify that history rows format package versions like package table rows.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction history package rows format versions")
{
  TransactionHistoryPackageRow row;
  row.version = "1.2.3";
  row.release = "4.fc44";

  REQUIRE(row.display_version() == "1.2.3-4.fc44");
}

// -----------------------------------------------------------------------------
// Verify that transaction history actions have user-facing labels.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction history actions have labels")
{
  REQUIRE(dnf_backend_transaction_history_action_to_string(TransactionHistoryAction::INSTALL) == "Install");
  REQUIRE(dnf_backend_transaction_history_action_to_string(TransactionHistoryAction::UPGRADE) == "Upgrade");
  REQUIRE(dnf_backend_transaction_history_action_to_string(TransactionHistoryAction::REMOVE) == "Remove");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
