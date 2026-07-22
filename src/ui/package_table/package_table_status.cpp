// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_status.cpp
// Package table status rendering helpers
// Keeps status text, sort priority, and CSS class handling separate from the broader package table construction code.
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_status.hpp"

#include "i18n.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"
#include "ui/common/widgets.hpp"

#include <string>

// -----------------------------------------------------------------------------
// Convert one backend install state into the Status column text.
// -----------------------------------------------------------------------------
const char *
package_table_status_text(PackageInstallState state)
{
  switch (state) {
  case PackageInstallState::INSTALLED:
    return _("Installed");
  case PackageInstallState::LOCAL_ONLY:
    return _("Installed (local only)");
  case PackageInstallState::INSTALLED_NEWER_THAN_REPO:
    return _("Installed (newer than repo)");
  case PackageInstallState::UPGRADEABLE:
    return _("Newer in repository");
  case PackageInstallState::AVAILABLE:
  default:
    return _("Available");
  }
}

// -----------------------------------------------------------------------------
// Return the text label inside a Status cell.
// -----------------------------------------------------------------------------
static GtkWidget *
status_cell_label(GtkWidget *cell)
{
  GtkWidget *label = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(cell), "package-status-label"));

  return label ? label : cell;
}

// -----------------------------------------------------------------------------
// Return the icon inside a Status cell.
// -----------------------------------------------------------------------------
static GtkWidget *
status_cell_icon(GtkWidget *cell)
{
  return static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(cell), "package-status-icon"));
}

// -----------------------------------------------------------------------------
// Return the CSS class for a pending action NEVRA.
// -----------------------------------------------------------------------------
static const char *
pending_css_class(MainWindowUiState *widgets, const std::string &nevra, const std::string &alternate_nevra)
{
  for (const auto &a : widgets->transaction.actions) {
    if (a.nevra == nevra || (!alternate_nevra.empty() && a.nevra == alternate_nevra)) {
      switch (a.type) {
      case PendingAction::INSTALL:
      case PendingAction::UPGRADE:
        return "package-status-pending-install";
      case PendingAction::REINSTALL:
        return "package-status-pending-reinstall";
      case PendingAction::REMOVE:
        return "package-status-pending-remove";
      }
    }
  }

  return nullptr;
}

// -----------------------------------------------------------------------------
// Return the pending action CSS class for one package row.
// -----------------------------------------------------------------------------
const char *
package_table_pending_action_css_class(MainWindowUiState *widgets, const PackageTableRow &row)
{
  const TransactionServiceUpgradeTarget *upgrade_target = row.upgrade_target();
  PackageInstallState install_state =
      upgrade_target ? PackageInstallState::UPGRADEABLE : dnf_backend_get_package_install_state(row.row);
  PendingTransactionActionRows action_rows;
  if (install_state == PackageInstallState::UPGRADEABLE) {
    action_rows = pending_transaction_action_rows_for_selection(row.row, upgrade_target, row.upgrade_generation());
  }

  std::string alternate_nevra;
  if (upgrade_target && upgrade_target->nevra != row.row.nevra) {
    alternate_nevra = upgrade_target->nevra;
  } else if (action_rows.has_install_row && action_rows.install_row.nevra != row.row.nevra) {
    alternate_nevra = action_rows.install_row.nevra;
  } else if (action_rows.has_installed_row && action_rows.installed_row.nevra != row.row.nevra) {
    alternate_nevra = action_rows.installed_row.nevra;
  }

  return pending_css_class(widgets, row.row.nevra, alternate_nevra);
}

// -----------------------------------------------------------------------------
// Return the icon name for a pending action.
// -----------------------------------------------------------------------------
static const char *
pending_icon_name(PendingAction::Type action_type, PackageInstallState install_state)
{
  switch (action_type) {
  case PendingAction::INSTALL:
  case PendingAction::UPGRADE:
    return install_state == PackageInstallState::UPGRADEABLE ? "view-refresh-symbolic" : "list-add-symbolic";
  case PendingAction::REINSTALL:
    return "view-refresh-symbolic";
  case PendingAction::REMOVE:
    return "list-remove-symbolic";
  }

  return nullptr;
}

// -----------------------------------------------------------------------------
// Return the icon name for a package install state.
// -----------------------------------------------------------------------------
static const char *
status_icon_name(PackageInstallState state)
{
  switch (state) {
  case PackageInstallState::INSTALLED:
  case PackageInstallState::LOCAL_ONLY:
  case PackageInstallState::INSTALLED_NEWER_THAN_REPO:
    return "object-select-symbolic";
  case PackageInstallState::UPGRADEABLE:
    return "view-refresh-symbolic";
  case PackageInstallState::AVAILABLE:
  default:
    return "list-add-symbolic";
  }
}

