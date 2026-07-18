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
#include "ui/package_query/package_query_controller_internal.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/transaction/pending_transaction_apply.hpp"
#include "ui/transaction/pending_transaction_controller.hpp"
#include "ui/transaction/pending_transaction_view.hpp"
#include "ui/common/ui_helpers.hpp"

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
  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
      pkg, selected.upgrade_target ? &selected.upgrade_target.value() : nullptr, selected.upgrade_generation);
  if (!action_rows.has_install_row) {
    ui_helpers_set_status(widgets->query.status_label, _("No install or upgrade action is available."), "gray");
    return;
  }

  bool self_protected =
      action_rows.has_installed_row && dnf_backend_is_package_self_protected(action_rows.installed_row);
  if (pending_transaction_install_action_blocked_by_self_protection(action_rows, self_protected)) {
    ui_helpers_set_status(
        widgets->query.status_label, self_protected_transaction_message(action_rows.installed_row).c_str(), "red");
    return;
  }

  // Add or remove the pending install or upgrade action.
  PendingAction::Type action_type = action_rows.install_is_upgrade ? PendingAction::UPGRADE : PendingAction::INSTALL;
  PendingAction::Type existing_type;
  bool has_existing = pending_transaction_get_action_type(widgets, action_rows.install_row.nevra, existing_type);

  if (has_existing && existing_type == action_type) {
    pending_transaction_remove_action(widgets, action_rows.install_row.nevra);
    pending_transaction_refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, (std::string(_("Unmarked: ")) + pkg.name).c_str(), "gray");
  } else {
    if (action_type == PendingAction::UPGRADE) {
      const TransactionServiceUpgradeTarget *upgrade_target =
          selected.upgrade_target ? &selected.upgrade_target.value() : nullptr;
      bool marked = pending_transaction_mark_upgrade_action_for_row(
          widgets->transaction.actions, pkg, upgrade_target, selected.upgrade_generation);
      if (!marked) {
        package_query_clear_displayed_upgradeable_table(widgets);
        ui_helpers_set_status(
            widgets->query.status_label, _("Upgrade information changed. Press List Upgradable to reload."), "blue");
        return;
      }
    } else {
      // Replace any related pending action with install.
      pending_transaction_remove_action(widgets, pkg.nevra);
      if (action_rows.has_installed_row) {
        pending_transaction_remove_action(widgets, action_rows.installed_row.nevra);
      }
      pending_transaction_remove_action(widgets, action_rows.install_row.nevra);
      widgets->transaction.actions.push_back(
          { action_type, action_rows.install_row.nevra, action_rows.install_row.nevra });
    }
    pending_transaction_refresh_pending_tab(widgets);
    const char *message = action_rows.install_is_upgrade ? _("Marked for upgrade: ") : _("Marked for install: ");
    ui_helpers_set_status(widgets->query.status_label, (std::string(message) + pkg.name).c_str(), "blue");
  }

  const std::string installed_nevra = action_rows.has_installed_row ? action_rows.installed_row.nevra : pkg.nevra;
  ui_helpers_update_action_button_labels_for_selection(
      widgets, action_rows.install_row.nevra, installed_nevra, installed_nevra, action_rows.install_is_upgrade);
  pending_transaction_invalidate_service_preview(widgets);

  // Refresh status badges without rebuilding the package table.
  package_table_refresh_statuses(widgets);
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

  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
      pkg, selected.upgrade_target ? &selected.upgrade_target.value() : nullptr, selected.upgrade_generation);

  // Removal checks the installed row.
  // Upgrade candidates use the currently installed NEVRA for removal.
  if (!action_rows.has_installed_row) {
    ui_helpers_set_status(widgets->query.status_label, _("Package is not installed."), "gray");
    return;
  }

  if (dnf_backend_is_package_self_protected(action_rows.installed_row)) {
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
    // Replace any other pending action with remove.
    pending_transaction_remove_action(widgets, pkg.nevra);
    if (action_rows.has_install_row) {
      pending_transaction_remove_action(widgets, action_rows.install_row.nevra);
    }
    pending_transaction_remove_action(widgets, action_rows.installed_row.nevra);
    widgets->transaction.actions.push_back(
        { PendingAction::REMOVE, action_rows.installed_row.nevra, action_rows.installed_row.nevra });
    pending_transaction_refresh_pending_tab(widgets);
    ui_helpers_set_status(
        widgets->query.status_label, (std::string(_("Marked for removal: ")) + pkg.name).c_str(), "blue");
  }

  const std::string install_nevra = action_rows.has_install_row ? action_rows.install_row.nevra : pkg.nevra;
  ui_helpers_update_action_button_labels_for_selection(widgets,
                                                       install_nevra,
                                                       action_rows.installed_row.nevra,
                                                       action_rows.installed_row.nevra,
                                                       action_rows.install_is_upgrade);
  pending_transaction_invalidate_service_preview(widgets);

  // Refresh status badges without rebuilding the package table.
  package_table_refresh_statuses(widgets);
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

  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
      pkg, selected.upgrade_target ? &selected.upgrade_target.value() : nullptr, selected.upgrade_generation);

  // Reinstall must check the installed package, not the visible update row.
  if (!action_rows.has_installed_row) {
    ui_helpers_set_status(widgets->query.status_label, _("Package is not installed."), "gray");
    return;
  }

  if (dnf_backend_is_package_self_protected(action_rows.installed_row)) {
    ui_helpers_set_status(
        widgets->query.status_label, self_protected_transaction_message(action_rows.installed_row).c_str(), "red");
    return;
  }

  if (!action_rows.can_try_reinstall) {
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
    pending_transaction_remove_action(widgets, pkg.nevra);
    if (action_rows.has_install_row) {
      pending_transaction_remove_action(widgets, action_rows.install_row.nevra);
    }
    pending_transaction_remove_action(widgets, action_rows.installed_row.nevra);
    widgets->transaction.actions.push_back(
        { PendingAction::REINSTALL, action_rows.installed_row.nevra, action_rows.installed_row.nevra });
    pending_transaction_refresh_pending_tab(widgets);
    ui_helpers_set_status(
        widgets->query.status_label, (std::string(_("Marked for reinstall: ")) + pkg.name).c_str(), "blue");
  }

  const std::string install_nevra = action_rows.has_install_row ? action_rows.install_row.nevra : pkg.nevra;
  ui_helpers_update_action_button_labels_for_selection(widgets,
                                                       install_nevra,
                                                       action_rows.installed_row.nevra,
                                                       action_rows.installed_row.nevra,
                                                       action_rows.install_is_upgrade);
  pending_transaction_invalidate_service_preview(widgets);

  package_table_refresh_statuses(widgets);
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

  std::vector<PackageTableRow> rows = package_table_get_displayed_packages(widgets);
  size_t marked_count = 0;
  for (const auto &row : rows) {
    if (pending_transaction_mark_upgrade_action_for_row(widgets->transaction.actions,
                                                        row.row,
                                                        row.upgrade_target ? &row.upgrade_target.value() : nullptr,
                                                        row.upgrade_generation)) {
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

  if (widgets->transaction.actions.empty()) {
    ui_helpers_set_status(widgets->query.status_label, _("No pending actions to clear."), "blue");
    return;
  }

  size_t count = widgets->transaction.actions.size();
  widgets->transaction.actions.clear();
  pending_transaction_invalidate_service_preview(widgets);
  pending_transaction_refresh_pending_tab(widgets);

  // Refresh status badges without rebuilding the package table.
  package_table_refresh_statuses(widgets);

  std::string msg = dnfui_i18n_format_count(count, "Cleared %zu pending action.", "Cleared %zu pending actions.");
  ui_helpers_set_status(widgets->query.status_label, msg, "green");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
