// -----------------------------------------------------------------------------
// package_table_view.cpp
// Package table view
// Builds the GTK4 ColumnView and keeps package selection wired into the details panel controller.
// -----------------------------------------------------------------------------
#include "ui/common/ui_helpers.hpp"

#include "i18n.hpp"
#include "ui/details/package_details_controller.hpp"
#include "ui/package_table/package_table_context_menu.hpp"
#include "ui/package_table/package_table_columns.hpp"
#include "ui/package_table/package_table_status.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/package_table/package_table_view_internal.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"
#include "ui/transaction/pending_transaction_controller.hpp"
#include "ui/common/widgets.hpp"

#include <set>
#include <string>
#include <vector>
#include <utility>

static void update_pending_action_css_for_cell(GtkWidget *cell,
                                               MainWindowUiState *widgets,
                                               const PackageTableRow &row,
                                               const PendingTransactionActionRows &action_rows);

// -----------------------------------------------------------------------------
// Return the filter model attached to the current package table.
// -----------------------------------------------------------------------------
static GtkFilterListModel *
package_table_quick_filter_model(MainWindowUiState *widgets)
{
  GtkWidget *view = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!GTK_IS_COLUMN_VIEW(view)) {
    return nullptr;
  }

  GtkMultiSelection *selection = GTK_MULTI_SELECTION(gtk_column_view_get_model(GTK_COLUMN_VIEW(view)));
  GtkSortListModel *sort_model = GTK_SORT_LIST_MODEL(gtk_multi_selection_get_model(selection));
  return GTK_FILTER_LIST_MODEL(gtk_sort_list_model_get_model(sort_model));
}

// -----------------------------------------------------------------------------
// Return true while the package filter is still processing results.
// -----------------------------------------------------------------------------
bool
package_table_quick_filter_is_pending(MainWindowUiState *widgets)
{
  if (!widgets || !widgets->results.list_scroller) {
    return false;
  }

  GtkFilterListModel *model = package_table_quick_filter_model(widgets);
  return model && gtk_filter_list_model_get_pending(model) > 0;
}

// -----------------------------------------------------------------------------
// Update the count of packages shown by the filter.
// -----------------------------------------------------------------------------
static void
update_quick_filter_count(MainWindowUiState *widgets)
{
  GtkFilterListModel *model = package_table_quick_filter_model(widgets);
  if (!model) {
    return;
  }

  size_t total = g_list_model_get_n_items(gtk_filter_list_model_get_model(model));
  size_t matching = g_list_model_get_n_items(G_LIST_MODEL(model));
  std::string text;
  if (!gtk_filter_list_model_get_filter(model)) {
    text = dnfui_i18n_format_count(total, "Item: %zu", "Items: %zu");
  } else if (gtk_filter_list_model_get_pending(model) > 0) {
    text =
        dnfui_i18n_format_two_counts(matching, total, "Item: %zu of %zu (filtering)", "Items: %zu of %zu (filtering)");
  } else {
    text = dnfui_i18n_format_two_counts(matching, total, "Item: %zu of %zu", "Items: %zu of %zu");
  }
  gtk_label_set_text(widgets->results.count_label, text.c_str());
}

static void
on_quick_filter_items_changed(GListModel *, guint, guint, guint, gpointer user_data)
{
  update_quick_filter_count(static_cast<MainWindowUiState *>(user_data));
}

static void
on_quick_filter_pending_changed(GObject *, GParamSpec *, gpointer user_data)
{
  update_quick_filter_count(static_cast<MainWindowUiState *>(user_data));
}

// -----------------------------------------------------------------------------
// Match package names and summaries without another backend query.
// -----------------------------------------------------------------------------
static gboolean
quick_filter_matches(gpointer object, gpointer user_data)
{
  const PackageItem *item = package_item_from_object(G_OBJECT(object));
  const char *text =
      static_cast<const char *>(g_object_get_data(G_OBJECT(user_data), "dnfui-package-quick-filter-text"));
  return item && package_table_quick_filter_matches(item->row, text);
}

// -----------------------------------------------------------------------------
// Apply the local filter when the entry text changes.
// -----------------------------------------------------------------------------
void
package_table_on_quick_filter_changed(GtkEditable *editable, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  const char *text = gtk_editable_get_text(editable);
  g_object_set_data_full(
      G_OBJECT(editable), "dnfui-package-quick-filter-text", *text ? g_utf8_casefold(text, -1) : nullptr, g_free);

  GtkFilterListModel *model = package_table_quick_filter_model(widgets);
  if (!model) {
    return;
  }

  GtkFilter *filter = gtk_filter_list_model_get_filter(model);
  if (!*text) {
    gtk_filter_list_model_set_filter(model, nullptr);
  } else if (filter) {
    gtk_filter_changed(filter, GTK_FILTER_CHANGE_DIFFERENT);
  } else {
    filter = GTK_FILTER(gtk_custom_filter_new(quick_filter_matches, editable, nullptr));
    gtk_filter_list_model_set_filter(model, filter);
    g_object_unref(filter);
  }
  update_quick_filter_count(widgets);
}

// -----------------------------------------------------------------------------
// Clearing the quick filter also restores all package rows.
// -----------------------------------------------------------------------------
void
package_table_on_quick_filter_mode_changed(GObject *bar, GParamSpec *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar))) {
    gtk_editable_set_text(GTK_EDITABLE(widgets->results.quick_filter_entry), "");
  }
}

