// -----------------------------------------------------------------------------
// pending_transaction_view.cpp
// Pending transaction tab helpers
//
// Keeps the Pending Actions tab rendering and small pending action list helpers
// out of the package button controller.
// -----------------------------------------------------------------------------
#include "pending_transaction_view.hpp"

#include "base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "package_query_controller.hpp"
#include "package_table_view.hpp"
#include "ui_helpers.hpp"
#include "widgets.hpp"

// Button payload used to jump from one pending action back to its package row.
struct PendingJumpButtonData {
  SearchWidgets *widgets;
  PendingAction action;
};

struct PendingBackendBaseDropGuard {
  ~PendingBackendBaseDropGuard()
  {
    BaseManager::instance().drop_cached_base();
  }
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
// Show the selected pending action in the main package list.
// -----------------------------------------------------------------------------
static void
show_pending_action_package(SearchWidgets *widgets, const PendingAction &action)
{
  if (!widgets) {
    return;
  }

  PendingBackendBaseDropGuard base_drop_guard;

  std::vector<PackageRow> rows;
  switch (action.type) {
  case PendingAction::INSTALL:
  case PendingAction::UPGRADE:
    rows = dnf_backend_get_available_package_rows_by_nevra(action.nevra);
    break;
  case PendingAction::REMOVE:
  case PendingAction::REINSTALL:
    rows = dnf_backend_get_installed_package_rows_by_nevra(action.nevra);
    break;
  }

  if (rows.empty()) {
    ui_helpers_set_status(
        widgets->query.status_label, _("Pending package could not be found in current package data."), "red");
    return;
  }

  // Selecting a pending action shows only that package in the table.
  // After a transaction, refresh it from the selected NEVRA instead of replaying an old search or list view.
  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->results.selected_nevra = action.nevra;
  package_table_fill_package_view(widgets, rows);
}

// -----------------------------------------------------------------------------
// Enable the Apply button only when actions are pending.
// -----------------------------------------------------------------------------
static void
update_apply_button(SearchWidgets *widgets)
{
  if (!widgets || !widgets->transaction.apply_button || !widgets->transaction.clear_pending_button) {
    return;
  }

  size_t pending_count = widgets->transaction.actions.size();
  bool has_pending = pending_count > 0;
  std::string apply_label = _("Apply Transactions");
  if (has_pending) {
    apply_label += " (" + std::to_string(pending_count) + ")";
  }

  ui_helpers_set_icon_button(widgets->transaction.apply_button, "emblem-ok-symbolic", apply_label.c_str());
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.apply_button), has_pending);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.clear_pending_button), has_pending);
}

// -----------------------------------------------------------------------------
// Rebuild the Pending Actions tab from the current pending actions.
// -----------------------------------------------------------------------------
void
pending_transaction_refresh_pending_tab(SearchWidgets *widgets)
{
  // Remove existing rows.
  while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(widgets->transaction.pending_list, 0)) {
    gtk_list_box_remove(widgets->transaction.pending_list, GTK_WIDGET(row));
  }

  // Add one row for each pending action.
  for (const auto &a : widgets->transaction.actions) {
    std::string prefix;
    switch (a.type) {
    case PendingAction::INSTALL:
      prefix = _("Install: ");
      break;
    case PendingAction::UPGRADE:
      prefix = _("Upgrade: ");
      break;
    case PendingAction::REINSTALL:
      prefix = _("Reinstall: ");
      break;
    case PendingAction::REMOVE:
      prefix = _("Remove: ");
      break;
    }
    std::string line = prefix + a.nevra;

    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_hexpand(button, TRUE);

    GtkWidget *label = gtk_label_new(line.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    gtk_button_set_child(GTK_BUTTON(button), label);

    PendingJumpButtonData *data = new PendingJumpButtonData { widgets, a };
    g_signal_connect_data(
        button,
        "clicked",
        G_CALLBACK(+[](GtkButton *, gpointer user_data) {
          PendingJumpButtonData *data = static_cast<PendingJumpButtonData *>(user_data);
          if (!data) {
            return;
          }

          show_pending_action_package(data->widgets, data->action);
        }),
        data,
        +[](gpointer data, GClosure *) { pending_jump_button_data_free(data); },
        GConnectFlags(0));

    gtk_list_box_append(widgets->transaction.pending_list, button);
  }
  update_apply_button(widgets);
}

// -----------------------------------------------------------------------------
// Remove one pending action by package ID.
// -----------------------------------------------------------------------------
bool
pending_transaction_remove_action(SearchWidgets *widgets, const std::string &nevra)
{
  for (size_t i = 0; i < widgets->transaction.actions.size(); ++i) {
    if (widgets->transaction.actions[i].nevra == nevra) {
      widgets->transaction.actions.erase(widgets->transaction.actions.begin() + i);
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Return the pending action type for one package ID.
// -----------------------------------------------------------------------------
bool
pending_transaction_get_action_type(SearchWidgets *widgets, const std::string &nevra, PendingAction::Type &out_type)
{
  for (const auto &a : widgets->transaction.actions) {
    if (a.nevra == nevra) {
      out_type = a.type;
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
