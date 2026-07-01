#include <catch2/catch_test_macros.hpp>

#include "ui/package_table/package_table_export_csv.hpp"

// -----------------------------------------------------------------------------
// Verify that the CSV formatter writes simple headers and rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package table CSV export writes headers and rows")
{
  std::string csv = package_table_export_csv_text({ "Name", "Version" }, { { "bash", "5.3.9" } });

  REQUIRE(csv == "Name,Version\nbash,5.3.9\n");
}

// -----------------------------------------------------------------------------
// Verify that fields with CSV control characters are quoted correctly.
// -----------------------------------------------------------------------------
TEST_CASE("Package table CSV export escapes quoted fields")
{
  std::string csv = package_table_export_csv_text({ "Name", "Summary" },
                                                  { { "demo", "Contains comma, quote \" and newline\ntext" } });

  REQUIRE(csv == "Name,Summary\ndemo,\"Contains comma, quote \"\" and newline\ntext\"\n");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