// -----------------------------------------------------------------------------
// Hide the filter before replacing a package table.
// -----------------------------------------------------------------------------
static void
reset_quick_filter(MainWindowUiState *widgets)
{
  gtk_search_bar_set_key_capture_widget(widgets->results.quick_filter_bar, nullptr);
  gtk_search_bar_set_search_mode(widgets->results.quick_filter_bar, FALSE);
  gtk_editable_set_text(GTK_EDITABLE(widgets->results.quick_filter_entry), "");
}

// -----------------------------------------------------------------------------
// Refresh stored package status values without changing the GTK model.
// -----------------------------------------------------------------------------
static void
refresh_model_status_values(GtkColumnView *view, MainWindowUiState *widgets)
{
  GtkSelectionModel *model = gtk_column_view_get_model(view);
  if (!model) {
    return;
  }

  GtkMultiSelection *selection = GTK_MULTI_SELECTION(model);
  GtkSortListModel *sort_model = GTK_SORT_LIST_MODEL(gtk_multi_selection_get_model(selection));
  GtkFilterListModel *filtered_model = GTK_FILTER_LIST_MODEL(gtk_sort_list_model_get_model(sort_model));
  GListModel *items_model = gtk_filter_list_model_get_model(filtered_model);

  guint n_items = g_list_model_get_n_items(items_model);
  for (guint i = 0; i < n_items; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(items_model, i));
    PackageItem *item = mutable_package_item_from_object(obj);
    if (item) {
      InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item->row);
      package_table_fill_item_status(widgets, *item, resolution);
    }
    g_object_unref(obj);
  }
}

// -----------------------------------------------------------------------------
// Refresh status cells and pending row colors currently realized by the virtualized view.
// -----------------------------------------------------------------------------
static void
refresh_visible_status_labels(GtkWidget *widget, MainWindowUiState *widgets)
{
  if (!widget) {
    return;
  }

  PackageTableRow *row = static_cast<PackageTableRow *>(g_object_get_data(G_OBJECT(widget), "package-context-row"));
  if (row) {
    const bool exact_installonly_actions =
        displayed_package_query_uses_exact_installonly_actions(widgets->query_state.displayed_query);
    PendingTransactionActionRows action_rows =
        pending_transaction_action_rows_for_selection_with_pending(row->row,
                                                                   row->upgrade_target(),
                                                                   row->upgrade_generation(),
                                                                   exact_installonly_actions,
                                                                   widgets->transaction_state.actions);
    update_pending_action_css_for_cell(widget, widgets, *row, action_rows);
    if (g_object_get_data(G_OBJECT(widget), "package-status-cell")) {
      package_table_update_resolved_status_label(widget, widgets, *row, action_rows);
    }
  }

  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    refresh_visible_status_labels(child, widgets);
  }
}

// -----------------------------------------------------------------------------
// Build the message shown when the package table has no rows.
// -----------------------------------------------------------------------------
static GtkWidget *
create_empty_package_view(PackageTableEmptyState state)
{
  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(outer, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(outer, TRUE);
  gtk_widget_set_vexpand(outer, TRUE);
  gtk_widget_add_css_class(outer, "package-empty-state");

  GtkWidget *icon = gtk_image_new_from_icon_name("system-search-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 36);
  gtk_widget_set_halign(icon, GTK_ALIGN_START);
  gtk_widget_add_css_class(icon, "package-empty-icon");
  gtk_box_append(GTK_BOX(outer), icon);

  const char *title_text = _("Find packages");
  const char *message_text = _("Search for packages, or choose a list option to browse packages.");
  if (state == PackageTableEmptyState::NO_RESULTS) {
    title_text = _("No packages found");
    message_text = _("Try a different search or refresh repositories.");
  }

  GtkWidget *title = gtk_label_new(nullptr);
  gchar *title_markup = g_markup_printf_escaped("<b>%s</b>", title_text);
  gtk_label_set_markup(GTK_LABEL(title), title_markup);
  g_free(title_markup);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "package-empty-title");
  gtk_box_append(GTK_BOX(outer), title);

  GtkWidget *message = gtk_label_new(message_text);
  gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_widget_add_css_class(message, "package-empty-message");
  gtk_box_append(GTK_BOX(outer), message);

  GtkWidget *shortcuts = gtk_label_new(_("Shortcuts\n"
                                         "Ctrl+F: Focus search\n"
                                         "Ctrl+L: Clear package list\n"
                                         "Ctrl+E: Export package list\n"
                                         "Ctrl+Enter: Apply pending changes\n"
                                         "F5: Refresh repositories\n"
                                         "Ctrl+H: Toggle history panel\n"
                                         "Ctrl+I: Toggle package info panel\n"
                                         "Ctrl+Shift+H: Open transaction history\n"
                                         "Ctrl+Shift+R: Open repositories\n"
                                         "Ctrl+Q or Ctrl+W: Quit"));
  gtk_label_set_xalign(GTK_LABEL(shortcuts), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(shortcuts), TRUE);
  gtk_widget_add_css_class(shortcuts, "package-empty-shortcuts");
  gtk_box_append(GTK_BOX(outer), shortcuts);

  return outer;
}

// -----------------------------------------------------------------------------
// Return the text label inside a table cell.
// -----------------------------------------------------------------------------
static GtkWidget *
table_cell_label(GtkWidget *cell)
{
  GtkWidget *label = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(cell), "package-status-label"));
  return label ? label : cell;
}

