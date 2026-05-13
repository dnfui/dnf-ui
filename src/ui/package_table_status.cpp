// -----------------------------------------------------------------------------
// src/ui/package_table_status.cpp
// Package table status rendering helpers
// Keeps status text, tooltip text, sort priority, and CSS class handling
// separate from the broader package table construction code.
// -----------------------------------------------------------------------------
#include "package_table_status.hpp"

#include "i18n.hpp"
#include "package_action_rows.hpp"
#include "widgets.hpp"

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
    return _("Update available");
  case PackageInstallState::AVAILABLE:
  default:
    return _("Available");
  }
}

// -----------------------------------------------------------------------------
// Return the table sort priority used by the Status column.
// -----------------------------------------------------------------------------
int
package_table_status_rank(PackageInstallState state)
{
  return dnf_backend_get_install_state_sort_rank(state);
}

// -----------------------------------------------------------------------------
// Build the hover text for one Status cell.
// -----------------------------------------------------------------------------
static std::string
package_table_status_tooltip_text(const PackageRow &row)
{
  PackageInstallState state = dnf_backend_get_package_install_state(row);
  if (state == PackageInstallState::AVAILABLE) {
    return {};
  }

  std::string tooltip = package_table_status_text(state);
  if (row.install_reason != PackageInstallReason::UNKNOWN) {
    tooltip += "\n";
    tooltip += _("Install reason: ");
    tooltip += dnf_backend_install_reason_to_string(row.install_reason);
  }

  if (row.upgrade_blocked) {
    tooltip += "\n";
    tooltip += _("A newer version exists, but the upgrade cannot be resolved right now.");
  }

  return tooltip;
}

// -----------------------------------------------------------------------------
// Return the CSS class for a pending action, if the row is currently marked.
// -----------------------------------------------------------------------------
static const char *
pending_css_class(SearchWidgets *widgets, const std::string &nevra, const std::string &alternate_nevra)
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
// Remove all Status-column CSS classes before applying the current one.
// -----------------------------------------------------------------------------
void
package_table_clear_status_css(GtkWidget *label)
{
  gtk_widget_remove_css_class(label, "package-status-available");
  gtk_widget_remove_css_class(label, "package-status-installed");
  gtk_widget_remove_css_class(label, "package-status-local-only");
  gtk_widget_remove_css_class(label, "package-status-upgradeable");
  gtk_widget_remove_css_class(label, "package-status-upgrade-blocked");
  gtk_widget_remove_css_class(label, "package-status-installed-newer");
  gtk_widget_remove_css_class(label, "package-status-pending-install");
  gtk_widget_remove_css_class(label, "package-status-pending-reinstall");
  gtk_widget_remove_css_class(label, "package-status-pending-remove");
}

// -----------------------------------------------------------------------------
// Apply text, CSS, and tooltip for one Status cell.
// -----------------------------------------------------------------------------
void
package_table_update_status_label(GtkWidget *label, SearchWidgets *widgets, const PackageRow &row)
{
  PackageInstallState install_state = dnf_backend_get_package_install_state(row);
  PackageActionRows action_rows;
  if (install_state == PackageInstallState::UPGRADEABLE) {
    action_rows = package_action_rows_for_selection(row);
  }

  const char *text = row.upgrade_blocked ? _("Update blocked") : package_table_status_text(install_state);
  for (const auto &a : widgets->transaction.actions) {
    bool action_matches_visible_row = a.nevra == row.nevra;
    bool action_matches_install_row = action_rows.has_install_row && a.nevra == action_rows.install_row.nevra;
    bool action_matches_installed_row = action_rows.has_installed_row && a.nevra == action_rows.installed_row.nevra;
    if (action_matches_visible_row || action_matches_install_row || action_matches_installed_row) {
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
      break;
    }
  }

  gtk_label_set_text(GTK_LABEL(label), text);

  package_table_clear_status_css(label);
  std::string alternate_nevra;

  // Upgradable rows can show one package ID while the pending action uses the
  // matching installed or available package ID.
  if (action_rows.has_install_row && action_rows.install_row.nevra != row.nevra) {
    alternate_nevra = action_rows.install_row.nevra;
  } else if (action_rows.has_installed_row && action_rows.installed_row.nevra != row.nevra) {
    alternate_nevra = action_rows.installed_row.nevra;
  }

  if (const char *pending_class = pending_css_class(widgets, row.nevra, alternate_nevra)) {
    gtk_widget_add_css_class(label, pending_class);
  } else {
    if (install_state == PackageInstallState::LOCAL_ONLY) {
      gtk_widget_add_css_class(label, "package-status-local-only");
    } else if (install_state == PackageInstallState::INSTALLED) {
      gtk_widget_add_css_class(label, "package-status-installed");
    } else if (install_state == PackageInstallState::INSTALLED_NEWER_THAN_REPO) {
      gtk_widget_add_css_class(label, "package-status-installed-newer");
    } else if (row.upgrade_blocked) {
      gtk_widget_add_css_class(label, "package-status-upgrade-blocked");
    } else if (install_state == PackageInstallState::UPGRADEABLE) {
      gtk_widget_add_css_class(label, "package-status-upgradeable");
    } else {
      gtk_widget_add_css_class(label, "package-status-available");
    }
  }

  std::string tooltip = package_table_status_tooltip_text(row);
  gtk_widget_set_tooltip_text(label, tooltip.empty() ? nullptr : tooltip.c_str());
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
