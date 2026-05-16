// -----------------------------------------------------------------------------
// test/unit/test_transaction_service_request_parser.cpp
// Transaction service request parser tests
// Verifies the small D-Bus tuple parser used by StartTransaction before the
// service validates or resolves the request.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "service/transaction_service_request_parser.hpp"

#include <glib.h>

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Verify that StartTransaction parameters keep install, remove, and reinstall
// arrays in the same order as the D-Bus method contract.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service request parser keeps package action arrays in order")
{
  const char *install[] = { "install-one", "install-two", nullptr };
  const char *remove[] = { "remove-one", nullptr };
  const char *reinstall[] = { "reinstall-one", "reinstall-two", nullptr };

  GVariant *parameters = g_variant_ref_sink(g_variant_new("(^as^as^as)", install, remove, reinstall));
  TransactionRequest request = transaction_service_request_from_variant(parameters);

  REQUIRE(request.install == std::vector<std::string> { "install-one", "install-two" });
  REQUIRE(request.remove == std::vector<std::string> { "remove-one" });
  REQUIRE(request.reinstall == std::vector<std::string> { "reinstall-one", "reinstall-two" });
  REQUIRE_FALSE(request.upgrade_all);

  g_variant_unref(parameters);
}

// -----------------------------------------------------------------------------
// Verify that empty D-Bus arrays become an empty package transaction request.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction service request parser accepts empty package action arrays")
{
  const char *install[] = { nullptr };
  const char *remove[] = { nullptr };
  const char *reinstall[] = { nullptr };

  GVariant *parameters = g_variant_ref_sink(g_variant_new("(^as^as^as)", install, remove, reinstall));
  TransactionRequest request = transaction_service_request_from_variant(parameters);

  REQUIRE(request.install.empty());
  REQUIRE(request.remove.empty());
  REQUIRE(request.reinstall.empty());
  REQUIRE_FALSE(request.upgrade_all);

  g_variant_unref(parameters);
}
