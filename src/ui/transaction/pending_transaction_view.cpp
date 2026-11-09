// -----------------------------------------------------------------------------
// pending_transaction_view.cpp
// Pending transaction tab helpers
//
// Keeps the Pending Actions tab rendering and small pending action list helpers
// out of the package button controller.
// -----------------------------------------------------------------------------
#include "ui/transaction/pending_transaction_view.hpp"

#include "i18n.hpp"
#include "ui/package_query/package_query_controller.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "ui/transaction/pending_transaction_state.hpp"

// Button payload used to jump from one pending action back to its package row.
struct PendingJumpButtonData {
  MainWindowUiState *widgets;
  std::string nevra;
  PendingAction::Type action_type;
  bool installonly;
};

// -----------------------------------------------------------------------------
// Free data owned by one pending action jump button.
// -----------------------------------------------------------------------------
static void
pending_jump_button_data_free(gpointer p)
{
  PendingJumpButtonData *d = static_cast<PendingJumpButtonData *>(p);
  delete d;
}

// -----------------------------------------------------------------------------
// Return true when one pending action matches the requested package and type.
// -----------------------------------------------------------------------------
static bool
has_pending_action(MainWindowUiState *widgets, const std::string &nevra, PendingAction::Type type)
{
  for (const auto &a : widgets->transaction_state.actions) {
    if (a.nevra == nevra && a.type == type) {
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
// Enable the Apply button only when actions are pending.
// -----------------------------------------------------------------------------
static void
update_apply_button(MainWindowUiState *widgets)
{
  if (!widgets || !widgets->transaction_widgets.apply_button || !widgets->transaction_widgets.clear_pending_button) {
    return;
  }

  size_t pending_count = widgets->transaction_state.actions.size();
  bool has_pending = pending_count > 0;
  std::string apply_label = _("Apply Transactions");
  if (has_pending) {
    apply_label += " (" + std::to_string(pending_count) + ")";
  }

  ui_helpers_set_icon_button(widgets->transaction_widgets.apply_button, "system-run-symbolic", apply_label.c_str());
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.apply_button), has_pending);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.clear_pending_button), has_pending);
}

// -----------------------------------------------------------------------------
// Rebuild the Pending Actions tab from the current pending actions.
// -----------------------------------------------------------------------------
void
pending_transaction_refresh_pending_tab(MainWindowUiState *widgets)
{
  // Remove existing rows.
  while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(widgets->transaction_widgets.pending_list, 0)) {
    gtk_list_box_remove(widgets->transaction_widgets.pending_list, GTK_WIDGET(row));
  }

  // Add one row for each pending action.
  for (const auto &a : widgets->transaction_state.actions) {
    std::string prefix;
    const char *icon_name = nullptr;
    const char *fallback_icon_name = nullptr;
    switch (a.type) {
    case PendingAction::INSTALL:
      prefix = _("Install: ");
      icon_name = "list-add-symbolic";
      fallback_icon_name = "document-new-symbolic";
      break;
    case PendingAction::UPGRADE:
      prefix = _("Upgrade: ");
      icon_name = "software-update-available-symbolic";
      fallback_icon_name = "view-refresh-symbolic";
      break;
    case PendingAction::DOWNGRADE:
      prefix = _("Downgrade: ");
      icon_name = "go-down-symbolic";
      fallback_icon_name = "pan-down-symbolic";
      break;
    case PendingAction::REINSTALL:
      prefix = _("Reinstall: ");
      icon_name = "document-revert-symbolic";
      fallback_icon_name = "view-refresh-symbolic";
      break;
    case PendingAction::REMOVE:
      prefix = _("Remove: ");
      icon_name = "user-trash-symbolic";
      fallback_icon_name = "list-remove-symbolic";
      break;
    }
    std::string line = prefix + a.nevra;

    GtkWidget *button = ui_helpers_create_icon_button_with_fallback(icon_name, fallback_icon_name, line.c_str());
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_hexpand(button, TRUE);
    gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(button)), GTK_ALIGN_START);

    PendingJumpButtonData *data = new PendingJumpButtonData { widgets, a.nevra, a.type, a.installonly };
    g_signal_connect_data(
        button,
        "clicked",
        G_CALLBACK(+[](GtkButton *, gpointer user_data) {
          PendingJumpButtonData *data = static_cast<PendingJumpButtonData *>(user_data);
          if (!data) {
            return;
          }

          if (data->widgets) {
            const bool exact_installonly_action = data->action_type == PendingAction::INSTALL && data->installonly;
            package_query_show_exact_package(data->widgets, data->nevra, exact_installonly_action);
          }
        }),
        data,
        +[](gpointer data, GClosure *) { pending_jump_button_data_free(data); },
        GConnectFlags(0));

    gtk_list_box_append(widgets->transaction_widgets.pending_list, button);
  }
  update_apply_button(widgets);
}

