// -----------------------------------------------------------------------------
// Pending transaction request tests
// Covers conversion from marked UI actions to the transaction request model.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"
#include "transaction_request.hpp"
#include "ui/transaction/pending_transaction_request.hpp"
#include "ui/common/widgets.hpp"

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Replace the existing pending action for one package identity, like the UI controller does.
// -----------------------------------------------------------------------------
static void
replace_pending_action_for_package(MainWindowUiState &widgets, const PendingAction &action)
{
  pending_actions_remove_package_key(widgets.transaction.actions, action.package_key);
  widgets.transaction.actions.push_back(action);
}

// -----------------------------------------------------------------------------
// Verify that marked UI actions are copied into the matching request lists.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction request builder splits actions by operation type")
{
  std::vector<PendingAction> actions = {
    { PendingAction::INSTALL, "demo-install-1-1.x86_64", "", "demo-install\nx86_64" },
    { PendingAction::UPGRADE, "demo-upgrade-2-1.x86_64", "demo.x86_64", "demo-upgrade\nx86_64" },
    { PendingAction::DOWNGRADE, "demo-downgrade-1-1.x86_64", "", "demo-downgrade\nx86_64" },
    { PendingAction::REMOVE, "demo-remove-1-1.x86_64", "", "demo-remove\nx86_64" },
    { PendingAction::REINSTALL, "demo-reinstall-1-1.x86_64", "", "demo-reinstall\nx86_64" },
    { PendingAction::INSTALL, "demo-install-libs-1-1.x86_64", "", "demo-install-libs\nx86_64" },
  };

  TransactionRequest request;
  std::string error;

  REQUIRE(pending_transaction_build_request(actions, request, error));
  REQUIRE(error.empty());

  REQUIRE(request.install ==
          std::vector<std::string> {
              "demo-install-1-1.x86_64",
              "demo-install-libs-1-1.x86_64",
          });
  REQUIRE(request.upgrade ==
          std::vector<std::string> {
              "demo.x86_64",
          });
  REQUIRE(request.downgrade ==
          std::vector<std::string> {
              "demo-downgrade-1-1.x86_64",
          });
  REQUIRE(request.remove ==
          std::vector<std::string> {
              "demo-remove-1-1.x86_64",
          });
  REQUIRE(request.reinstall ==
          std::vector<std::string> {
              "demo-reinstall-1-1.x86_64",
          });
}

// -----------------------------------------------------------------------------
// Verify that building a request replaces stale data from an earlier request.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction request builder clears stale request data")
{
  TransactionRequest request;
  request.upgrade_all = true;
  request.install.push_back("old-install");
  request.upgrade.push_back("old-upgrade");
  request.downgrade.push_back("old-downgrade");
  request.remove.push_back("old-remove");
  request.reinstall.push_back("old-reinstall");
  std::string error;

  std::vector<PendingAction> actions = {
    { PendingAction::REMOVE, "demo-remove-1-1.x86_64", "", "demo-remove\nx86_64" },
  };

  REQUIRE(pending_transaction_build_request(actions, request, error));
  REQUIRE(error.empty());

  REQUIRE_FALSE(request.upgrade_all);
  REQUIRE(request.install.empty());
  REQUIRE(request.upgrade.empty());
  REQUIRE(request.downgrade.empty());
  REQUIRE(request.remove ==
          std::vector<std::string> {
              "demo-remove-1-1.x86_64",
          });
  REQUIRE(request.reinstall.empty());
}

