// -----------------------------------------------------------------------------
// package_table_view.cpp
// Package table view
// Builds the GTK4 ColumnView and keeps package selection wired into the details
// panel controller.
// -----------------------------------------------------------------------------
#include "ui_helpers.hpp"

#include "i18n.hpp"
#include "package_info_controller.hpp"
#include "package_table_context_menu.hpp"
#include "package_table_status.hpp"
#include "package_table_view.hpp"
#include "package_table_view_internal.hpp"
#include "pending_transaction_controller.hpp"
#include "widgets.hpp"

#include <string>

// -----------------------------------------------------------------------------
// Refresh stored package status values without changing the GTK model.
// -----------------------------------------------------------------------------
static void
refresh_model_status_values(GtkColumnView *view, SearchWidgets *widgets)
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
// Refresh the status cells that are currently realized by the virtualized view.
// -----------------------------------------------------------------------------
static void
refresh_visible_status_labels(GtkWidget *widget, SearchWidgets *widgets)
{
  if (!widget) {
    return;
  }

  if (GTK_IS_LABEL(widget) && g_object_get_data(G_OBJECT(widget), "package-status-cell")) {
    PackageRow *row = static_cast<PackageRow *>(g_object_get_data(G_OBJECT(widget), "package-context-row"));
    if (row) {
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
create_empty_package_view()
{
  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(outer, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(outer, TRUE);
  gtk_widget_set_vexpand(outer, TRUE);
  gtk_widget_add_css_class(outer, "package-empty-state");

  GtkWidget *title = gtk_label_new(nullptr);
  gchar *title_markup = g_markup_printf_escaped("<b>%s</b>", _("No packages to show"));
  gtk_label_set_markup(GTK_LABEL(title), title_markup);
  g_free(title_markup);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "package-empty-title");
  gtk_box_append(GTK_BOX(outer), title);

  GtkWidget *message = gtk_label_new(_("Search for packages or choose List Packages."));
  gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_widget_add_css_class(message, "package-empty-message");
  gtk_box_append(GTK_BOX(outer), message);

  GtkWidget *shortcuts = gtk_label_new(_("Shortcuts\n"
                                         "Ctrl+F: Focus search\n"
                                         "Ctrl+H: Toggle history panel\n"
                                         "Ctrl+I: Toggle package info panel\n"
                                         "Ctrl+Q or Ctrl+W: Quit"));
  gtk_label_set_xalign(GTK_LABEL(shortcuts), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(shortcuts), TRUE);
  gtk_widget_add_css_class(shortcuts, "package-empty-shortcuts");
  gtk_box_append(GTK_BOX(outer), shortcuts);

  return outer;
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
// Build one text column for the package table.
// -----------------------------------------------------------------------------
static GtkColumnViewColumn *
create_text_column(SearchWidgets *widgets, const char *title, PackageColumnKind kind, int fixed_width, bool expand)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_object_set_data(G_OBJECT(factory), "package-column-kind", GINT_TO_POINTER(static_cast<int>(kind)));
  g_object_set_data(G_OBJECT(factory), "package-table-widgets", widgets);

  g_signal_connect(
      factory,
      "setup",
      G_CALLBACK(+[](GtkSignalListItemFactory *factory, GtkListItem *item, gpointer) {
        PackageColumnKind kind = static_cast<PackageColumnKind>(
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(factory), "package-column-kind")));

        GtkWidget *label = gtk_label_new(nullptr);
        gtk_widget_set_margin_start(label, 6);
        gtk_widget_set_margin_end(label, 6);
        gtk_widget_set_margin_top(label, 4);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_list_item_set_activatable(item, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(label), kind == PackageColumnKind::STATUS ? 0.5f : 0.0f);

        if (kind == PackageColumnKind::STATUS) {
          gtk_widget_add_css_class(label, "package-status");
          // Mark status cells so local status refreshes can update visible rows.
          g_object_set_data(G_OBJECT(label), "package-status-cell", GINT_TO_POINTER(1));
        }
        if (kind == PackageColumnKind::VERSION || kind == PackageColumnKind::ARCH || kind == PackageColumnKind::REPO) {
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
              SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
              GtkWidget *label = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
              PackageRow *row = static_cast<PackageRow *>(g_object_get_data(G_OBJECT(label), "package-context-row"));
              if (!row) {
                return;
              }

              GtkWidget *view = gtk_widget_get_ancestor(label, GTK_TYPE_COLUMN_VIEW);
              if (!view || !GTK_IS_COLUMN_VIEW(view)) {
                return;
              }

              package_table_show_context_menu(label, widgets, *row, x, y, [view](const std::string &nevra) {
                return select_package_table_row(GTK_COLUMN_VIEW(view), nevra);
              });
            }),
            g_object_get_data(G_OBJECT(factory), "package-table-widgets"));
        gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(context_click));

        gtk_list_item_set_child(item, label);
      }),
      nullptr);

  g_signal_connect(factory,
                   "bind",
                   G_CALLBACK(+[](GtkSignalListItemFactory *factory, GtkListItem *item, gpointer) {
                     SearchWidgets *widgets =
                         static_cast<SearchWidgets *>(g_object_get_data(G_OBJECT(factory), "package-table-widgets"));
                     PackageColumnKind kind = static_cast<PackageColumnKind>(
                         GPOINTER_TO_INT(g_object_get_data(G_OBJECT(factory), "package-column-kind")));

                     GtkWidget *label = gtk_list_item_get_child(item);
                     GObject *obj = G_OBJECT(gtk_list_item_get_item(item));
                     const PackageItem *package_item = package_item_from_object(obj);

                     if (!package_item) {
                       gtk_label_set_text(GTK_LABEL(label), "");
                       gtk_widget_set_tooltip_text(label, nullptr);
                       package_table_clear_status_css(label);
                       g_object_set_data_full(G_OBJECT(label), "package-context-row", nullptr, nullptr);
                       return;
                     }

                     // Store the package row currently bound to this reused table cell.
                     g_object_set_data_full(
                         G_OBJECT(label), "package-context-row", new PackageRow(package_item->row), +[](gpointer p) {
                           delete static_cast<PackageRow *>(p);
                         });

                     if (kind == PackageColumnKind::STATUS) {
                       package_table_update_status_label(label, widgets, package_item->row);
                     } else {
                       std::string text = package_table_column_text(*package_item, kind);
                       gtk_label_set_text(GTK_LABEL(label), text.c_str());
                     }
                   }),
                   nullptr);

  GtkColumnViewColumn *column = gtk_column_view_column_new(title, nullptr);
  gtk_column_view_column_set_factory(column, factory);
  g_object_unref(factory);

  g_object_set_data(G_OBJECT(column), "package-column-kind", GINT_TO_POINTER(static_cast<int>(kind)));
  gtk_column_view_column_set_resizable(column, TRUE);
  gtk_column_view_column_set_expand(column, expand);

  GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(
      package_table_column_sorter_compare, new ColumnSorterData { kind }, package_table_column_sorter_data_free));
  gtk_column_view_column_set_sorter(column, sorter);
  g_object_unref(sorter);

  if (fixed_width > 0) {
    gtk_column_view_column_set_fixed_width(column, fixed_width);
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
get_package_view_sort_state(SearchWidgets *widgets, PackageColumnKind &out_kind, GtkSortType &out_order)
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
  if (!column) {
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

    if (column_kind == kind) {
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
package_table_get_selected_package_row(SearchWidgets *widgets, PackageRow &out_pkg)
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
// Refresh package status text and colors without rebuilding the package table.
// -----------------------------------------------------------------------------
void
package_table_refresh_statuses(SearchWidgets *widgets)
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
// Package table population
// Builds a virtualized GTK4 ColumnView with structured package metadata.
// Preserves the selected NEVRA across list refreshes when possible.
// -----------------------------------------------------------------------------
void
package_table_fill_package_view(SearchWidgets *widgets, const std::vector<PackageRow> &items)
{
  if (items.empty()) {
    gtk_scrolled_window_set_child(widgets->results.list_scroller, create_empty_package_view());
    widgets->results.listbox = nullptr;
    gtk_label_set_text(widgets->results.count_label, _("Items: 0"));
    package_info_clear_selected_package_state(widgets);
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
  gtk_widget_set_hexpand(GTK_WIDGET(view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(view), TRUE);
  gtk_column_view_set_single_click_activate(view, FALSE);
  gtk_column_view_set_show_row_separators(view, TRUE);
  gtk_column_view_set_show_column_separators(view, FALSE);

  append_package_column(view, create_text_column(widgets, _("Status"), PackageColumnKind::STATUS, 160, FALSE));
  append_package_column(view, create_text_column(widgets, _("Package"), PackageColumnKind::PACKAGE, 180, FALSE));
  append_package_column(view, create_text_column(widgets, _("Version"), PackageColumnKind::VERSION, 150, FALSE));
  append_package_column(view, create_text_column(widgets, _("Arch"), PackageColumnKind::ARCH, 95, FALSE));
  append_package_column(view, create_text_column(widgets, _("Repo"), PackageColumnKind::REPO, 130, FALSE));
  append_package_column(view, create_text_column(widgets, _("Summary"), PackageColumnKind::SUMMARY, 0, TRUE));

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
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     guint index = gtk_single_selection_get_selected(self);

                     if (index == GTK_INVALID_LIST_POSITION) {
                       package_info_clear_selected_package_state(widgets);
                       return;
                     }

                     GObject *obj = G_OBJECT(g_list_model_get_item(gtk_single_selection_get_model(self), index));
                     const PackageRow *row = package_row_from_object(obj);
                     if (!row) {
                       g_object_unref(obj);
                       package_info_clear_selected_package_state(widgets);
                       return;
                     }

                     PackageRow selected = *row;
                     g_object_unref(obj);
                     package_info_load_selected_package_info(widgets, selected);
                   }),
                   widgets);

  gtk_column_view_set_model(view, GTK_SELECTION_MODEL(sel));

  if (have_sort_state) {
    restore_package_view_sort_state(view, sort_kind, sort_order);
  }

  g_signal_connect(view,
                   "activate",
                   G_CALLBACK(+[](GtkColumnView *self, guint position, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
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

                     // Double-click toggles install vs remove based on whether
                     // this exact row already exists in the installed snapshot.
                     bool installed_exact = dnf_backend_is_package_installed_exact(*row);
                     gtk_single_selection_set_selected(sel, position);
                     g_object_unref(obj);

                     if (installed_exact) {
                       pending_transaction_on_remove_button_clicked(nullptr, widgets);
                     } else {
                       pending_transaction_on_install_button_clicked(nullptr, widgets);
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
    package_info_clear_selected_package_state(widgets);
  }

  g_object_unref(sort_model);
  g_object_unref(sel);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