// -----------------------------------------------------------------------------
// Return the widget that owns the package row state for one table cell.
// -----------------------------------------------------------------------------
static GtkWidget *
table_cell_frame(GtkWidget *cell)
{
  GtkWidget *frame = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(cell), "package-cell-frame"));
  return frame ? frame : cell;
}

// -----------------------------------------------------------------------------
// Return the GTK cell wrapper when it exists.
// -----------------------------------------------------------------------------
static GtkWidget *
table_cell_color_target(GtkWidget *cell)
{
  GtkWidget *parent = gtk_widget_get_parent(cell);
  if (!parent || GTK_IS_COLUMN_VIEW(parent)) {
    return cell;
  }

  return parent;
}

// -----------------------------------------------------------------------------
// Shade every second row according to its current position in the sorted view.
// -----------------------------------------------------------------------------
static void
update_alternating_row_css(GtkListItem *item)
{
  GtkWidget *cell = gtk_list_item_get_child(item);
  if (!cell) {
    return;
  }

  GtkWidget *target = table_cell_color_target(cell);
  gtk_widget_remove_css_class(cell, "package-row-alternate");
  gtk_widget_remove_css_class(target, "package-row-alternate");

  guint position = gtk_list_item_get_position(item);
  if (position != GTK_INVALID_LIST_POSITION && position % 2 == 1) {
    gtk_widget_add_css_class(target, "package-row-alternate");
  }
}

// -----------------------------------------------------------------------------
// Remove pending row CSS classes from one table widget.
// -----------------------------------------------------------------------------
static void
clear_pending_row_css(GtkWidget *widget)
{
  gtk_widget_remove_css_class(widget, "package-row-pending-install");
  gtk_widget_remove_css_class(widget, "package-row-pending-reinstall");
  gtk_widget_remove_css_class(widget, "package-row-pending-remove");
}

// -----------------------------------------------------------------------------
// Remove pending row color from one table cell.
// -----------------------------------------------------------------------------
static void
clear_pending_row_css_for_cell(GtkWidget *cell)
{
  clear_pending_row_css(cell);
  clear_pending_row_css(table_cell_color_target(cell));
}

// -----------------------------------------------------------------------------
// Return the pending row CSS class for one package row.
// -----------------------------------------------------------------------------
static const char *
pending_row_css_class(MainWindowUiState *widgets,
                      const PackageTableRow &row,
                      const PendingTransactionActionRows &action_rows)
{
  PendingAction::Type action_type;
  if (!package_table_pending_action_for_resolved_row(widgets, row, action_rows, action_type)) {
    return nullptr;
  }

  const char *status_class = package_table_pending_action_css_class_for_type(action_type);
  if (!status_class) {
    return nullptr;
  }
  if (std::string(status_class) == "package-status-pending-install") {
    return "package-row-pending-install";
  }
  if (std::string(status_class) == "package-status-pending-reinstall") {
    return "package-row-pending-reinstall";
  }
  if (std::string(status_class) == "package-status-pending-remove") {
    return "package-row-pending-remove";
  }

  return nullptr;
}

// -----------------------------------------------------------------------------
// Apply pending action color to one table cell.
// -----------------------------------------------------------------------------
static void
update_pending_action_css_for_cell(GtkWidget *cell,
                                   MainWindowUiState *widgets,
                                   const PackageTableRow &row,
                                   const PendingTransactionActionRows &action_rows)
{
  const char *pending_class = pending_row_css_class(widgets, row, action_rows);
  GtkWidget *target = table_cell_color_target(cell);

  clear_pending_row_css(cell);
  clear_pending_row_css(target);
  if (pending_class) {
    gtk_widget_add_css_class(target, pending_class);
  }
}

// -----------------------------------------------------------------------------
// Build the Status column cell with a symbolic icon and text label.
// -----------------------------------------------------------------------------
static GtkWidget *
create_status_cell()
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);
  gtk_widget_add_css_class(box, "package-status");

  GtkWidget *icon = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 14);
  gtk_widget_add_css_class(icon, "package-status-icon");
  gtk_box_append(GTK_BOX(box), icon);

  GtkWidget *label = gtk_label_new(nullptr);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_box_append(GTK_BOX(box), label);

  g_object_set_data(G_OBJECT(box), "package-status-cell", GINT_TO_POINTER(1));
  g_object_set_data(G_OBJECT(box), "package-status-label", label);
  g_object_set_data(G_OBJECT(box), "package-status-icon", icon);

  return box;
}

// -----------------------------------------------------------------------------
// Select the package row used by the context menu action.
// -----------------------------------------------------------------------------
static bool
select_package_table_row(GtkColumnView *view, const std::string &nevra)
{
  if (!view) {
    return false;
  }

  GtkSelectionModel *model = gtk_column_view_get_model(view);
  if (!model) {
    return false;
  }

  GListModel *items_model = G_LIST_MODEL(model);
  if (!items_model) {
    return false;
  }

  guint n_items = g_list_model_get_n_items(items_model);
  for (guint i = 0; i < n_items; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(items_model, i));
    const PackageRow *row = package_row_from_object(obj);
    bool match = row && row->nevra == nevra;
    g_object_unref(obj);

    if (match) {
      if (!gtk_selection_model_is_selected(model, i)) {
        gtk_selection_model_select_item(model, i, TRUE);
      }
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
// Return true when one GTK column belongs to the requested table column kind.
// -----------------------------------------------------------------------------
static bool
package_table_column_has_kind(GtkColumnViewColumn *column, PackageColumnKind kind)
{
  if (!column) {
    return false;
  }

  PackageColumnKind column_kind =
      static_cast<PackageColumnKind>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "package-column-kind")));
  return column_kind == kind;
}

