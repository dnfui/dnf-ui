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

static void update_pending_action_css_for_cell(GtkWidget *cell, MainWindowUiState *widgets, const PackageRow &row);

// -----------------------------------------------------------------------------
// Refresh stored package status values without changing the GTK model.
// -----------------------------------------------------------------------------
static void
refresh_model_status_values(GtkColumnView *view, MainWindowUiState *widgets)
{
  GtkSelectionModel *model = gtk_column_view_get_model(view);
  if (!model || !GTK_IS_SINGLE_SELECTION(model)) {
    return;
  }

  GtkSingleSelection *selection = GTK_SINGLE_SELECTION(model);
  GListModel *items_model = gtk_single_selection_get_model(selection);
  if (!items_model) {
    return;
  }

  guint n_items = g_list_model_get_n_items(items_model);
  for (guint i = 0; i < n_items; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(items_model, i));
    PackageItem *item = mutable_package_item_from_object(obj);
    if (item) {
      package_table_fill_item_status(widgets, *item);
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

  PackageRow *row = static_cast<PackageRow *>(g_object_get_data(G_OBJECT(widget), "package-context-row"));
  if (row) {
    update_pending_action_css_for_cell(widget, widgets, *row);
    if (g_object_get_data(G_OBJECT(widget), "package-status-cell")) {
      package_table_update_status_label(widget, widgets, *row);
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
                                         "Ctrl+H: Toggle history panel\n"
                                         "Ctrl+Shift+H: Open transaction history\n"
                                         "Ctrl+I: Toggle package info panel\n"
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
pending_row_css_class(MainWindowUiState *widgets, const PackageRow &row)
{
  const char *status_class = package_table_pending_action_css_class(widgets, row);
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
update_pending_action_css_for_cell(GtkWidget *cell, MainWindowUiState *widgets, const PackageRow &row)
{
  const char *pending_class = pending_row_css_class(widgets, row);
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
  if (!model || !GTK_IS_SINGLE_SELECTION(model)) {
    return false;
  }

  GtkSingleSelection *selection = GTK_SINGLE_SELECTION(model);
  GListModel *items_model = gtk_single_selection_get_model(selection);
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
      gtk_single_selection_set_selected(selection, i);
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

  g_signal_connect(factory,
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
                         kind == PackageColumnKind::ARCH || kind == PackageColumnKind::REPO) {
                       gtk_widget_add_css_class(label, "package-meta");
                     }
                     if (kind == PackageColumnKind::SUMMARY) {
                       gtk_widget_add_css_class(label, "package-summary");
                     }

                     // Right-click opens the same package actions as the main buttons.
                     GtkGesture *context_click = gtk_gesture_click_new();
                     gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context_click), GDK_BUTTON_SECONDARY);
                     g_signal_connect(
                         context_click,
                         "pressed",
                         G_CALLBACK(+[](GtkGestureClick *gesture, int, double x, double y, gpointer user_data) {
                           MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
                           GtkWidget *cell = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
                           PackageRow *row =
                               static_cast<PackageRow *>(g_object_get_data(G_OBJECT(cell), "package-context-row"));
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
                     GObject *obj = G_OBJECT(gtk_list_item_get_item(item));
                     const PackageItem *package_item = package_item_from_object(obj);

                     if (!package_item) {
                       gtk_label_set_text(GTK_LABEL(label), "");
                       gtk_widget_set_tooltip_text(frame, nullptr);
                       package_table_clear_status_css(cell);
                       package_table_clear_pending_action_css(frame);
                       clear_pending_row_css_for_cell(frame);
                       g_object_set_data_full(G_OBJECT(frame), "package-context-row", nullptr, nullptr);
                       return;
                     }

                     // Store the package row currently bound to this reused table cell.
                     g_object_set_data_full(
                         G_OBJECT(frame), "package-context-row", new PackageRow(package_item->row), +[](gpointer p) {
                           delete static_cast<PackageRow *>(p);
                         });

                     update_pending_action_css_for_cell(frame, widgets, package_item->row);
                     if (kind == PackageColumnKind::STATUS) {
                       package_table_update_status_label(cell, widgets, package_item->row);
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

  GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(package_table_column_sorter_compare,
                                                       new ColumnSorterData { definition.kind },
                                                       package_table_column_sorter_data_free));
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
// Return the selected package row from the current package table.
// -----------------------------------------------------------------------------
bool
package_table_get_selected_package_row(MainWindowUiState *widgets, PackageRow &out_pkg)
{
  if (!widgets || !widgets->results.list_scroller) {
    return false;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return false;
  }

  GtkSelectionModel *model = gtk_column_view_get_model(GTK_COLUMN_VIEW(child));
  if (!model || !GTK_IS_SINGLE_SELECTION(model)) {
    return false;
  }

  GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
  guint index = gtk_single_selection_get_selected(sel);
  if (index == GTK_INVALID_LIST_POSITION) {
    return false;
  }

  GObject *obj = G_OBJECT(g_list_model_get_item(gtk_single_selection_get_model(sel), index));
  if (!obj) {
    return false;
  }

  const PackageRow *row = package_row_from_object(obj);
  bool ok = row != nullptr;
  if (ok) {
    out_pkg = *row;
  }

  g_object_unref(obj);
  return ok;
}

// -----------------------------------------------------------------------------
// Return all package rows currently displayed in the package table.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
package_table_get_displayed_package_rows(MainWindowUiState *widgets)
{
  std::vector<PackageRow> rows;
  if (!widgets || !widgets->results.list_scroller) {
    return rows;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return rows;
  }

  GtkSelectionModel *model = gtk_column_view_get_model(GTK_COLUMN_VIEW(child));
  if (!model || !GTK_IS_SINGLE_SELECTION(model)) {
    return rows;
  }

  GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
  GListModel *items_model = gtk_single_selection_get_model(sel);
  if (!items_model) {
    return rows;
  }

  guint n_items = g_list_model_get_n_items(items_model);
  rows.reserve(n_items);
  for (guint i = 0; i < n_items; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(items_model, i));
    const PackageRow *row = package_row_from_object(obj);
    if (row) {
      rows.push_back(*row);
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
  if (items.empty()) {
    gtk_scrolled_window_set_child(widgets->results.list_scroller, create_empty_package_view(empty_state));
    widgets->results.listbox = nullptr;
    gtk_label_set_text(widgets->results.count_label, _("Items: 0"));
    package_details_clear_selected_package_state(widgets);
    return;
  }

  PackageColumnKind sort_kind = PackageColumnKind::STATUS;
  GtkSortType sort_order = GTK_SORT_ASCENDING;
  bool have_sort_state = get_package_view_sort_state(widgets, sort_kind, sort_order);

  GListStore *store = g_list_store_new(G_TYPE_OBJECT);
  for (const auto &row : items) {
    GObject *obj = make_package_object(widgets, row);
    g_list_store_append(store, obj);
    g_object_unref(obj);
  }

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

  // Wrap the package list in a GTK sort model so column header clicks reorder it.
  GtkSortListModel *sort_model = gtk_sort_list_model_new(nullptr, nullptr);
  gtk_sort_list_model_set_model(sort_model, G_LIST_MODEL(store));
  gtk_sort_list_model_set_sorter(sort_model, gtk_column_view_get_sorter(view));
  g_object_unref(store);

  GtkSingleSelection *sel = gtk_single_selection_new(nullptr);
  gtk_single_selection_set_autoselect(sel, FALSE);
  gtk_single_selection_set_can_unselect(sel, TRUE);
  gtk_single_selection_set_model(sel, G_LIST_MODEL(sort_model));
  gtk_single_selection_set_selected(sel, GTK_INVALID_LIST_POSITION);

  g_signal_connect(sel,
                   "selection-changed",
                   G_CALLBACK(+[](GtkSingleSelection *self, guint, guint, gpointer user_data) {
                     MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
                     guint index = gtk_single_selection_get_selected(self);

                     if (index == GTK_INVALID_LIST_POSITION) {
                       package_details_clear_selected_package_state(widgets);
                       return;
                     }

                     GObject *obj = G_OBJECT(g_list_model_get_item(gtk_single_selection_get_model(self), index));
                     const PackageRow *row = package_row_from_object(obj);
                     if (!row) {
                       g_object_unref(obj);
                       package_details_clear_selected_package_state(widgets);
                       return;
                     }

                     PackageRow selected = *row;
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
                     if (!model || !GTK_IS_SINGLE_SELECTION(model)) {
                       return;
                     }

                     GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
                     GListModel *items_model = gtk_single_selection_get_model(sel);
                     if (!items_model) {
                       return;
                     }

                     GObject *obj = G_OBJECT(g_list_model_get_item(items_model, position));
                     if (!obj) {
                       return;
                     }

                     const PackageRow *row = package_row_from_object(obj);
                     if (!row) {
                       g_object_unref(obj);
                       return;
                     }

                     PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(*row);
                     gtk_single_selection_set_selected(sel, position);
                     g_object_unref(obj);

                     if (action_rows.has_install_row) {
                       pending_transaction_on_install_button_clicked(nullptr, widgets);
                     } else if (dnf_backend_is_package_installed_exact(*row)) {
                       pending_transaction_on_remove_button_clicked(nullptr, widgets);
                     }
                   }),
                   widgets);

  gtk_scrolled_window_set_child(widgets->results.list_scroller, GTK_WIDGET(view));
  widgets->results.listbox = nullptr;

  // Update count label
  std::string count_msg = dnfui_i18n_format_count(items.size(), "Item: %zu", "Items: %zu");
  gtk_label_set_text(widgets->results.count_label, count_msg.c_str());

  // Restore selection when the same package is still present after a refresh.
  bool restored = false;
  if (!widgets->results.selected_nevra.empty()) {
    GListModel *selected_model = gtk_single_selection_get_model(sel);
    guint n_items = g_list_model_get_n_items(selected_model);

    for (guint i = 0; i < n_items; ++i) {
      GObject *obj = G_OBJECT(g_list_model_get_item(selected_model, i));
      const PackageRow *row = package_row_from_object(obj);
      bool match = row && row->nevra == widgets->results.selected_nevra;
      g_object_unref(obj);

      if (match) {
        gtk_single_selection_set_selected(sel, i);
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
// EOF
// -----------------------------------------------------------------------------
