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
  case PackageInstallState::DOWNGRADEABLE:
    return _("Older in repository");
  case PackageInstallState::AVAILABLE:
  default:
    return _("Available");
  }
}

// -----------------------------------------------------------------------------
// Return display text for one package table row after action rows have been resolved.
// -----------------------------------------------------------------------------
const char *
package_table_status_text_for_resolved_row(const PackageTableRow &row,
                                           const PendingTransactionActionRows &action_rows,
                                           bool exact_available_rows)
{
  if (action_rows.state == PackageInstallState::UPGRADEABLE && exact_available_rows) {
    if (action_rows.has_installed_row && row.row.nevra == action_rows.installed_row.nevra) {
      return _("Installed, update available");
    }

    if (action_rows.has_install_row && row.row.nevra == action_rows.install_row.nevra) {
      return _("Available update");
    }
  }

  return package_table_status_text(action_rows.state);
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
// Return true when the current table may show installed-row actions on update rows.
// -----------------------------------------------------------------------------
static bool
displayed_view_projects_installed_actions(const MainWindowUiState *widgets)
{
  return widgets && displayed_package_query_uses_compact_rows(widgets->query_state.displayed_query);
}

// -----------------------------------------------------------------------------
// Return true when the current table may show upgrade actions on installed rows.
// -----------------------------------------------------------------------------
static bool
displayed_view_projects_upgrade_actions(const MainWindowUiState *widgets)
{
  return widgets && displayed_package_query_projects_upgrade_actions(widgets->query_state.displayed_query);
}

// -----------------------------------------------------------------------------
// Return true when the current table shows exact available repository rows.
// -----------------------------------------------------------------------------
static bool
displayed_view_uses_exact_available_rows(const MainWindowUiState *widgets)
{
  return widgets && displayed_package_query_uses_exact_available_rows(widgets->query_state.displayed_query);
}

// -----------------------------------------------------------------------------
// Return true when one pending action matches one package table row.
// -----------------------------------------------------------------------------
static bool
pending_action_matches_row(const MainWindowUiState *widgets,
                           const PendingAction &action,
                           const PackageTableRow &row,
                           const PendingTransactionActionRows &action_rows)
{
  if (action.type == PendingAction::REMOVE || action.type == PendingAction::REINSTALL) {
    if (!action_rows.has_installed_row || action.nevra != action_rows.installed_row.nevra) {
      return false;
    }

    return pending_transaction_selection_allows_installed_action(
        row.row, action_rows, displayed_view_projects_installed_actions(widgets));
  }

  if (action.type == PendingAction::INSTALL || action.type == PendingAction::DOWNGRADE) {
    if (action.nevra == row.row.nevra) {
      return true;
    }

    return action_rows.install_action_from_pending && action_rows.has_install_row &&
        action.nevra == action_rows.install_row.nevra;
  }

  if (action.type != PendingAction::UPGRADE) {
    return false;
  }

  if (!action_rows.install_is_upgrade) {
    return false;
  }

  if (action.nevra == row.row.nevra) {
    return true;
  }

  const DaemonUpgradeTarget *upgrade_target = row.upgrade_target();
  if (upgrade_target && action.nevra == upgrade_target->nevra) {
    return true;
  }

  if (action_rows.has_install_row && action.nevra == action_rows.install_row.nevra) {
    return pending_transaction_selection_allows_install_action(
        row.row, action_rows, displayed_view_projects_upgrade_actions(widgets));
  }

  if (action_rows.has_installed_row && action.nevra == action_rows.installed_row.nevra) {
    return pending_transaction_selection_allows_install_action(
        row.row, action_rows, displayed_view_projects_upgrade_actions(widgets));
  }

  return false;
}

// -----------------------------------------------------------------------------
// Return the pending action for one package table row.
// -----------------------------------------------------------------------------
bool
package_table_pending_action_for_resolved_row(MainWindowUiState *widgets,
                                              const PackageTableRow &row,
                                              const PendingTransactionActionRows &action_rows,
                                              PendingAction::Type &out_type,
                                              bool *resolved_only)
{
  if (!widgets) {
    return false;
  }
  if (resolved_only) {
    *resolved_only = false;
  }
  for (const auto &action : widgets->transaction_state.actions) {
    if (pending_action_matches_row(widgets, action, row, action_rows)) {
      out_type = action.type;
      return true;
    }
  }

  if (!package_table_resolved_action_for_row(widgets, row, out_type)) {
    return false;
  }
  if (resolved_only) {
    *resolved_only = true;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Look up a daemon-selected action without searching explicit marks.
// -----------------------------------------------------------------------------
bool
package_table_resolved_action_for_row(MainWindowUiState *widgets,
                                      const PackageTableRow &row,
                                      PendingAction::Type &out_type)
{
  if (!widgets) {
    return false;
  }
  const auto &state = widgets->transaction_state;
  if (state.resolved_actions_by_nevra.empty() && state.resolved_actions_by_name_arch.empty()) {
    return false;
  }

  auto exact = state.resolved_actions_by_nevra.find(row.row.nevra);
  if (exact == state.resolved_actions_by_nevra.end()) {
    if (const DaemonUpgradeTarget *target = row.upgrade_target()) {
      auto upgrade = state.resolved_actions_by_nevra.find(target->nevra);
      if (upgrade != state.resolved_actions_by_nevra.end() &&
          (upgrade->second == PendingAction::UPGRADE || upgrade->second == PendingAction::DOWNGRADE)) {
        exact = upgrade;
      }
    }
  }
  if (exact != state.resolved_actions_by_nevra.end()) {
    out_type = exact->second;
  } else {
    auto stream = state.resolved_actions_by_name_arch.find(row.row.name_arch_key());
    if (stream == state.resolved_actions_by_name_arch.end()) {
      return false;
    }

    const auto &displayed = widgets->query_state.displayed_query;
    const bool compact = displayed_package_query_uses_compact_rows(displayed);
    const bool installed_view = displayed.kind == DisplayedPackageQueryKind::LIST_INSTALLED ||
        displayed.kind == DisplayedPackageQueryKind::LIST_UNNEEDED;
    const bool installed_upgrade = installed_view && !row.row.installonly &&
        (stream->second == PendingAction::UPGRADE || stream->second == PendingAction::DOWNGRADE);
    if (!compact && !installed_upgrade) {
      return false;
    }
    const bool version_specific = stream->second != PendingAction::INSTALL;
    if ((installed_upgrade || (compact && version_specific)) &&
        dnf_backend_installed_name_arch_is_ambiguous(row.row.name_arch_key())) {
      return false;
    }
    out_type = stream->second;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Return display text for one pending package action.
// -----------------------------------------------------------------------------
const char *
package_table_pending_action_status_text(PendingAction::Type action_type, PackageInstallState install_state)
{
  switch (action_type) {
  case PendingAction::INSTALL:
    return _("Pending Install");
  case PendingAction::UPGRADE:
    return _("Pending Upgrade");
  case PendingAction::DOWNGRADE:
    return _("Pending Downgrade");
  case PendingAction::REINSTALL:
    return _("Pending Reinstall");
  case PendingAction::REMOVE:
    return _("Pending Removal");
  }

  return package_table_status_text(install_state);
}

// -----------------------------------------------------------------------------
// Return the Status label for an additional daemon-resolved action.
// -----------------------------------------------------------------------------
const char *
package_table_resolved_action_status_text(PendingAction::Type action_type)
{
  switch (action_type) {
  case PendingAction::INSTALL:
    return _("Additional Install");
  case PendingAction::UPGRADE:
    return _("Additional Upgrade");
  case PendingAction::DOWNGRADE:
    return _("Additional Downgrade");
  case PendingAction::REINSTALL:
    return _("Additional Reinstall");
  case PendingAction::REMOVE:
    return _("Additional Removal");
  }
  return _("Additional Action");
}

// -----------------------------------------------------------------------------
// Return the CSS class for one pending action.
// -----------------------------------------------------------------------------
const char *
package_table_pending_action_css_class_for_type(PendingAction::Type action_type)
{
  switch (action_type) {
  case PendingAction::INSTALL:
  case PendingAction::UPGRADE:
  case PendingAction::DOWNGRADE:
    return "package-status-pending-install";
  case PendingAction::REINSTALL:
    return "package-status-pending-reinstall";
  case PendingAction::REMOVE:
    return "package-status-pending-remove";
  }

  return nullptr;
}

// -----------------------------------------------------------------------------
// Return the icon name for a pending action.
// -----------------------------------------------------------------------------
static const char *
pending_icon_name(PendingAction::Type action_type, PackageInstallState)
{
  switch (action_type) {
  case PendingAction::INSTALL:
    return "list-add-symbolic";
  case PendingAction::UPGRADE:
    return "view-refresh-symbolic";
  case PendingAction::DOWNGRADE:
    return "view-refresh-symbolic";
  case PendingAction::REINSTALL:
    return "view-refresh-symbolic";
  case PendingAction::REMOVE:
    return "user-trash-symbolic";
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
    return "drive-harddisk-system-symbolic";
  case PackageInstallState::UPGRADEABLE:
    return "view-refresh-symbolic";
  case PackageInstallState::DOWNGRADEABLE:
    return "view-refresh-symbolic";
  case PackageInstallState::AVAILABLE:
  default:
    return "list-add-symbolic";
  }
}

// -----------------------------------------------------------------------------
// Return the fallback icon name for a package install state.
// -----------------------------------------------------------------------------
static const char *
status_icon_fallback_name(PackageInstallState state)
{
  switch (state) {
  case PackageInstallState::INSTALLED:
  case PackageInstallState::LOCAL_ONLY:
  case PackageInstallState::INSTALLED_NEWER_THAN_REPO:
    return "object-select-symbolic";
  case PackageInstallState::UPGRADEABLE:
  case PackageInstallState::DOWNGRADEABLE:
  case PackageInstallState::AVAILABLE:
  default:
    return nullptr;
  }
}

// -----------------------------------------------------------------------------
// Set a status icon with a fallback icon name.
// -----------------------------------------------------------------------------
static void
status_image_set_icon(GtkImage *image, const char *icon_name, const char *fallback_icon_name)
{
  if (!image) {
    return;
  }

  if (icon_name && icon_name[0] != '\0' && fallback_icon_name && fallback_icon_name[0] != '\0') {
    GIcon *gicon = g_themed_icon_new(icon_name);
    g_themed_icon_append_name(G_THEMED_ICON(gicon), fallback_icon_name);
    gtk_image_set_from_gicon(image, gicon);
    g_object_unref(gicon);
    return;
  }

  gtk_image_set_from_icon_name(image, icon_name);
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
  gtk_widget_remove_css_class(cell, "package-status-installed-upgradeable");
  gtk_widget_remove_css_class(cell, "package-status-downgradeable");
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
  gtk_widget_remove_css_class(cell, "package-status-additional");
}

// -----------------------------------------------------------------------------
// Apply text and CSS for one Status cell.
// -----------------------------------------------------------------------------
void
package_table_update_resolved_status_label(GtkWidget *cell,
                                           MainWindowUiState *widgets,
                                           const PackageTableRow &row,
                                           const PendingTransactionActionRows &action_rows)
{
  PackageInstallState install_state = action_rows.state;
  const bool exact_available_rows = displayed_view_uses_exact_available_rows(widgets);

  const char *text = package_table_status_text_for_resolved_row(row, action_rows, exact_available_rows);
  const char *icon_name = status_icon_name(install_state);
  PendingAction::Type action_type;
  bool resolved_only = false;
  bool has_pending_action =
      package_table_pending_action_for_resolved_row(widgets, row, action_rows, action_type, &resolved_only);
  if (has_pending_action) {
    text = resolved_only ? package_table_resolved_action_status_text(action_type)
                         : package_table_pending_action_status_text(action_type, install_state);
    icon_name = pending_icon_name(action_type, install_state);
  }

  GtkWidget *label = status_cell_label(cell);
  gtk_label_set_text(GTK_LABEL(label), text);

  package_table_clear_status_css(cell);

  if (GtkWidget *icon = status_cell_icon(cell)) {
    const char *fallback_icon_name = has_pending_action ? nullptr : status_icon_fallback_name(install_state);
    status_image_set_icon(GTK_IMAGE(icon), icon_name, fallback_icon_name);
    gtk_widget_set_visible(icon, icon_name != nullptr);
  }

  if (has_pending_action) {
    const char *pending_class = package_table_pending_action_css_class_for_type(action_type);
    gtk_widget_add_css_class(cell, pending_class);
    if (resolved_only) {
      gtk_widget_add_css_class(cell, "package-status-additional");
    }
  } else {
    if (install_state == PackageInstallState::LOCAL_ONLY) {
      gtk_widget_add_css_class(cell, "package-status-local-only");
    } else if (install_state == PackageInstallState::INSTALLED) {
      gtk_widget_add_css_class(cell, "package-status-installed");
    } else if (install_state == PackageInstallState::INSTALLED_NEWER_THAN_REPO) {
      gtk_widget_add_css_class(cell, "package-status-installed-newer");
    } else if (install_state == PackageInstallState::UPGRADEABLE) {
      if (exact_available_rows && action_rows.has_installed_row && row.row.nevra == action_rows.installed_row.nevra) {
        gtk_widget_add_css_class(cell, "package-status-installed-upgradeable");
      } else {
        gtk_widget_add_css_class(cell, "package-status-upgradeable");
      }
    } else if (install_state == PackageInstallState::DOWNGRADEABLE) {
      gtk_widget_add_css_class(cell, "package-status-downgradeable");
    } else {
      gtk_widget_add_css_class(cell, "package-status-available");
    }
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