// -----------------------------------------------------------------------------
// Return the preferred visible sort column, or the first visible column.
// The returned column keeps a reference for the caller.
// -----------------------------------------------------------------------------
static GtkColumnViewColumn *
package_table_find_visible_sort_column(GtkColumnView *view, PackageColumnKind preferred_kind)
{
  if (!view) {
    return nullptr;
  }

  GListModel *columns = gtk_column_view_get_columns(view);
  guint n_columns = g_list_model_get_n_items(columns);
  GtkColumnViewColumn *fallback = nullptr;

  for (guint i = 0; i < n_columns; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(columns, i));
    GtkColumnViewColumn *column = GTK_COLUMN_VIEW_COLUMN(obj);
    if (!gtk_column_view_column_get_visible(column)) {
      g_object_unref(obj);
      continue;
    }

    if (package_table_column_has_kind(column, preferred_kind)) {
      if (fallback) {
        g_object_unref(fallback);
      }
      return column;
    }

    if (!fallback) {
      fallback = column;
    } else {
      g_object_unref(obj);
    }
  }

  return fallback;
}

// -----------------------------------------------------------------------------
// Move sorting away from a hidden column.
// -----------------------------------------------------------------------------
static void
package_table_ensure_sort_column_visible(GtkColumnView *view)
{
  if (!view) {
    return;
  }

  GtkSorter *sorter = gtk_column_view_get_sorter(view);
  if (!sorter || !GTK_IS_COLUMN_VIEW_SORTER(sorter)) {
    return;
  }

  GtkColumnViewColumn *column = gtk_column_view_sorter_get_primary_sort_column(GTK_COLUMN_VIEW_SORTER(sorter));
  if (!column || gtk_column_view_column_get_visible(column)) {
    return;
  }

  GtkColumnViewColumn *fallback = package_table_find_visible_sort_column(view, PackageColumnKind::PACKAGE);
  if (!fallback) {
    return;
  }

  gtk_column_view_sort_by_column(view, fallback, GTK_SORT_ASCENDING);
  g_object_unref(fallback);
}

// -----------------------------------------------------------------------------
// Apply saved column visibility to the current table when it exists.
// -----------------------------------------------------------------------------
static void
package_table_apply_column_visibility(GtkColumnView *view)
{
  if (!view) {
    return;
  }

  std::set<std::string> visible = package_table_load_visible_column_ids();
  GListModel *columns = gtk_column_view_get_columns(view);
  guint n_columns = g_list_model_get_n_items(columns);
  gint last_visible_column = -1;
  bool has_expanding_visible_column = false;
  for (guint i = 0; i < n_columns; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(columns, i));
    GtkColumnViewColumn *column = GTK_COLUMN_VIEW_COLUMN(obj);
    const char *column_id = static_cast<const char *>(g_object_get_data(G_OBJECT(column), "package-column-id"));
    const PackageTableColumnDefinition *definition = package_table_column_definition_by_id(column_id);
    bool column_visible = column_id && visible.count(column_id) > 0;
    gtk_column_view_column_set_visible(column, column_visible);
    gtk_column_view_column_set_expand(column, definition && definition->expand);
    if (column_visible) {
      last_visible_column = static_cast<gint>(i);
      has_expanding_visible_column = has_expanding_visible_column || (definition && definition->expand);
    }
    g_object_unref(obj);
  }

  package_table_ensure_sort_column_visible(view);

  if (!has_expanding_visible_column && last_visible_column >= 0) {
    GObject *obj = G_OBJECT(g_list_model_get_item(columns, static_cast<guint>(last_visible_column)));
    gtk_column_view_column_set_expand(GTK_COLUMN_VIEW_COLUMN(obj), TRUE);
    g_object_unref(obj);
  }
}