// -----------------------------------------------------------------------------
// Remove one pending action by package ID.
// -----------------------------------------------------------------------------
bool
pending_transaction_remove_action(MainWindowUiState *widgets, const std::string &nevra)
{
  for (size_t i = 0; i < widgets->transaction_state.actions.size(); ++i) {
    if (widgets->transaction_state.actions[i].nevra == nevra) {
      widgets->transaction_state.actions.erase(widgets->transaction_state.actions.begin() + i);
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Return the pending action type for one package ID.
// -----------------------------------------------------------------------------
bool
pending_transaction_get_action_type(MainWindowUiState *widgets, const std::string &nevra, PendingAction::Type &out_type)
{
  for (const auto &a : widgets->transaction_state.actions) {
    if (a.nevra == nevra) {
      out_type = a.type;
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Update package action button labels based on pending actions.
// -----------------------------------------------------------------------------
void
pending_transaction_update_action_button_labels_for_selection(MainWindowUiState *widgets,
                                                              const std::string &install_nevra,
                                                              const std::string &remove_nevra,
                                                              const std::string &reinstall_nevra,
                                                              bool install_is_upgrade,
                                                              bool install_is_downgrade)
{
  bool pending_install = has_pending_action(widgets, install_nevra, PendingAction::INSTALL);
  bool pending_upgrade = has_pending_action(widgets, install_nevra, PendingAction::UPGRADE);
  bool pending_downgrade = has_pending_action(widgets, install_nevra, PendingAction::DOWNGRADE);
  bool pending_remove = has_pending_action(widgets, remove_nevra, PendingAction::REMOVE);
  bool pending_reinstall = has_pending_action(widgets, reinstall_nevra, PendingAction::REINSTALL);

  const char *mark_install = _("Mark for Install");
  const char *unmark_install = _("Unmark Install");
  if (install_is_upgrade) {
    mark_install = _("Mark for Upgrade");
    unmark_install = _("Unmark Upgrade");
  } else if (install_is_downgrade) {
    mark_install = _("Mark for Downgrade");
    unmark_install = _("Unmark Downgrade");
  }

  if (pending_install || pending_upgrade || pending_downgrade) {
    ui_helpers_set_icon_button(widgets->transaction_widgets.install_button, "edit-clear-symbolic", unmark_install);
    ui_helpers_set_icon_button(
        widgets->transaction_widgets.remove_button, "user-trash-symbolic", _("Mark for Removal"));
    ui_helpers_set_icon_button_with_fallback(widgets->transaction_widgets.reinstall_button,
                                             "document-revert-symbolic",
                                             "view-refresh-symbolic",
                                             _("Mark for Reinstall"));
  } else if (pending_reinstall) {
    ui_helpers_set_icon_button(widgets->transaction_widgets.install_button, "list-add-symbolic", mark_install);
    ui_helpers_set_icon_button(
        widgets->transaction_widgets.remove_button, "user-trash-symbolic", _("Mark for Removal"));
    ui_helpers_set_icon_button(
        widgets->transaction_widgets.reinstall_button, "edit-clear-symbolic", _("Unmark Reinstall"));
  } else if (pending_remove) {
    ui_helpers_set_icon_button(widgets->transaction_widgets.install_button, "list-add-symbolic", mark_install);
    ui_helpers_set_icon_button(widgets->transaction_widgets.remove_button, "edit-clear-symbolic", _("Unmark Removal"));
    ui_helpers_set_icon_button_with_fallback(widgets->transaction_widgets.reinstall_button,
                                             "document-revert-symbolic",
                                             "view-refresh-symbolic",
                                             _("Mark for Reinstall"));
  } else {
    ui_helpers_set_icon_button(widgets->transaction_widgets.install_button, "list-add-symbolic", mark_install);
    ui_helpers_set_icon_button(
        widgets->transaction_widgets.remove_button, "user-trash-symbolic", _("Mark for Removal"));
    ui_helpers_set_icon_button_with_fallback(widgets->transaction_widgets.reinstall_button,
                                             "document-revert-symbolic",
                                             "view-refresh-symbolic",
                                             _("Mark for Reinstall"));
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
