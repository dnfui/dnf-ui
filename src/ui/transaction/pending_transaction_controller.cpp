// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_controller.cpp
// Pending package action button controller
//
// Handles the install, remove, reinstall, and clear buttons.
// Preview and apply handling lives in pending_transaction_apply.cpp
// -----------------------------------------------------------------------------
#include "ui/common/widgets.hpp"

#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "ui/details/package_details_controller.hpp"
#include "ui/package_query/package_query_controller_internal.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/transaction/pending_transaction_apply.hpp"
#include "ui/transaction/pending_transaction_controller.hpp"
#include "ui/transaction/pending_transaction_view.hpp"
#include "ui/common/ui_helpers.hpp"

#include <set>
#include <vector>

// -----------------------------------------------------------------------------
// Explain why the running application package can be viewed but not modified from inside the same process.
// -----------------------------------------------------------------------------
static std::string
self_protected_transaction_message(const PackageRow &pkg)
{
  return dnfui_i18n_format(_("Cannot modify %s while DNF UI is running. Close the application and use another tool."),
                           pkg.name.c_str());
}

// -----------------------------------------------------------------------------
// Return true when pending actions must not be changed.
// -----------------------------------------------------------------------------
static bool
pending_transaction_action_is_busy(MainWindowUiState *widgets)
{
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return true;
  }

  if (pending_transaction_apply_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_apply_busy_message(), "blue");
    return true;
  }

  return false;
}

// -----------------------------------------------------------------------------
// Handle marking the selected package for install.
// -----------------------------------------------------------------------------
void
pending_transaction_on_install_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (pending_transaction_action_is_busy(widgets)) {
    return;
  }

  // Read the selected package from the current package table.
  PackageTableRow selected;
  if (!package_table_get_selected_package(widgets, selected)) {
    ui_helpers_set_status(widgets->query.status_label, _("No package selected."), "gray");
    return;
  }
  PackageRow pkg = selected.row;

  // Resolve the package ID to queue before adding an install or upgrade action.
  PendingTransactionSelectionActions actions =
      pending_transaction_actions_for_selection(pkg,
                                                selected.upgrade_target(),
                                                selected.upgrade_generation(),
                                                widgets->query_state.displayed_query,
                                                widgets->transaction_state.actions);
  const PendingTransactionActionRows &action_rows = actions.rows;
  if (!actions.has_install_action) {
    ui_helpers_set_status(
        widgets->query.status_label, _("No install, upgrade, or downgrade action is available."), "gray");
    return;
  }

  if (actions.install_blocked_by_self_protection) {
    ui_helpers_set_status(
        widgets->query.status_label, self_protected_transaction_message(action_rows.installed_row).c_str(), "red");
    return;
  }

  // Add or remove the pending install, upgrade, or downgrade action.
  PendingAction::Type action_type = PendingAction::INSTALL;
  if (action_rows.install_is_upgrade) {
    action_type = PendingAction::UPGRADE;
  } else if (action_rows.install_is_downgrade) {
    action_type = PendingAction::DOWNGRADE;
  }
  PendingAction::Type existing_type;
  bool has_existing = pending_transaction_get_action_type(widgets, action_rows.install_row.nevra, existing_type);

  if (has_existing && existing_type == action_type) {
    pending_transaction_remove_action(widgets, action_rows.install_row.nevra);
    pending_transaction_refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, (std::string(_("Unmarked: ")) + pkg.name).c_str(), "gray");
  } else {
    bool marked = pending_transaction_mark_install_side_action(widgets->transaction_state.actions, action_rows);
    if (!marked) {
      package_query_clear_displayed_upgradeable_table(widgets);
      ui_helpers_set_status(
          widgets->query.status_label, _("Upgrade information changed. Press List Upgradable to reload."), "blue");
      return;
    }
    pending_transaction_refresh_pending_tab(widgets);
    const char *message = _("Marked for install: ");
    if (action_rows.install_is_upgrade) {
      message = _("Marked for upgrade: ");
    } else if (action_rows.install_is_downgrade) {
      message = _("Marked for downgrade: ");
    }
    ui_helpers_set_status(widgets->query.status_label, (std::string(message) + pkg.name).c_str(), "blue");
  }

  pending_transaction_update_action_button_labels_for_selection(widgets,
                                                                actions.install_action_nevra,
                                                                actions.installed_action_nevra,
                                                                actions.installed_action_nevra,
                                                                action_rows.install_is_upgrade,
                                                                action_rows.install_is_downgrade);
  pending_transaction_invalidate_service_preview(widgets);

  // Refresh status badges without rebuilding the package table.
  package_table_refresh_statuses(widgets);
  package_details_refresh_selected_package_actions(widgets);
}