// -----------------------------------------------------------------------------
// Build one text column for the package table.
// -----------------------------------------------------------------------------
static GtkColumnViewColumn *
create_text_column(MainWindowUiState *widgets, const PackageTableColumnDefinition &definition)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_object_set_data(G_OBJECT(factory), "package-column-kind", GINT_TO_POINTER(static_cast<int>(definition.kind)));
  g_object_set_data(G_OBJECT(factory), "package-table-widgets", widgets);

  g_signal_connect(
      factory,
      "setup",
      G_CALLBACK(+[](GtkSignalListItemFactory *factory, GtkListItem *item, gpointer) {
        PackageColumnKind kind = static_cast<PackageColumnKind>(
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(factory), "package-column-kind")));

        GtkWidget *cell = nullptr;
        GtkWidget *label = nullptr;
        if (kind == PackageColumnKind::STATUS) {
          cell = create_status_cell();
          label = table_cell_label(cell);
        } else {
          cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
          gtk_widget_set_hexpand(cell, TRUE);
          label = gtk_label_new(nullptr);
          gtk_widget_set_margin_start(label, 6);
          gtk_widget_set_margin_end(label, 6);
          gtk_widget_set_margin_top(label, 4);
          gtk_widget_set_margin_bottom(label, 4);
          gtk_widget_set_hexpand(label, TRUE);
          gtk_box_append(GTK_BOX(cell), label);
          g_object_set_data(G_OBJECT(cell), "package-cell-frame", cell);
          g_object_set_data(G_OBJECT(cell), "package-status-label", label);
        }
        gtk_list_item_set_activatable(item, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        if (kind == PackageColumnKind::VERSION || kind == PackageColumnKind::UPDATE_VERSION ||
            kind == PackageColumnKind::RELEASE || kind == PackageColumnKind::UPDATE_RELEASE ||
            kind == PackageColumnKind::ARCH || kind == PackageColumnKind::REPO || kind == PackageColumnKind::SIZE) {
          gtk_widget_add_css_class(label, "package-meta");
        }
        if (kind == PackageColumnKind::SUMMARY) {
          gtk_widget_add_css_class(label, "package-summary");
        }

        // Right-click opens the same package actions as the main buttons.
        GtkGesture *context_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context_click), GDK_BUTTON_SECONDARY);
        g_signal_connect(context_click,
                         "pressed",
                         G_CALLBACK(+[](GtkGestureClick *gesture, int, double x, double y, gpointer user_data) {
                           MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
                           GtkWidget *cell = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
                           PackageTableRow *row =
                               static_cast<PackageTableRow *>(g_object_get_data(G_OBJECT(cell), "package-context-row"));
                           if (!row) {
                             return;
                           }

                           GtkWidget *view = gtk_widget_get_ancestor(cell, GTK_TYPE_COLUMN_VIEW);
                           if (!view || !GTK_IS_COLUMN_VIEW(view)) {
                             return;
                           }

                           package_table_show_context_menu(cell, widgets, *row, x, y, [view](const std::string &nevra) {
                             return select_package_table_row(GTK_COLUMN_VIEW(view), nevra);
                           });
                         }),
                         g_object_get_data(G_OBJECT(factory), "package-table-widgets"));
        gtk_widget_add_controller(cell, GTK_EVENT_CONTROLLER(context_click));

        gtk_list_item_set_child(item, cell);
        g_signal_connect(
            item,
            "notify::position",
            G_CALLBACK(+[](GtkListItem *item, GParamSpec *, gpointer) { update_alternating_row_css(item); }),
            nullptr);
      }),
      nullptr);

  g_signal_connect(factory,
                   "bind",
                   G_CALLBACK(+[](GtkSignalListItemFactory *factory, GtkListItem *item, gpointer) {
                     MainWindowUiState *widgets = static_cast<MainWindowUiState *>(
                         g_object_get_data(G_OBJECT(factory), "package-table-widgets"));
                     PackageColumnKind kind = static_cast<PackageColumnKind>(
                         GPOINTER_TO_INT(g_object_get_data(G_OBJECT(factory), "package-column-kind")));

                     GtkWidget *cell = gtk_list_item_get_child(item);
                     GtkWidget *label = table_cell_label(cell);
                     GtkWidget *frame = table_cell_frame(cell);
                     update_alternating_row_css(item);
                     GObject *obj = G_OBJECT(gtk_list_item_get_item(item));
                     const PackageItem *package_item = package_item_from_object(obj);

                     if (!package_item) {
                       gtk_label_set_text(GTK_LABEL(label), "");
                       package_table_clear_status_css(cell);
                       package_table_clear_pending_action_css(frame);
                       clear_pending_row_css_for_cell(frame);
                       g_object_set_data_full(G_OBJECT(frame), "package-context-row", nullptr, nullptr);
                       return;
                     }

                     // Store the package row currently bound to this reused table cell.
                     PackageTableRow table_row = package_table_row_from_item(*package_item);
                     g_object_set_data_full(
                         G_OBJECT(frame), "package-context-row", new PackageTableRow(table_row), +[](gpointer p) {
                           delete static_cast<PackageTableRow *>(p);
                         });

                     const bool exact_installonly_actions =
                         displayed_package_query_uses_exact_installonly_actions(widgets->query_state.displayed_query);
                     PendingTransactionActionRows action_rows =
                         pending_transaction_action_rows_for_selection_with_pending(table_row.row,
                                                                                    table_row.upgrade_target(),
                                                                                    table_row.upgrade_generation(),
                                                                                    exact_installonly_actions,
                                                                                    widgets->transaction_state.actions);
                     update_pending_action_css_for_cell(frame, widgets, table_row, action_rows);
                     if (kind == PackageColumnKind::STATUS) {
                       package_table_update_resolved_status_label(cell, widgets, table_row, action_rows);
                     } else {
                       std::string text = package_table_column_text(*package_item, kind);
                       gtk_label_set_text(GTK_LABEL(label), text.c_str());
                     }
                   }),
                   nullptr);

  GtkColumnViewColumn *column = gtk_column_view_column_new(_(definition.title), nullptr);
  gtk_column_view_column_set_factory(column, factory);
  g_object_unref(factory);

  g_object_set_data(G_OBJECT(column), "package-column-kind", GINT_TO_POINTER(static_cast<int>(definition.kind)));
  g_object_set_data(G_OBJECT(column), "package-column-id", const_cast<char *>(definition.id));
  gtk_column_view_column_set_resizable(column, TRUE);
  gtk_column_view_column_set_expand(column, definition.expand);

  gpointer sorter_data = GINT_TO_POINTER(static_cast<int>(definition.kind) + 1);
  GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(package_table_column_sorter_compare, sorter_data, nullptr));
  gtk_column_view_column_set_sorter(column, sorter);
  g_object_unref(sorter);

  if (definition.fixed_width > 0) {
    gtk_column_view_column_set_fixed_width(column, definition.fixed_width);
  }
  return column;
}

