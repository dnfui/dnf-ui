// -----------------------------------------------------------------------------
// package_table_model.cpp
// Package row storage for the GTK package table.
// Wraps PackageRow values in GObjects so the ColumnView can sort and select rows without owning backend data directly.
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_view_internal.hpp"

#include "ui/package_table/package_table_status.hpp"
#include "ui/common/widgets.hpp"

#include <utility>

// -----------------------------------------------------------------------------
// Return the private key used to store package rows on GTK objects.
// -----------------------------------------------------------------------------
static GQuark
package_row_quark()
{
  static GQuark q = 0;
  if (G_UNLIKELY(q == 0)) {
    q = g_quark_from_static_string("package-table-row");
  }

  return q;
}

// -----------------------------------------------------------------------------
// Return the EVR value used for version-like table sorting.
// -----------------------------------------------------------------------------
static PackageTableSortEvr
sort_evr_from_values(const std::string &epoch, const std::string &version, const std::string &release)
{
  if (version.empty() && release.empty()) {
    return {};
  }

  return PackageTableSortEvr {
    .present = true,
    .epoch = epoch,
    .version = version,
    .release = release,
  };
}

// -----------------------------------------------------------------------------
// Return the EVR value used for sorting one package row.
// -----------------------------------------------------------------------------
static PackageTableSortEvr
sort_evr_from_row(const PackageRow &row)
{
  return sort_evr_from_values(row.epoch, row.version, row.release);
}

// -----------------------------------------------------------------------------
// Return the EVR value used for sorting one daemon upgrade target.
// -----------------------------------------------------------------------------
static PackageTableSortEvr
sort_evr_from_daemon_target(const DaemonUpgradeTarget &target)
{
  return sort_evr_from_values(target.epoch, target.version, target.release);
}

// -----------------------------------------------------------------------------
// Return true when the current table uses compact package rows.
// -----------------------------------------------------------------------------
static bool
package_table_uses_compact_rows(const MainWindowUiState *widgets)
{
  return widgets && displayed_package_query_uses_compact_rows(widgets->query_state.displayed_query);
}

// -----------------------------------------------------------------------------
// Return true when the current table shows exact available repository rows.
// -----------------------------------------------------------------------------
static bool
package_table_uses_exact_available_rows(const MainWindowUiState *widgets)
{
  return widgets && displayed_package_query_uses_exact_available_rows(widgets->query_state.displayed_query);
}

// -----------------------------------------------------------------------------
// Snapshot the visible status text and its sort order for one package row.
// -----------------------------------------------------------------------------
void
package_table_fill_item_status(MainWindowUiState *widgets,
                               PackageItem &item,
                               const InstalledPackageResolution &resolution)
{
  // Keep Status sorting tied to the stable package state so marking a pending
  // action does not move the row away from the user in the current view.
  PackageInstallState install_state = item.upgrade_target() ? PackageInstallState::UPGRADEABLE : resolution.state;
  item.status_rank = dnf_backend_get_install_state_sort_rank(install_state);

  PackageTableRow table_row {
    .row = item.row,
    .daemon_upgrade = item.daemon_upgrade,
  };
  const bool exact_available_rows = package_table_uses_exact_available_rows(widgets);
  const bool exact_installonly_actions =
      widgets && displayed_package_query_uses_exact_installonly_actions(widgets->query_state.displayed_query);
  const std::vector<PendingAction> empty_actions;
  const std::vector<PendingAction> &actions = widgets ? widgets->transaction_state.actions : empty_actions;
  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_resolved_selection_with_pending(
      item.row, item.upgrade_target(), table_row.upgrade_generation(), resolution, exact_installonly_actions, actions);
  PendingAction::Type action_type;
  if (package_table_pending_action_for_resolved_row(widgets, table_row, action_rows, action_type)) {
    item.status_text = package_table_pending_action_status_text(action_type, install_state);
    return;
  }

  item.status_text = package_table_status_text_for_resolved_row(table_row, action_rows, exact_available_rows);
}

