#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"
#include "ui/common/widgets.hpp"
#include "ui/package_table/package_table_view_internal.hpp"

#include <memory>

static PackageRow
make_table_test_row(const std::string &nevra,
                    const std::string &name,
                    const std::string &version,
                    const std::string &release,
                    const std::string &arch,
                    const std::string &epoch = "")
{
  PackageRow row;
  row.nevra = nevra;
  row.name = name;
  row.epoch = epoch;
  row.version = version;
  row.release = release;
  row.arch = arch;
  row.repo = "fedora";
  row.summary = "Test package";
  return row;
}

static PackageItem
make_table_test_item(const PackageRow &row, bool compact_view = true)
{
  PackageItem item {};
  item.row = row;
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_display_values(item, resolution, compact_view);
  return item;
}

static int
compare_table_test_rows(MainWindowUiState &widgets,
                        const PackageRow &left,
                        const PackageRow &right,
                        PackageColumnKind kind)
{
  GObject *left_object = make_package_object(&widgets, left);
  GObject *right_object = make_package_object(&widgets, right);
  int result =
      package_table_column_sorter_compare(left_object, right_object, GINT_TO_POINTER(static_cast<int>(kind) + 1));
  g_object_unref(left_object);
  g_object_unref(right_object);
  return result;
}

// -----------------------------------------------------------------------------
// Verify that the package table Version column matches the Details tab Version
// field and does not include the package release.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version column shows only package version")
{
  reset_backend_globals();

  PackageItem item =
      make_table_test_item(make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64"));

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
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(update);

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
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(update);

  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.4");
}

// -----------------------------------------------------------------------------
// Verify that an all-version update row displays its exact selected version.
// -----------------------------------------------------------------------------
TEST_CASE("Package table all-version update row displays selected version")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");

  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");
  update.repo = "updates";
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(update, false);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == update.version);
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == update.release);
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION).empty());
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE).empty());
  REQUIRE(package_table_column_text(item, PackageColumnKind::REPO) == update.repo);
}

// -----------------------------------------------------------------------------
// Verify that an exact installed row in all-version mode shows its update columns.
// -----------------------------------------------------------------------------
TEST_CASE("Package table all-version installed row displays update candidate")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  installed.repo = "@System";
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-1.2.4-1.fc44.x86_64";
  installed.repo_candidate_version = "1.2.4";
  installed.repo_candidate_release = "1.fc44";
  installed.repo_candidate_repo = "updates";
  installed.repo_candidate_is_newest_available = true;

  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");
  update.repo = "updates";
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem installed_item = make_table_test_item(installed, false);
  PackageItem update_item = make_table_test_item(update, false);

  REQUIRE(package_table_column_text(installed_item, PackageColumnKind::VERSION) == installed.version);
  REQUIRE(package_table_column_text(installed_item, PackageColumnKind::RELEASE) == installed.release);
  REQUIRE(package_table_column_text(installed_item, PackageColumnKind::UPDATE_VERSION) == update.version);
  REQUIRE(package_table_column_text(installed_item, PackageColumnKind::UPDATE_RELEASE) == update.release);

  REQUIRE(package_table_column_text(update_item, PackageColumnKind::VERSION) == update.version);
  REQUIRE(package_table_column_text(update_item, PackageColumnKind::RELEASE) == update.release);
  REQUIRE(package_table_column_text(update_item, PackageColumnKind::UPDATE_VERSION).empty());
  REQUIRE(package_table_column_text(update_item, PackageColumnKind::UPDATE_RELEASE).empty());
}

// -----------------------------------------------------------------------------
// Verify that a non upgradable row leaves the Update column empty.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update column is empty for normal rows")
{
  reset_backend_globals();

  PackageItem item =
      make_table_test_item(make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64"));

  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION).empty());
}

// -----------------------------------------------------------------------------
// Verify that an older available version shows the selected package version.
// -----------------------------------------------------------------------------
TEST_CASE("Package table downgradeable row displays selected version")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-2.0-1.fc44.x86_64", "demo", "2.0", "1.fc44", "x86_64");
  PackageRow older = make_table_test_row("demo-1.0-1.fc44.x86_64", "demo", "1.0", "1.fc44", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(older);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == older.version);
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == older.release);
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION).empty());
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE).empty());
}