// -----------------------------------------------------------------------------
// Append one package table column and release the caller reference.
// -----------------------------------------------------------------------------
static void
append_package_column(GtkColumnView *view, GtkColumnViewColumn *column)
{
  gtk_column_view_append_column(view, column);
  g_object_unref(column);
}

// -----------------------------------------------------------------------------
// Read the current primary package table sort before rebuilding the GTK view.
// -----------------------------------------------------------------------------
static bool
get_package_view_sort_state(MainWindowUiState *widgets, PackageColumnKind &out_kind, GtkSortType &out_order)
{
  if (!widgets || !widgets->results.list_scroller) {
    return false;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return false;
  }

  GtkSorter *sorter = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(child));
  if (!sorter || !GTK_IS_COLUMN_VIEW_SORTER(sorter)) {
    return false;
  }

  GtkColumnViewColumn *column = gtk_column_view_sorter_get_primary_sort_column(GTK_COLUMN_VIEW_SORTER(sorter));
  if (!column || !gtk_column_view_column_get_visible(column)) {
    return false;
  }

  out_kind =
      static_cast<PackageColumnKind>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "package-column-kind")));
  out_order = gtk_column_view_sorter_get_primary_sort_order(GTK_COLUMN_VIEW_SORTER(sorter));
  return true;
}

// -----------------------------------------------------------------------------
// Reapply the primary package table sort after rebuilding the GTK view.
// -----------------------------------------------------------------------------
static void
restore_package_view_sort_state(GtkColumnView *view, PackageColumnKind kind, GtkSortType order)
{
  if (!view) {
    return;
  }

  GListModel *columns = gtk_column_view_get_columns(view);
  guint n_columns = g_list_model_get_n_items(columns);

  for (guint i = 0; i < n_columns; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(columns, i));
    GtkColumnViewColumn *column = GTK_COLUMN_VIEW_COLUMN(obj);
    PackageColumnKind column_kind =
        static_cast<PackageColumnKind>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "package-column-kind")));

    if (column_kind == kind && gtk_column_view_column_get_visible(column)) {
      gtk_column_view_sort_by_column(view, column, order);
      g_object_unref(obj);
      return;
    }

    g_object_unref(obj);
  }
}

// -----------------------------------------------------------------------------
// Return all selected package table rows in the current sort order.
// -----------------------------------------------------------------------------
std::vector<PackageTableRow>
package_table_get_selected_packages(MainWindowUiState *widgets)
{
  std::vector<PackageTableRow> rows;
  if (!widgets || !widgets->results.list_scroller) {
    return rows;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return rows;
  }

  GtkSelectionModel *model = gtk_column_view_get_model(GTK_COLUMN_VIEW(child));
  if (!model) {
    return rows;
  }

  GtkBitset *selected = gtk_selection_model_get_selection(model);
  rows.reserve(gtk_bitset_get_size(selected));
  GtkBitsetIter iter;
  guint position;
  if (gtk_bitset_iter_init_first(&iter, selected, &position)) {
    do {
      GObject *obj = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(model), position));
      const PackageItem *item = package_item_from_object(obj);
      if (item) {
        rows.push_back(package_table_row_from_item(*item));
      }
      if (obj) {
        g_object_unref(obj);
      }
    } while (gtk_bitset_iter_next(&iter, &position));
  }

  gtk_bitset_unref(selected);
  return rows;
}

// -----------------------------------------------------------------------------
// Return the package row when exactly one package is selected.
// -----------------------------------------------------------------------------
bool
package_table_get_selected_package(MainWindowUiState *widgets, PackageTableRow &out_pkg)
{
  std::vector<PackageTableRow> rows = package_table_get_selected_packages(widgets);
  if (rows.size() != 1) {
    return false;
  }

  out_pkg = std::move(rows.front());
  return true;
}

// -----------------------------------------------------------------------------
// Return all package table rows currently displayed in the package table.
// -----------------------------------------------------------------------------
std::vector<PackageTableRow>
package_table_get_displayed_packages(MainWindowUiState *widgets)
{
  std::vector<PackageTableRow> rows;
  if (!widgets || !widgets->results.list_scroller) {
    return rows;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return rows;
  }

  GtkSelectionModel *model = gtk_column_view_get_model(GTK_COLUMN_VIEW(child));
  if (!model) {
    return rows;
  }

  GListModel *items_model = G_LIST_MODEL(model);
  if (!items_model) {
    return rows;
  }

  guint n_items = g_list_model_get_n_items(items_model);
  rows.reserve(n_items);
  for (guint i = 0; i < n_items; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(items_model, i));
    const PackageItem *item = package_item_from_object(obj);
    if (item) {
      rows.push_back(package_table_row_from_item(*item));
    }
    g_object_unref(obj);
  }

  return rows;
}

// -----------------------------------------------------------------------------
// Refresh package status text and colors without rebuilding the package table.
// -----------------------------------------------------------------------------
void
package_table_refresh_statuses(MainWindowUiState *widgets)
{
  if (!widgets || !widgets->results.list_scroller) {
    return;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return;
  }

  refresh_model_status_values(GTK_COLUMN_VIEW(child), widgets);
  refresh_visible_status_labels(child, widgets);
}