// -----------------------------------------------------------------------------
// Handle marking the selected package for removal.
// -----------------------------------------------------------------------------
void
pending_transaction_on_remove_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (pending_transaction_action_is_busy(widgets)) {
    return;
  }

  // Read the selected package from the current package table.
  PackageTableRow selected;
  if (!package_table_get_selected_package(widgets, selected)) {
    ui_helpers_set_status(widgets->query.status_label, _("No package selected."), "gray");
    return;
  }
  PackageRow pkg = selected.row;

  PendingTransactionSelectionActions actions =
      pending_transaction_actions_for_selection(pkg,
                                                selected.upgrade_target(),
                                                selected.upgrade_generation(),
                                                widgets->query_state.displayed_query,
                                                widgets->transaction_state.actions);
  const PendingTransactionActionRows &action_rows = actions.rows;

  if (!actions.has_installed_action) {
    ui_helpers_set_status(widgets->query.status_label, _("Package is not installed."), "gray");
    return;
  }

  if (actions.installed_action_blocked_by_self_protection) {
    ui_helpers_set_status(
        widgets->query.status_label, self_protected_transaction_message(action_rows.installed_row).c_str(), "red");
    return;
  }

  // Add or remove the pending remove action.
  PendingAction::Type existing_type;
  bool has_existing = pending_transaction_get_action_type(widgets, action_rows.installed_row.nevra, existing_type);

  if (has_existing && existing_type == PendingAction::REMOVE) {
    pending_transaction_remove_action(widgets, action_rows.installed_row.nevra);
    pending_transaction_refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, (std::string(_("Unmarked: ")) + pkg.name).c_str(), "gray");
  } else {
    pending_transaction_mark_remove_action(widgets->transaction_state.actions, action_rows);
    pending_transaction_refresh_pending_tab(widgets);
    ui_helpers_set_status(
        widgets->query.status_label, (std::string(_("Marked for removal: ")) + pkg.name).c_str(), "blue");
  }

  pending_transaction_update_action_button_labels_for_selection(widgets,
                                                                actions.install_action_nevra,
                                                                actions.installed_action_nevra,
                                                                actions.installed_action_nevra,
                                                                action_rows.install_is_upgrade,
                                                                action_rows.install_is_downgrade);
  pending_transaction_invalidate_service_preview(widgets);

  // Refresh status badges without rebuilding the package table.
  package_table_refresh_statuses(widgets);
  package_details_refresh_selected_package_actions(widgets);
}

// -----------------------------------------------------------------------------
// Handle marking the selected package for reinstall.
// -----------------------------------------------------------------------------
void
pending_transaction_on_reinstall_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (pending_transaction_action_is_busy(widgets)) {
    return;
  }

  PackageTableRow selected;
  if (!package_table_get_selected_package(widgets, selected)) {
    ui_helpers_set_status(widgets->query.status_label, _("No package selected."), "gray");
    return;
  }
  PackageRow pkg = selected.row;

  PendingTransactionSelectionActions actions =
      pending_transaction_actions_for_selection(pkg,
                                                selected.upgrade_target(),
                                                selected.upgrade_generation(),
                                                widgets->query_state.displayed_query,
                                                widgets->transaction_state.actions);
  const PendingTransactionActionRows &action_rows = actions.rows;

  if (!actions.has_installed_action) {
    ui_helpers_set_status(widgets->query.status_label, _("Package is not installed."), "gray");
    return;
  }

  if (actions.installed_action_blocked_by_self_protection) {
    ui_helpers_set_status(
        widgets->query.status_label, self_protected_transaction_message(action_rows.installed_row).c_str(), "red");
    return;
  }

  if (!action_rows.exact_reinstall_available) {
    ui_helpers_set_status(
        widgets->query.status_label, _("Package cannot be reinstalled from current repositories."), "gray");
    return;
  }

  PendingAction::Type existing_type;
  bool has_existing = pending_transaction_get_action_type(widgets, action_rows.installed_row.nevra, existing_type);

  if (has_existing && existing_type == PendingAction::REINSTALL) {
    pending_transaction_remove_action(widgets, action_rows.installed_row.nevra);
    pending_transaction_refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, (std::string(_("Unmarked: ")) + pkg.name).c_str(), "gray");
  } else {
    pending_transaction_mark_reinstall_action(widgets->transaction_state.actions, action_rows);
    pending_transaction_refresh_pending_tab(widgets);
    ui_helpers_set_status(
        widgets->query.status_label, (std::string(_("Marked for reinstall: ")) + pkg.name).c_str(), "blue");
  }

  pending_transaction_update_action_button_labels_for_selection(widgets,
                                                                actions.install_action_nevra,
                                                                actions.installed_action_nevra,
                                                                actions.installed_action_nevra,
                                                                action_rows.install_is_upgrade,
                                                                action_rows.install_is_downgrade);
  pending_transaction_invalidate_service_preview(widgets);

  package_table_refresh_statuses(widgets);
  package_details_refresh_selected_package_actions(widgets);
}