// -----------------------------------------------------------------------------
// Verify that an intermediate newer version shows the selected package version.
// -----------------------------------------------------------------------------
TEST_CASE("Package table intermediate update row displays selected version")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.0-1.fc44.x86_64", "demo", "1.0", "1.fc44", "x86_64");
  PackageRow intermediate = make_table_test_row("demo-2.0-1.fc44.x86_64", "demo", "2.0", "1.fc44", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(intermediate);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == intermediate.version);
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == intermediate.release);
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION).empty());
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE).empty());
}

// -----------------------------------------------------------------------------
// Verify that the Release column shows the package release.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Release column shows package release")
{
  reset_backend_globals();

  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");

  PackageItem item = make_table_test_item(update);

  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == "1.fc44");
}

// -----------------------------------------------------------------------------
// Verify that exact installed rows display themselves when another installed EVR exists.
// -----------------------------------------------------------------------------
TEST_CASE("Package table exact installed row displays its own version")
{
  reset_backend_globals();

  PackageRow older = make_table_test_row("demo-1.0-1.fc44.x86_64", "demo", "1.0", "1.fc44", "x86_64");
  older.repo = "@System";
  PackageRow newer = make_table_test_row("demo-2.0-1.fc44.x86_64", "demo", "2.0", "1.fc44", "x86_64");
  newer.repo = "@System";

  dnf_backend_testonly_replace_installed_snapshot_rows({ older, newer });

  PackageItem item = make_table_test_item(older);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == older.version);
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == older.release);
  REQUIRE(package_table_column_text(item, PackageColumnKind::REPO) == older.repo);
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
  installed.repo_candidate_is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(installed);

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
  installed.repo_candidate_is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(installed);

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
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(update);

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
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(update);

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

  DaemonUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "1.2.5";
  target.release = "2.fc44";
  target.nevra = "demo-1.2.5-2.fc44.x86_64";
  target.full_nevra = target.nevra;
  target.repo_id = "daemon-repo";

  item.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = target,
  });
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_display_values(item, resolution, true);

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

  DaemonUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "1.2.5";
  target.release = "2.fc44";
  target.nevra = "demo-1.2.5-2.fc44.x86_64";
  target.full_nevra = target.nevra;
  target.repo_id = "daemon-repo";

  item.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = target,
  });
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_display_values(item, resolution, true);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == "1.2.4");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.5");
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == "1.fc44");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE) == "2.fc44");
}

// -----------------------------------------------------------------------------
// Verify that Version sorting uses values stored on the package items.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version sorter uses stored item values")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.0-1.x86_64", "left", "1.0", "1", "x86_64");
  PackageRow right = make_table_test_row("right-2.0-1.x86_64", "right", "2.0", "1", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Version sorting follows RPM version rules instead of plain text order.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version sorter uses RPM version order")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.2.9-1.x86_64", "left", "1.2.9", "1", "x86_64");
  PackageRow right = make_table_test_row("right-1.2.10-1.x86_64", "right", "1.2.10", "1", "x86_64");
  left.is_newest_available = true;
  right.is_newest_available = true;

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Version sorting includes the RPM epoch, not only the visible version text.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version sorter uses RPM epoch order")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-99.0-1.x86_64", "left", "99.0", "1", "x86_64", "0");
  PackageRow right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64", "1");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Version sorting follows RPM pre-release ordering.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version sorter handles RPM pre-release marker")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.0~rc1-1.x86_64", "left", "1.0~rc1", "1", "x86_64");
  PackageRow right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Version sorting does not use Release as a hidden sort key.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version sorter ignores Release")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("bravo-1.0-2.x86_64", "bravo", "1.0", "2", "x86_64");
  PackageRow right = make_table_test_row("alpha-1.0-10.x86_64", "alpha", "1.0", "10", "x86_64");
  left.is_newest_available = true;
  right.is_newest_available = true;

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) > 0);
}