// -----------------------------------------------------------------------------
// Return true when one package table column is visible in the current GTK view.
// -----------------------------------------------------------------------------
bool
package_table_column_is_visible(MainWindowUiState *widgets, PackageColumnKind kind)
{
  if (!widgets || !widgets->results.list_scroller) {
    return false;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return false;
  }

  GListModel *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(child));
  guint n_columns = g_list_model_get_n_items(columns);
  for (guint i = 0; i < n_columns; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(columns, i));
    GtkColumnViewColumn *column = GTK_COLUMN_VIEW_COLUMN(obj);
    bool visible = package_table_column_has_kind(column, kind) && gtk_column_view_column_get_visible(column);
    g_object_unref(obj);
    if (visible) {
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
// Change one package table column setting and update the current table if shown.
// -----------------------------------------------------------------------------
bool
package_table_set_column_visible(MainWindowUiState *widgets, const char *column_id, bool visible)
{
  if (!package_table_column_definition_by_id(column_id)) {
    return false;
  }

  std::set<std::string> visible_columns = package_table_load_visible_column_ids();
  if (!package_table_update_visible_column_ids(visible_columns, column_id, visible)) {
    return false;
  }

  package_table_save_visible_column_ids(visible_columns);

  if (widgets && widgets->results.list_scroller) {
    GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
    if (child && GTK_IS_COLUMN_VIEW(child)) {
      package_table_apply_column_visibility(GTK_COLUMN_VIEW(child));
    }
  }
  package_details_refresh_selected_package_actions(widgets);

  return true;
}

// -----------------------------------------------------------------------------
// Reset package table columns to their default visibility and update the table.
// -----------------------------------------------------------------------------
void
package_table_reset_columns_to_default(MainWindowUiState *widgets)
{
  package_table_reset_visible_column_ids();

  if (widgets && widgets->results.list_scroller) {
    GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
    if (child && GTK_IS_COLUMN_VIEW(child)) {
      package_table_apply_column_visibility(GTK_COLUMN_VIEW(child));
    }
  }
  package_details_refresh_selected_package_actions(widgets);
}

static void
finish_package_table_view(MainWindowUiState *widgets,
                          GListStore *store,
                          size_t item_count,
                          PackageColumnKind sort_kind,
                          GtkSortType sort_order,
                          bool have_sort_state)
{
  GtkColumnView *view = GTK_COLUMN_VIEW(gtk_column_view_new(nullptr));
  gtk_widget_add_css_class(GTK_WIDGET(view), "package-table-view");
  gtk_widget_set_hexpand(GTK_WIDGET(view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(view), TRUE);
  gtk_column_view_set_single_click_activate(view, FALSE);
  gtk_column_view_set_show_row_separators(view, TRUE);
  gtk_column_view_set_show_column_separators(view, FALSE);

  for (const auto &column : package_table_column_definitions()) {
    append_package_column(view, create_text_column(widgets, column));
  }
  package_table_apply_column_visibility(view);

  // Filter loaded packages before applying the existing column sorting.
  GtkFilterListModel *filtered_model = gtk_filter_list_model_new(nullptr, nullptr);
  gtk_filter_list_model_set_model(filtered_model, G_LIST_MODEL(store));
  gtk_filter_list_model_set_incremental(filtered_model, TRUE);
  g_signal_connect(filtered_model, "items-changed", G_CALLBACK(on_quick_filter_items_changed), widgets);
  g_signal_connect(filtered_model, "notify::pending", G_CALLBACK(on_quick_filter_pending_changed), widgets);
  g_object_unref(store);

  GtkSortListModel *sort_model = gtk_sort_list_model_new(nullptr, nullptr);
  gtk_sort_list_model_set_model(sort_model, G_LIST_MODEL(filtered_model));
  gtk_sort_list_model_set_sorter(sort_model, gtk_column_view_get_sorter(view));
  g_object_unref(filtered_model);

  GtkMultiSelection *sel = gtk_multi_selection_new(nullptr);
  gtk_multi_selection_set_model(sel, G_LIST_MODEL(sort_model));

  g_signal_connect(sel,
                   "selection-changed",
                   G_CALLBACK(+[](GtkSelectionModel *self, guint, guint, gpointer user_data) {
                     MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
                     GtkBitset *selected_rows = gtk_selection_model_get_selection(self);
                     guint count = gtk_bitset_get_size(selected_rows);
                     guint index = count == 1 ? gtk_bitset_get_nth(selected_rows, 0) : GTK_INVALID_LIST_POSITION;
                     gtk_bitset_unref(selected_rows);

                     if (count > 1) {
                       package_details_show_multiple_selected_packages(widgets);
                       return;
                     }
                     if (index == GTK_INVALID_LIST_POSITION) {
                       package_details_cancel_active_load(widgets);
                       package_details_clear_selected_package_state(widgets);
                       package_details_reset_details_view(widgets);
                       return;
                     }

                     GObject *obj = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(self), index));
                     const PackageItem *item = package_item_from_object(obj);
                     if (!item) {
                       if (obj) {
                         g_object_unref(obj);
                       }
                       package_details_clear_selected_package_state(widgets);
                       return;
                     }

                     PackageTableRow selected = package_table_row_from_item(*item);
                     g_object_unref(obj);
                     package_details_load_selected_package_info(widgets, selected);
                   }),
                   widgets);

  gtk_column_view_set_model(view, GTK_SELECTION_MODEL(sel));

  if (have_sort_state) {
    restore_package_view_sort_state(view, sort_kind, sort_order);
  }

  g_signal_connect(view,
                   "activate",
                   G_CALLBACK(+[](GtkColumnView *self, guint position, gpointer user_data) {
                     MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
                     GtkSelectionModel *model = gtk_column_view_get_model(self);
                     if (!model) {
                       return;
                     }

                     GListModel *items_model = G_LIST_MODEL(model);
                     if (!items_model) {
                       return;
                     }

                     GObject *obj = G_OBJECT(g_list_model_get_item(items_model, position));
                     if (!obj) {
                       return;
                     }

                     const PackageItem *item = package_item_from_object(obj);
                     if (!item) {
                       g_object_unref(obj);
                       return;
                     }

                     PackageTableRow row = package_table_row_from_item(*item);
                     PendingTransactionSelectionActions actions =
                         pending_transaction_actions_for_selection(row.row,
                                                                   row.upgrade_target(),
                                                                   row.upgrade_generation(),
                                                                   widgets->query_state.displayed_query,
                                                                   widgets->transaction_state.actions);
                     gtk_selection_model_select_item(model, position, TRUE);
                     g_object_unref(obj);

                     if (actions.can_install) {
                       pending_transaction_on_install_button_clicked(nullptr, widgets);
                     } else if (actions.can_remove &&
                                pending_transaction_activation_should_remove(row.row, actions.rows)) {
                       pending_transaction_on_remove_button_clicked(nullptr, widgets);
                     }
                   }),
                   widgets);

  gtk_scrolled_window_set_child(widgets->results.list_scroller, GTK_WIDGET(view));
  gtk_search_bar_set_key_capture_widget(widgets->results.quick_filter_bar, GTK_WIDGET(view));

  // Update count label
  std::string count_msg = dnfui_i18n_format_count(item_count, "Item: %zu", "Items: %zu");
  gtk_label_set_text(widgets->results.count_label, count_msg.c_str());

  // Restore selection when the same package is still present after a refresh.
  bool restored = false;
  if (!widgets->results.selected_nevra.empty()) {
    GListModel *selected_model = G_LIST_MODEL(sel);
    guint n_items = g_list_model_get_n_items(selected_model);

    for (guint i = 0; i < n_items; ++i) {
      GObject *obj = G_OBJECT(g_list_model_get_item(selected_model, i));
      const PackageRow *row = package_row_from_object(obj);
      bool match = row && row->nevra == widgets->results.selected_nevra;
      g_object_unref(obj);

      if (match) {
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(sel), i, TRUE);
        restored = true;
        break;
      }
    }
  }

  if (!restored) {
    package_details_clear_selected_package_state(widgets);
  }

  g_object_unref(sort_model);
  g_object_unref(sel);
}