// -----------------------------------------------------------------------------
// Mark all listed upgrade candidates as pending upgrades.
// -----------------------------------------------------------------------------
void
pending_transaction_on_mark_listed_upgrades_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (pending_transaction_action_is_busy(widgets)) {
    return;
  }
  if (package_query_has_active_package_list_request(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, _("Wait for the current package query to finish."), "blue");
    return;
  }
  if (package_table_quick_filter_is_pending(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, _("Wait for package filtering to finish."), "blue");
    return;
  }

  std::vector<PackageTableRow> rows = package_table_get_displayed_packages(widgets);
  const bool projects_upgrade_actions =
      displayed_package_query_projects_upgrade_actions(widgets->query_state.displayed_query);
  const bool exact_installonly_actions =
      displayed_package_query_uses_exact_installonly_actions(widgets->query_state.displayed_query);
  std::set<std::string> marked_package_keys;
  size_t marked_count = 0;
  for (const auto &row : rows) {
    PendingTransactionActionRows action_rows =
        pending_transaction_action_rows_for_selection_with_pending(row.row,
                                                                   row.upgrade_target(),
                                                                   row.upgrade_generation(),
                                                                   exact_installonly_actions,
                                                                   widgets->transaction_state.actions);
    if (pending_transaction_mark_unique_upgrade_action(
            widgets->transaction_state.actions, marked_package_keys, row.row, action_rows, projects_upgrade_actions)) {
      ++marked_count;
    }
  }

  if (marked_count == 0) {
    ui_helpers_set_status(widgets->query.status_label, _("No listed upgrades to mark."), "gray");
    return;
  }

  pending_transaction_invalidate_service_preview(widgets);
  pending_transaction_refresh_pending_tab(widgets);
  package_table_refresh_statuses(widgets);
  package_details_refresh_selected_package_actions(widgets);

  std::string msg = dnfui_i18n_format_count(marked_count, "Marked %zu listed upgrade.", "Marked %zu listed upgrades.");
  ui_helpers_set_status(widgets->query.status_label, msg, "blue");
}

// -----------------------------------------------------------------------------
// Clear all pending package actions without applying them.
// -----------------------------------------------------------------------------
void
pending_transaction_on_clear_pending_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (pending_transaction_action_is_busy(widgets)) {
    return;
  }

  if (widgets->transaction_state.actions.empty()) {
    ui_helpers_set_status(widgets->query.status_label, _("No pending actions to clear."), "blue");
    return;
  }

  size_t count = widgets->transaction_state.actions.size();
  widgets->transaction_state.actions.clear();
  pending_transaction_invalidate_service_preview(widgets);
  pending_transaction_refresh_pending_tab(widgets);

  // Refresh status badges without rebuilding the package table.
  package_table_refresh_statuses(widgets);
  package_details_refresh_selected_package_actions(widgets);

  std::string msg = dnfui_i18n_format_count(count, "Cleared %zu pending action.", "Cleared %zu pending actions.");
  ui_helpers_set_status(widgets->query.status_label, msg, "green");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