// -----------------------------------------------------------------------------
// Verify that Update Version sorting uses values stored on the package items.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Version sorter uses stored item values")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow installed_left = make_table_test_row("left-1.0-1.x86_64", "left", "1.0", "1", "x86_64");
  PackageRow installed_right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_left, installed_right });

  PackageRow left = make_table_test_row("left-1.2.9-1.x86_64", "left", "1.2.9", "1", "x86_64");
  PackageRow right = make_table_test_row("right-1.2.10-1.x86_64", "right", "1.2.10", "1", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::UPDATE_VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Update Version sorting does not use Update Release as a hidden sort key.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Version sorter ignores Update Release")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow installed_left = make_table_test_row("bravo-0.9-1.x86_64", "bravo", "0.9", "1", "x86_64");
  PackageRow installed_right = make_table_test_row("alpha-0.9-1.x86_64", "alpha", "0.9", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_left, installed_right });

  PackageRow left = make_table_test_row("bravo-1.0-2.x86_64", "bravo", "1.0", "2", "x86_64");
  PackageRow right = make_table_test_row("alpha-1.0-10.x86_64", "alpha", "1.0", "10", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::UPDATE_VERSION) > 0);
}

// -----------------------------------------------------------------------------
// Verify that Update Version sorting includes the candidate RPM epoch.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Version sorter uses candidate epoch")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.0-1.x86_64", "left", "1.0", "1", "x86_64");
  left.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  left.repo_candidate_epoch = "0";
  left.repo_candidate_version = "99.0";
  left.repo_candidate_release = "1";
  left.repo_candidate_is_newest_available = true;

  PackageRow right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64");
  right.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  right.repo_candidate_epoch = "1";
  right.repo_candidate_version = "1.0";
  right.repo_candidate_release = "1";
  right.repo_candidate_is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ left, right });

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::UPDATE_VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade rows keep daemon target epochs for Update Version sorting.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Version sorter uses daemon target epoch")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageTableRow left {
    .row = make_table_test_row("left-99.0-1.x86_64", "left", "99.0", "1", "x86_64", "0"),
    .daemon_upgrade = {},
  };
  DaemonUpgradeTarget left_target;
  left_target.name = "left";
  left_target.arch = "x86_64";
  left_target.epoch = "0";
  left_target.version = "99.0";
  left_target.release = "1";
  left_target.nevra = "left-99.0-1.x86_64";
  left.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = left_target,
  });

  PackageTableRow right {
    .row = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64", "1"),
    .daemon_upgrade = {},
  };
  DaemonUpgradeTarget right_target;
  right_target.name = "right";
  right_target.arch = "x86_64";
  right_target.epoch = "1";
  right_target.version = "1.0";
  right_target.release = "1";
  right_target.nevra = "right-1.0-1.x86_64";
  right.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = right_target,
  });

  GObject *left_object = make_package_object(&widgets, left);
  GObject *right_object = make_package_object(&widgets, right);
  int result = package_table_column_sorter_compare(
      left_object, right_object, GINT_TO_POINTER(static_cast<int>(PackageColumnKind::UPDATE_VERSION) + 1));
  g_object_unref(left_object);
  g_object_unref(right_object);

  REQUIRE(result < 0);
}