// -----------------------------------------------------------------------------
// Verify that unknown pending action values are rejected instead of becoming removals.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction request builder rejects unknown action types")
{
  std::vector<PendingAction> actions = {
    { PendingAction::INSTALL, "demo-install-1-1.x86_64", "", "demo-install\nx86_64" },
    { static_cast<PendingAction::Type>(999), "demo-unknown-1-1.x86_64", "", "demo-unknown\nx86_64" },
  };

  TransactionRequest request;
  std::string error;

  REQUIRE_FALSE(pending_transaction_build_request(actions, request, error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(request.install.empty());
  REQUIRE(request.upgrade.empty());
  REQUIRE(request.downgrade.empty());
  REQUIRE(request.remove.empty());
  REQUIRE(request.reinstall.empty());
}

// -----------------------------------------------------------------------------
// Verify that changing from upgrade to downgrade keeps one action for the package.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction package identity replaces upgrade with downgrade")
{
  MainWindowUiState widgets;
  const std::string package_key = "demo\nx86_64";

  replace_pending_action_for_package(widgets,
                                     { PendingAction::UPGRADE, "demo-2.0-1.fc44.x86_64", "demo.x86_64", package_key });
  replace_pending_action_for_package(
      widgets, { PendingAction::DOWNGRADE, "demo-1.0-1.fc44.x86_64", "demo-1.0-1.fc44.x86_64", package_key });

  REQUIRE(widgets.transaction.actions.size() == 1);
  REQUIRE(widgets.transaction.actions[0].type == PendingAction::DOWNGRADE);
  REQUIRE(widgets.transaction.actions[0].nevra == "demo-1.0-1.fc44.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that selecting another older version replaces the previous downgrade.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction package identity replaces downgrade version")
{
  MainWindowUiState widgets;
  const std::string package_key = "demo\nx86_64";

  replace_pending_action_for_package(
      widgets, { PendingAction::DOWNGRADE, "demo-1.0-1.fc44.x86_64", "demo-1.0-1.fc44.x86_64", package_key });
  replace_pending_action_for_package(
      widgets, { PendingAction::DOWNGRADE, "demo-1.1-1.fc44.x86_64", "demo-1.1-1.fc44.x86_64", package_key });

  REQUIRE(widgets.transaction.actions.size() == 1);
  REQUIRE(widgets.transaction.actions[0].type == PendingAction::DOWNGRADE);
  REQUIRE(widgets.transaction.actions[0].nevra == "demo-1.1-1.fc44.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that selecting another available version replaces the previous install.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction package identity replaces install version")
{
  MainWindowUiState widgets;
  const std::string package_key = "demo\nx86_64";

  replace_pending_action_for_package(
      widgets, { PendingAction::INSTALL, "demo-1.0-1.fc44.x86_64", "demo-1.0-1.fc44.x86_64", package_key });
  replace_pending_action_for_package(
      widgets, { PendingAction::INSTALL, "demo-2.0-1.fc44.x86_64", "demo-2.0-1.fc44.x86_64", package_key });

  REQUIRE(widgets.transaction.actions.size() == 1);
  REQUIRE(widgets.transaction.actions[0].type == PendingAction::INSTALL);
  REQUIRE(widgets.transaction.actions[0].nevra == "demo-2.0-1.fc44.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that ordinary non protected package requests pass UI validation.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction request validation accepts non protected requests")
{
  TransactionRequest request;
  request.install.push_back("demo-install-1-1.x86_64");
  request.upgrade.push_back("demo-upgrade-1-1.x86_64");
  request.downgrade.push_back("demo-downgrade-1-1.x86_64");
  request.remove.push_back("demo-remove-1-1.x86_64");
  request.reinstall.push_back("demo-reinstall-1-1.x86_64");
  std::string error;

  REQUIRE(pending_transaction_validate_request(request, error));
  REQUIRE(error.empty());
}

// -----------------------------------------------------------------------------
// Verify that selected installs and upgrades are not blocked before dnf5daemon resolves them.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction request validation allows protected install and upgrade specs")
{
  reset_backend_globals();
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", "dnf-ui");

  TransactionRequest request;
  request.install.push_back("dnf-ui");
  request.upgrade.push_back("dnf-ui-0.2.3-1.fc44.x86_64");
  std::string error;

  REQUIRE(pending_transaction_validate_request(request, error));
  REQUIRE(error.empty());

  reset_backend_globals();
}

// -----------------------------------------------------------------------------
// Verify that direct destructive requests for the running app are still rejected.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction request validation rejects protected remove and reinstall specs")
{
  reset_backend_globals();
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", "dnf-ui");

  TransactionRequest remove_request;
  remove_request.remove.push_back("dnf-ui");
  std::string remove_error;

  REQUIRE_FALSE(pending_transaction_validate_request(remove_request, remove_error));
  REQUIRE_FALSE(remove_error.empty());

  TransactionRequest reinstall_request;
  reinstall_request.reinstall.push_back("dnf-ui");
  std::string reinstall_error;

  REQUIRE_FALSE(pending_transaction_validate_request(reinstall_request, reinstall_error));
  REQUIRE_FALSE(reinstall_error.empty());

  TransactionRequest downgrade_request;
  downgrade_request.downgrade.push_back("dnf-ui");
  std::string downgrade_error;

  REQUIRE_FALSE(pending_transaction_validate_request(downgrade_request, downgrade_error));
  REQUIRE_FALSE(downgrade_error.empty());

  reset_backend_globals();
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