// -----------------------------------------------------------------------------
// Package table population
// Builds a virtualized GTK4 ColumnView with structured package metadata.
// Preserves the selected NEVRA across list refreshes when possible.
// -----------------------------------------------------------------------------
void
package_table_fill_package_view(MainWindowUiState *widgets,
                                const std::vector<PackageRow> &items,
                                PackageTableEmptyState empty_state)
{
  reset_quick_filter(widgets);
  if (items.empty()) {
    gtk_scrolled_window_set_child(widgets->results.list_scroller, create_empty_package_view(empty_state));
    gtk_label_set_text(widgets->results.count_label, _("Items: 0"));
    package_details_clear_selected_package_state(widgets);
    return;
  }

  PackageColumnKind sort_kind = PackageColumnKind::STATUS;
  GtkSortType sort_order = GTK_SORT_ASCENDING;
  bool have_sort_state = get_package_view_sort_state(widgets, sort_kind, sort_order);

  // Release the old table before allocating the new row model.
  // Large package lists otherwise keep the old and new row objects alive at the same time.
  gtk_scrolled_window_set_child(widgets->results.list_scroller, nullptr);

  GListStore *store = g_list_store_new(G_TYPE_OBJECT);
  for (const auto &row : items) {
    GObject *obj = make_package_object(widgets, row);
    g_list_store_append(store, obj);
    g_object_unref(obj);
  }

  finish_package_table_view(widgets, store, items.size(), sort_kind, sort_order, have_sort_state);
}

// -----------------------------------------------------------------------------
// Package table population
// Builds a virtualized GTK4 ColumnView with structured package metadata.
// Preserves the selected NEVRA across list refreshes when possible.
// -----------------------------------------------------------------------------
void
package_table_fill_package_view(MainWindowUiState *widgets,
                                const std::vector<PackageTableRow> &items,
                                PackageTableEmptyState empty_state)
{
  reset_quick_filter(widgets);
  if (items.empty()) {
    gtk_scrolled_window_set_child(widgets->results.list_scroller, create_empty_package_view(empty_state));
    gtk_label_set_text(widgets->results.count_label, _("Items: 0"));
    package_details_clear_selected_package_state(widgets);
    return;
  }

  PackageColumnKind sort_kind = PackageColumnKind::STATUS;
  GtkSortType sort_order = GTK_SORT_ASCENDING;
  bool have_sort_state = get_package_view_sort_state(widgets, sort_kind, sort_order);

  // Release the old table before allocating the new row model.
  // Large package lists otherwise keep the old and new row objects alive at the same time.
  gtk_scrolled_window_set_child(widgets->results.list_scroller, nullptr);

  GListStore *store = g_list_store_new(G_TYPE_OBJECT);
  for (const auto &row : items) {
    GObject *obj = make_package_object(widgets, row);
    g_list_store_append(store, obj);
    g_object_unref(obj);
  }

  finish_package_table_view(widgets, store, items.size(), sort_kind, sort_order, have_sort_state);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