// -----------------------------------------------------------------------------
// Verify that rows without update values sort before rows with update values.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Version sorter keeps absent values first")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.0-1.x86_64", "left", "1.0", "1", "x86_64");
  PackageRow installed_right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_right });
  PackageRow right = make_table_test_row("right-2.0-1.x86_64", "right", "2.0", "1", "x86_64");
  right.is_newest_available = true;

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::UPDATE_VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Release sorting follows RPM version rules instead of plain text order.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Release sorter uses RPM version order")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.0-2.fc44.x86_64", "left", "1.0", "2.fc44", "x86_64");
  PackageRow right = make_table_test_row("right-1.0-10.fc44.x86_64", "right", "1.0", "10.fc44", "x86_64");
  left.is_newest_available = true;
  right.is_newest_available = true;

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::RELEASE) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Release sorting follows RPM pre-release ordering.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Release sorter handles RPM pre-release marker")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.0-1~beta.x86_64", "left", "1.0", "1~beta", "x86_64");
  PackageRow right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::RELEASE) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Release sorting does not use Version or epoch as hidden sort keys.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Release sorter ignores Version and epoch")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-2.0-1.x86_64", "left", "2.0", "1", "x86_64", "1");
  PackageRow right = make_table_test_row("right-1.0-10.x86_64", "right", "1.0", "10", "x86_64", "0");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::RELEASE) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Update Release sorting follows RPM version rules.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Release sorter uses RPM version order")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow installed_left = make_table_test_row("left-1.0-1.x86_64", "left", "1.0", "1", "x86_64");
  PackageRow installed_right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_left, installed_right });

  PackageRow left = make_table_test_row("left-1.0-2.fc44.x86_64", "left", "1.0", "2.fc44", "x86_64");
  PackageRow right = make_table_test_row("right-1.0-10.fc44.x86_64", "right", "1.0", "10.fc44", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::UPDATE_RELEASE) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Update Release sorting does not use Update Version as a hidden sort key.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Release sorter ignores Update Version")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow installed_left = make_table_test_row("left-0.9-1.x86_64", "left", "0.9", "1", "x86_64");
  PackageRow installed_right = make_table_test_row("right-0.9-1.x86_64", "right", "0.9", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_left, installed_right });

  PackageRow left = make_table_test_row("left-2.0-1.x86_64", "left", "2.0", "1", "x86_64");
  PackageRow right = make_table_test_row("right-1.0-10.x86_64", "right", "1.0", "10", "x86_64");
  left.is_newest_available = true;
  right.is_newest_available = true;

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::UPDATE_RELEASE) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Repository sorting uses values stored on the package items.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Repo sorter uses stored item values")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.0-1.x86_64", "left", "1.0", "1", "x86_64");
  left.repo = "fedora";
  PackageRow right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64");
  right.repo = "updates";

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::REPO) < 0);
}

// -----------------------------------------------------------------------------
// Verify that equal column values fall back to package name and then NEVRA.
// -----------------------------------------------------------------------------
TEST_CASE("Package table sorter falls back to name and NEVRA")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow right = make_table_test_row("other-1.0-1.x86_64", "other", "1.0", "1", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) < 0);

  right.name = left.name;
  right.nevra = "demo-1.0-2.x86_64";
  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that RPM-equal version text still uses the normal fallback order.
// -----------------------------------------------------------------------------
TEST_CASE("Package table sorter falls back after RPM-equal Version values")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("zulu-1.02-1.x86_64", "zulu", "1.02", "1", "x86_64");
  PackageRow right = make_table_test_row("alpha-1.2-1.x86_64", "alpha", "1.2", "1", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) > 0);
}

// -----------------------------------------------------------------------------
// Verify that an exact installed row sorts by its own EVR.
// -----------------------------------------------------------------------------
TEST_CASE("Package table sorter uses exact installed row EVR")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow older = make_table_test_row("demo-99.0-1.x86_64", "demo", "99.0", "1", "x86_64", "0");
  older.repo = "@System";
  PackageRow newer = make_table_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64", "1");
  newer.repo = "@System";
  PackageRow comparison = make_table_test_row("other-1.0-1.x86_64", "other", "1.0", "1", "x86_64", "1");

  dnf_backend_testonly_replace_installed_snapshot_rows({ older, newer });

  REQUIRE(compare_table_test_rows(widgets, older, comparison, PackageColumnKind::VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that replacing installed state does not change existing item comparisons.
// -----------------------------------------------------------------------------
TEST_CASE("Package table sorter keeps existing item values after installed snapshot changes")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  widgets.query_state.displayed_query.kind = DisplayedPackageQueryKind::LIST_PACKAGES;
  widgets.query_state.displayed_query.latest_only = true;

  PackageRow installed = make_table_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageRow update = make_table_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;
  PackageRow comparison = make_table_test_row("other-1.2-1.x86_64", "other", "1.2", "1", "x86_64");

  GObject *update_object = make_package_object(&widgets, update);
  GObject *comparison_object = make_package_object(&widgets, comparison);

  installed.version = "1.3";
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  int result = package_table_column_sorter_compare(
      update_object, comparison_object, GINT_TO_POINTER(static_cast<int>(PackageColumnKind::VERSION) + 1));

  g_object_unref(update_object);
  g_object_unref(comparison_object);

  REQUIRE(result < 0);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
