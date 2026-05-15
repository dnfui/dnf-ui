#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"
#include "ui/package_table_view_internal.hpp"

static PackageRow
make_table_test_row(const std::string &nevra,
                    const std::string &name,
                    const std::string &version,
                    const std::string &release,
                    const std::string &arch)
{
  PackageRow row;
  row.nevra = nevra;
  row.name = name;
  row.version = version;
  row.release = release;
  row.arch = arch;
  row.repo = "fedora";
  row.summary = "Test package";
  return row;
}

// -----------------------------------------------------------------------------
// Verify that the package table Version column matches the Info tab Version
// field and does not include the package release.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version column shows only package version")
{
  reset_backend_globals();

  PackageItem item {};
  item.row = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == "1.2.3");
}

// -----------------------------------------------------------------------------
// Verify that an available update row still shows the installed package version.
// The release is kept out of the table because the column is not named Release.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version column uses installed version for update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item {};
  item.row = update;

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == "1.2.3");
}
