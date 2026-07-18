#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"
#include "ui/package_table/package_table_view_internal.hpp"

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

// -----------------------------------------------------------------------------
// Verify that an available update row shows the version that would be installed.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update column uses candidate version for update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item {};
  item.row = update;

  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.4");
}

// -----------------------------------------------------------------------------
// Verify that a non upgradable row leaves the Update column empty.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update column is empty for normal rows")
{
  reset_backend_globals();

  PackageItem item {};
  item.row = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");

  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION).empty());
}

// -----------------------------------------------------------------------------
// Verify that the Release column shows the package release.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Release column shows package release")
{
  reset_backend_globals();

  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");

  PackageItem item {};
  item.row = update;

  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == "1.fc44");
}

// -----------------------------------------------------------------------------
// Verify that installed-list rows keep installed and update releases separate.
// -----------------------------------------------------------------------------
TEST_CASE("Package table release columns handle installed rows with update candidates")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-1.2.4-1.fc44.x86_64";
  installed.repo_candidate_version = "1.2.4";
  installed.repo_candidate_release = "1.fc44";
  installed.repo_candidate_repo = "updates";

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item {};
  item.row = installed;

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == "1.2.3");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.4");
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == "4.fc44");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE) == "1.fc44");
}

// -----------------------------------------------------------------------------
// Verify that installed-list rows show the repo that provides the update.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Repo column uses candidate repo for installed update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  installed.repo = "@System";
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-1.2.4-1.fc44.x86_64";
  installed.repo_candidate_version = "1.2.4";
  installed.repo_candidate_release = "1.fc44";
  installed.repo_candidate_repo = "updates";

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item {};
  item.row = installed;

  REQUIRE(package_table_column_text(item, PackageColumnKind::REPO) == "updates");
}

// -----------------------------------------------------------------------------
// Verify that an available update row can show the release that would be installed.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Release column uses candidate release for update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item {};
  item.row = update;

  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE) == "1.fc44");
}

// -----------------------------------------------------------------------------
// Verify that an available update row shows the repository that provides the update.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Repo column uses candidate repo for update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  installed.repo = "@System";

  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");
  update.repo = "updates";

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item {};
  item.row = update;

  REQUIRE(package_table_column_text(item, PackageColumnKind::REPO) == "updates");
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade rows use daemon target values for update columns.
// -----------------------------------------------------------------------------
TEST_CASE("Package table update columns use daemon upgrade target")
{
  reset_backend_globals();

  PackageItem item {};
  item.row = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");
  item.row.repo = "metadata-repo";

  TransactionServiceUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "1.2.5";
  target.release = "2.fc44";
  target.nevra = "demo-1.2.5-2.fc44.x86_64";
  target.full_nevra = target.nevra;
  target.repo_id = "daemon-repo";

  item.upgrade_target = target;

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION).empty());
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.5");
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE).empty());
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE) == "2.fc44");
  REQUIRE(package_table_column_text(item, PackageColumnKind::REPO) == "daemon-repo");
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade rows keep installed and target versions separate.
// -----------------------------------------------------------------------------
TEST_CASE("Package table daemon upgrade rows use installed version when available")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");
  installed.repo = "@System";
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item {};
  item.row = make_table_test_row("demo-1.2.5-2.fc44.x86_64", "demo", "1.2.5", "2.fc44", "x86_64");

  TransactionServiceUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "1.2.5";
  target.release = "2.fc44";
  target.nevra = "demo-1.2.5-2.fc44.x86_64";
  target.full_nevra = target.nevra;
  target.repo_id = "daemon-repo";

  item.upgrade_target = target;

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == "1.2.4");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.5");
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == "1.fc44");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE) == "2.fc44");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