// -----------------------------------------------------------------------------
// Snapshot display values that depend on installed-package state.
// -----------------------------------------------------------------------------
void
package_table_fill_item_display_values(PackageItem &item,
                                       const InstalledPackageResolution &resolution,
                                       bool compact_view)
{
  PackageTableDisplayValues values {
    .version = item.row.version,
    .update_version = {},
    .install_size = item.row.install_size,
    .release = item.row.release,
    .update_release = {},
    .repo = item.row.repo,
  };
  item.current_evr = sort_evr_from_row(item.row);
  item.update_evr = {};

  if (const DaemonUpgradeTarget *upgrade_target = item.upgrade_target()) {
    if (resolution.has_installed_row) {
      values.version = resolution.installed_row.version;
      values.release = resolution.installed_row.release;
      values.install_size = resolution.installed_row.install_size;
      item.current_evr = sort_evr_from_row(resolution.installed_row);
    } else {
      values.version.clear();
      values.release.clear();
      values.install_size.reset();
      item.current_evr = {};
    }

    values.update_version = upgrade_target->version;
    values.update_release = upgrade_target->release;
    values.repo = upgrade_target->repo_id;
    item.update_evr = sort_evr_from_daemon_target(*upgrade_target);
    item.display_values = std::make_shared<PackageTableDisplayValues>(std::move(values));
    return;
  }

  const bool newest_actionable_upgrade =
      resolution.state == PackageInstallState::UPGRADEABLE && item.row.is_newest_available;

  const bool project_available_upgrade =
      compact_view && !resolution.exact_installed && resolution.has_installed_row && newest_actionable_upgrade;

  if (project_available_upgrade) {
    // The table column is named Version, so keep it aligned with the Details tab Version field.
    values.version = resolution.installed_row.version;
    values.release = resolution.installed_row.release;
    values.install_size = resolution.installed_row.install_size;
    values.repo = resolution.installed_row.repo;
    item.current_evr = sort_evr_from_row(resolution.installed_row);
  }

  if (resolution.state == PackageInstallState::UPGRADEABLE &&
      (resolution.exact_installed || project_available_upgrade)) {
    values.update_version =
        item.row.repo_candidate_version.empty() ? item.row.version : item.row.repo_candidate_version;
    values.update_release =
        item.row.repo_candidate_release.empty() ? item.row.release : item.row.repo_candidate_release;

    if (!item.row.repo_candidate_repo.empty()) {
      values.repo = item.row.repo_candidate_repo;
    } else {
      values.repo = item.row.repo;
    }

    if (item.row.repo_candidate_version.empty() && item.row.repo_candidate_release.empty()) {
      item.update_evr = sort_evr_from_row(item.row);
    } else {
      item.update_evr = sort_evr_from_values(
          item.row.repo_candidate_epoch, item.row.repo_candidate_version, item.row.repo_candidate_release);
    }
  }

  const bool display_values_differ = values.version != item.row.version || !values.update_version.empty() ||
      values.install_size != item.row.install_size || values.release != item.row.release ||
      !values.update_release.empty() || values.repo != item.row.repo;
  if (display_values_differ) {
    item.display_values = std::make_shared<PackageTableDisplayValues>(std::move(values));
  } else {
    item.display_values.reset();
  }
}

// -----------------------------------------------------------------------------
// Wrap one package row in a GObject so GTK list models can sort and select it.
// -----------------------------------------------------------------------------
GObject *
make_package_object(MainWindowUiState *widgets, const PackageRow &row)
{
  GObject *obj = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
  auto *item = new PackageItem;
  item->row = row;
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item->row);
  package_table_fill_item_display_values(*item, resolution, package_table_uses_compact_rows(widgets));
  package_table_fill_item_status(widgets, *item, resolution);
  g_object_set_qdata_full(obj, package_row_quark(), item, +[](gpointer p) { delete static_cast<PackageItem *>(p); });

  return obj;
}

// -----------------------------------------------------------------------------
// Wrap one package row in a GObject so the ColumnView can sort and select rows without owning backend data directly.
// -----------------------------------------------------------------------------
GObject *
make_package_object(MainWindowUiState *widgets, const PackageTableRow &row)
{
  GObject *obj = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
  auto *item = new PackageItem;
  item->row = row.row;
  item->daemon_upgrade = row.daemon_upgrade;
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item->row);
  package_table_fill_item_display_values(*item, resolution, package_table_uses_compact_rows(widgets));
  package_table_fill_item_status(widgets, *item, resolution);
  g_object_set_qdata_full(obj, package_row_quark(), item, +[](gpointer p) { delete static_cast<PackageItem *>(p); });

  return obj;
}

// -----------------------------------------------------------------------------
// Match package names and summaries against case-folded filter text.
// -----------------------------------------------------------------------------
bool
package_table_quick_filter_matches(const PackageRow &row, const char *folded_text)
{
  if (!folded_text || !*folded_text) {
    return true;
  }

  gchar *name = g_utf8_casefold(row.name.c_str(), -1);
  bool matches = g_strstr_len(name, -1, folded_text) != nullptr;
  g_free(name);

  if (!matches) {
    gchar *summary = g_utf8_casefold(row.summary.c_str(), -1);
    matches = g_strstr_len(summary, -1, folded_text) != nullptr;
    g_free(summary);
  }

  return matches;
}

// -----------------------------------------------------------------------------
// Read the sortable package wrapper stored on a GTK list item.
// -----------------------------------------------------------------------------
const PackageItem *
package_item_from_object(GObject *obj)
{
  if (!obj) {
    return nullptr;
  }

  return static_cast<const PackageItem *>(g_object_get_qdata(obj, package_row_quark()));
}

// -----------------------------------------------------------------------------
// Read the mutable package wrapper stored on a GTK list item.
// -----------------------------------------------------------------------------
PackageItem *
mutable_package_item_from_object(GObject *obj)
{
  if (!obj) {
    return nullptr;
  }

  return static_cast<PackageItem *>(g_object_get_qdata(obj, package_row_quark()));
}

// -----------------------------------------------------------------------------
// Map a package wrapper back to the package row used elsewhere in the UI.
// -----------------------------------------------------------------------------
const PackageRow *
package_row_from_object(GObject *obj)
{
  const PackageItem *item = package_item_from_object(obj);
  if (!item) {
    return nullptr;
  }

  return &item->row;
}

// -----------------------------------------------------------------------------
// Convert the stored package wrapper back to the public table row value.
// -----------------------------------------------------------------------------
PackageTableRow
package_table_row_from_item(const PackageItem &item)
{
  return {
    .row = item.row,
    .daemon_upgrade = item.daemon_upgrade,
  };
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
