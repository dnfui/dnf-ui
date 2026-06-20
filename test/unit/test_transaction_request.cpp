// -----------------------------------------------------------------------------
// test/unit/test_transaction_request.cpp
// Shared transaction request contract tests
// Exercises transaction request validation before D-Bus or libdnf work begins.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "transaction_request.hpp"

#include <string>

// -----------------------------------------------------------------------------
// TransactionRequest basic state tests
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that empty state and item count include explicit actions and upgrade all.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request empty state and item count reflect queued actions")
{
  TransactionRequest request;

  REQUIRE(request.empty());
  REQUIRE(request.item_count() == 0);

  request.install.push_back("example-install-spec");
  request.upgrade.push_back("example-upgrade-spec");
  request.remove.push_back("example-remove-spec");
  request.reinstall.push_back("example-reinstall-spec");

  REQUIRE_FALSE(request.empty());
  REQUIRE(request.item_count() == 4);

  TransactionRequest upgrade_all_request;
  upgrade_all_request.upgrade_all = true;

  REQUIRE_FALSE(upgrade_all_request.empty());
  REQUIRE(upgrade_all_request.item_count() == 1);
}

// -----------------------------------------------------------------------------
// TransactionRequest validation tests
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that validation rejects requests with no package operation.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects an empty request")
{
  TransactionRequest request;
  std::string error = "stale";

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request is empty.");
}

// -----------------------------------------------------------------------------
// Verify that validation rejects empty install specs before service work starts.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects an empty install package spec")
{
  TransactionRequest request;
  std::string error;

  request.install.push_back("");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains an empty install package spec.");
}

// -----------------------------------------------------------------------------
// Verify that validation rejects empty upgrade specs before service work starts.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects an empty upgrade package spec")
{
  TransactionRequest request;
  std::string error;

  request.upgrade.push_back("");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains an empty upgrade package spec.");
}

// -----------------------------------------------------------------------------
// Verify that validation rejects empty remove specs before service work starts.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects an empty remove package spec")
{
  TransactionRequest request;
  std::string error;

  request.remove.push_back("");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains an empty remove package spec.");
}

// -----------------------------------------------------------------------------
// Verify that validation rejects empty reinstall specs before service work starts.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects an empty reinstall package spec")
{
  TransactionRequest request;
  std::string error;

  request.reinstall.push_back("");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains an empty reinstall package spec.");
}

// -----------------------------------------------------------------------------
// Verify that validation enforces the request item limit.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects too many package actions")
{
  TransactionRequest request;
  std::string error;

  request.install.assign(kTransactionRequestMaxItems + 1, "example-install-spec");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains too many package actions.");
}

// -----------------------------------------------------------------------------
// Verify that upgrade all cannot be combined with explicit package actions.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects mixed upgrade-all requests")
{
  TransactionRequest request;
  std::string error;

  request.upgrade_all = true;
  request.upgrade.push_back("example-upgrade-spec");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Upgrade all cannot be combined with other package actions.");
}

// -----------------------------------------------------------------------------
// Verify that upgrade all is valid as a standalone request.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation accepts upgrade-all requests")
{
  TransactionRequest request;
  std::string error = "stale";

  request.upgrade_all = true;

  REQUIRE(request.validate(error));
  REQUIRE(error.empty());
}

// -----------------------------------------------------------------------------
// Verify that validation enforces the package spec length limit.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects package specs that are too long")
{
  TransactionRequest request;
  std::string error;

  request.install.push_back(std::string(kTransactionRequestMaxSpecLength + 1, 'x'));

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains a package spec that is too long.");
}

// -----------------------------------------------------------------------------
// Verify that validation rejects the same package spec repeated in one action list.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects duplicate package specs")
{
  TransactionRequest request;
  std::string error;

  request.upgrade.push_back("example-upgrade-spec");
  request.upgrade.push_back("example-upgrade-spec");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains a duplicate upgrade package spec.");
}

// -----------------------------------------------------------------------------
// Verify that validation rejects the same package spec in different action lists.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation rejects conflicting package actions")
{
  TransactionRequest request;
  std::string error;

  request.install.push_back("example-package-spec");
  request.upgrade.push_back("example-package-spec");

  REQUIRE_FALSE(request.validate(error));
  REQUIRE(error == "Transaction request contains conflicting package actions.");
}

// -----------------------------------------------------------------------------
// Verify that validation accepts normal mixed package action requests.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction request validation accepts mixed non empty package specs")
{
  TransactionRequest request;
  std::string error = "stale";

  request.install.push_back("example-install-spec");
  request.upgrade.push_back("example-upgrade-spec");
  request.remove.push_back("example-remove-spec");
  request.reinstall.push_back("example-reinstall-spec");

  REQUIRE(request.validate(error));
  REQUIRE(error.empty());
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
