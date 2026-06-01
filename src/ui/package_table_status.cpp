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
#include <vector>

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

  return tooltip;
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
// Return true when one package ID describes the current row.
// -----------------------------------------------------------------------------
static bool
package_id_matches_row(const std::string &package_nevra, const std::string &nevra, const std::string &alternate_nevra)
{
  return package_nevra == nevra || (!alternate_nevra.empty() && package_nevra == alternate_nevra);
}

// -----------------------------------------------------------------------------
// Return true when one resolved preview section contains the current row.
// -----------------------------------------------------------------------------
static bool
preview_section_matches_row(const std::vector<std::string> &items,
                            const std::string &nevra,
                            const std::string &alternate_nevra)
{
  for (const auto &item : items) {
    if (package_id_matches_row(item, nevra, alternate_nevra)) {
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
// Return the text shown for a transaction effect in the Status column.
// -----------------------------------------------------------------------------
static const char *
status_effect_text(PackageTableStatusEffect effect, PackageInstallState install_state)
{
  switch (effect) {
  case PackageTableStatusEffect::PENDING_INSTALL:
    return install_state == PackageInstallState::UPGRADEABLE ? _("Pending Upgrade") : _("Pending Install");
  case PackageTableStatusEffect::PENDING_REINSTALL:
    return _("Pending Reinstall");
  case PackageTableStatusEffect::PENDING_REMOVE:
    return _("Pending Removal");
  case PackageTableStatusEffect::PREVIEW_INSTALL:
    return _("Required Install");
  case PackageTableStatusEffect::PREVIEW_UPGRADE:
    return _("Required Upgrade");
  case PackageTableStatusEffect::PREVIEW_DOWNGRADE:
    return _("Required Downgrade");
  case PackageTableStatusEffect::PREVIEW_REINSTALL:
    return _("Required Reinstall");
  case PackageTableStatusEffect::PREVIEW_REMOVE:
    return _("Required Removal");
  case PackageTableStatusEffect::NONE:
  default:
    return package_table_status_text(install_state);
  }
}

// -----------------------------------------------------------------------------
// Return the CSS class for one Status column transaction effect.
// -----------------------------------------------------------------------------
static const char *
status_effect_css_class(PackageTableStatusEffect effect)
{
  switch (effect) {
  case PackageTableStatusEffect::PENDING_INSTALL:
    return "package-status-pending-install";
  case PackageTableStatusEffect::PENDING_REINSTALL:
    return "package-status-pending-reinstall";
  case PackageTableStatusEffect::PENDING_REMOVE:
    return "package-status-pending-remove";
  case PackageTableStatusEffect::PREVIEW_INSTALL:
  case PackageTableStatusEffect::PREVIEW_UPGRADE:
  case PackageTableStatusEffect::PREVIEW_DOWNGRADE:
  case PackageTableStatusEffect::PREVIEW_REINSTALL:
  case PackageTableStatusEffect::PREVIEW_REMOVE:
    return "package-status-required-change";
  case PackageTableStatusEffect::NONE:
  default:
    return nullptr;
  }
}

// -----------------------------------------------------------------------------
// Return true when the effect came from a prepared preview rather than a marked action.
// -----------------------------------------------------------------------------
static bool
status_effect_is_preview(PackageTableStatusEffect effect)
{
  switch (effect) {
  case PackageTableStatusEffect::PREVIEW_INSTALL:
  case PackageTableStatusEffect::PREVIEW_UPGRADE:
  case PackageTableStatusEffect::PREVIEW_DOWNGRADE:
  case PackageTableStatusEffect::PREVIEW_REINSTALL:
  case PackageTableStatusEffect::PREVIEW_REMOVE:
    return true;
  case PackageTableStatusEffect::NONE:
  case PackageTableStatusEffect::PENDING_INSTALL:
  case PackageTableStatusEffect::PENDING_REINSTALL:
  case PackageTableStatusEffect::PENDING_REMOVE:
  default:
    return false;
  }
}

// -----------------------------------------------------------------------------
// Return the current transaction effect for one package row.
// -----------------------------------------------------------------------------
PackageTableStatusEffect
package_table_status_effect_for_row(const PendingTransactionWidgets &transaction,
                                    const std::string &nevra,
                                    const std::string &alternate_nevra)
{
  for (const auto &a : transaction.actions) {
    if (package_id_matches_row(a.nevra, nevra, alternate_nevra)) {
      switch (a.type) {
      case PendingAction::INSTALL:
      case PendingAction::UPGRADE:
        return PackageTableStatusEffect::PENDING_INSTALL;
      case PendingAction::REINSTALL:
        return PackageTableStatusEffect::PENDING_REINSTALL;
      case PendingAction::REMOVE:
        return PackageTableStatusEffect::PENDING_REMOVE;
      }
    }
  }

  if (!transaction.show_required_package_changes) {
    return PackageTableStatusEffect::NONE;
  }

  if (preview_section_matches_row(transaction.prepared_preview.install, nevra, alternate_nevra)) {
    return PackageTableStatusEffect::PREVIEW_INSTALL;
  }
  if (preview_section_matches_row(transaction.prepared_preview.upgrade, nevra, alternate_nevra)) {
    return PackageTableStatusEffect::PREVIEW_UPGRADE;
  }
  if (preview_section_matches_row(transaction.prepared_preview.downgrade, nevra, alternate_nevra)) {
    return PackageTableStatusEffect::PREVIEW_DOWNGRADE;
  }
  if (preview_section_matches_row(transaction.prepared_preview.reinstall, nevra, alternate_nevra)) {
    return PackageTableStatusEffect::PREVIEW_REINSTALL;
  }
  if (preview_section_matches_row(transaction.prepared_preview.remove, nevra, alternate_nevra)) {
    return PackageTableStatusEffect::PREVIEW_REMOVE;
  }

  return PackageTableStatusEffect::NONE;
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
// Return the icon name for one Status column transaction effect.
// -----------------------------------------------------------------------------
static const char *
status_effect_icon_name(PackageTableStatusEffect effect, PackageInstallState install_state)
{
  switch (effect) {
  case PackageTableStatusEffect::PENDING_INSTALL:
    return install_state == PackageInstallState::UPGRADEABLE ? "view-refresh-symbolic" : "list-add-symbolic";
  case PackageTableStatusEffect::PREVIEW_INSTALL:
    return "list-add-symbolic";
  case PackageTableStatusEffect::PENDING_REINSTALL:
  case PackageTableStatusEffect::PREVIEW_UPGRADE:
  case PackageTableStatusEffect::PREVIEW_DOWNGRADE:
  case PackageTableStatusEffect::PREVIEW_REINSTALL:
    return "view-refresh-symbolic";
  case PackageTableStatusEffect::PENDING_REMOVE:
  case PackageTableStatusEffect::PREVIEW_REMOVE:
    return "list-remove-symbolic";
  case PackageTableStatusEffect::NONE:
  default:
    return status_icon_name(install_state);
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
  gtk_widget_remove_css_class(cell, "package-status-pending-install");
  gtk_widget_remove_css_class(cell, "package-status-pending-reinstall");
  gtk_widget_remove_css_class(cell, "package-status-pending-remove");
  gtk_widget_remove_css_class(cell, "package-status-required-change");

  if (GtkWidget *icon = status_cell_icon(cell)) {
    gtk_widget_set_visible(icon, FALSE);
  }
}

// -----------------------------------------------------------------------------
// Apply text, CSS, and tooltip for one Status cell.
// -----------------------------------------------------------------------------
void
package_table_update_status_label(GtkWidget *cell, SearchWidgets *widgets, const PackageRow &row)
{
  PackageInstallState install_state = dnf_backend_get_package_install_state(row);
  PackageActionRows action_rows;
  if (install_state == PackageInstallState::UPGRADEABLE) {
    action_rows = package_action_rows_for_selection(row);
  }

  package_table_clear_status_css(cell);
  std::string alternate_nevra;

  // Upgradable rows can show one package ID while the pending action uses a matching installed or available package ID.
  if (action_rows.has_install_row && action_rows.install_row.nevra != row.nevra) {
    alternate_nevra = action_rows.install_row.nevra;
  } else if (action_rows.has_installed_row && action_rows.installed_row.nevra != row.nevra) {
    alternate_nevra = action_rows.installed_row.nevra;
  }

  PackageTableStatusEffect effect =
      package_table_status_effect_for_row(widgets->transaction, row.nevra, alternate_nevra);
  GtkWidget *label = status_cell_label(cell);
  gtk_label_set_text(GTK_LABEL(label), status_effect_text(effect, install_state));

  if (GtkWidget *icon = status_cell_icon(cell)) {
    const char *icon_name = status_effect_icon_name(effect, install_state);
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name);
    gtk_widget_set_visible(icon, icon_name != nullptr);
  }

  if (const char *effect_class = status_effect_css_class(effect)) {
    gtk_widget_add_css_class(cell, effect_class);
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

  std::string tooltip = package_table_status_tooltip_text(row);
  if (status_effect_is_preview(effect)) {
    if (!tooltip.empty()) {
      tooltip += "\n";
    }
    tooltip += _("This package is required by the current pending changes.");
  }
  gtk_widget_set_tooltip_text(cell, tooltip.empty() ? nullptr : tooltip.c_str());
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