// -----------------------------------------------------------------------------
// Remove all Status-column CSS classes before applying the current one.
// -----------------------------------------------------------------------------
void
package_table_clear_status_css(GtkWidget *cell)
{
  gtk_widget_remove_css_class(cell, "package-status-available");
  gtk_widget_remove_css_class(cell, "package-status-installed");
  gtk_widget_remove_css_class(cell, "package-status-local-only");
  gtk_widget_remove_css_class(cell, "package-status-upgradeable");
  gtk_widget_remove_css_class(cell, "package-status-installed-newer");
  package_table_clear_pending_action_css(cell);

  if (GtkWidget *icon = status_cell_icon(cell)) {
    gtk_widget_set_visible(icon, FALSE);
  }
}

// -----------------------------------------------------------------------------
// Remove pending action CSS classes from one table cell.
// -----------------------------------------------------------------------------
void
package_table_clear_pending_action_css(GtkWidget *cell)
{
  gtk_widget_remove_css_class(cell, "package-status-pending-install");
  gtk_widget_remove_css_class(cell, "package-status-pending-reinstall");
  gtk_widget_remove_css_class(cell, "package-status-pending-remove");
}

// -----------------------------------------------------------------------------
// Apply text and CSS for one Status cell.
// -----------------------------------------------------------------------------
void
package_table_update_status_label(GtkWidget *cell, MainWindowUiState *widgets, const PackageTableRow &row)
{
  const TransactionServiceUpgradeTarget *upgrade_target = row.upgrade_target();
  PackageInstallState install_state =
      upgrade_target ? PackageInstallState::UPGRADEABLE : dnf_backend_get_package_install_state(row.row);
  PendingTransactionActionRows action_rows;
  if (install_state == PackageInstallState::UPGRADEABLE) {
    action_rows = pending_transaction_action_rows_for_selection(row.row, upgrade_target, row.upgrade_generation());
  }

  const char *text = package_table_status_text(install_state);
  const char *icon_name = status_icon_name(install_state);
  for (const auto &a : widgets->transaction.actions) {
    bool action_matches_visible_row = a.nevra == row.row.nevra;
    bool action_matches_target = upgrade_target && a.nevra == upgrade_target->nevra;
    bool action_matches_install_row = action_rows.has_install_row && a.nevra == action_rows.install_row.nevra;
    bool action_matches_installed_row = action_rows.has_installed_row && a.nevra == action_rows.installed_row.nevra;
    if (action_matches_visible_row || action_matches_target || action_matches_install_row ||
        action_matches_installed_row) {
      switch (a.type) {
      case PendingAction::INSTALL:
      case PendingAction::UPGRADE:
        text = install_state == PackageInstallState::UPGRADEABLE ? _("Pending Upgrade") : _("Pending Install");
        break;
      case PendingAction::REINSTALL:
        text = _("Pending Reinstall");
        break;
      case PendingAction::REMOVE:
        text = _("Pending Removal");
        break;
      }
      icon_name = pending_icon_name(a.type, install_state);
      break;
    }
  }

  GtkWidget *label = status_cell_label(cell);
  gtk_label_set_text(GTK_LABEL(label), text);

  package_table_clear_status_css(cell);

  if (GtkWidget *icon = status_cell_icon(cell)) {
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name);
    gtk_widget_set_visible(icon, icon_name != nullptr);
  }

  if (const char *pending_class = package_table_pending_action_css_class(widgets, row)) {
    gtk_widget_add_css_class(cell, pending_class);
  } else {
    if (install_state == PackageInstallState::LOCAL_ONLY) {
      gtk_widget_add_css_class(cell, "package-status-local-only");
    } else if (install_state == PackageInstallState::INSTALLED) {
      gtk_widget_add_css_class(cell, "package-status-installed");
    } else if (install_state == PackageInstallState::INSTALLED_NEWER_THAN_REPO) {
      gtk_widget_add_css_class(cell, "package-status-installed-newer");
    } else if (install_state == PackageInstallState::UPGRADEABLE) {
      gtk_widget_add_css_class(cell, "package-status-upgradeable");
    } else {
      gtk_widget_add_css_class(cell, "package-status-available");
    }
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
