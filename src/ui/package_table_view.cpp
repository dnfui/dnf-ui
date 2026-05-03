// -----------------------------------------------------------------------------
// src/ui/package_table_view.cpp
// Package table view
// Builds the GTK4 ColumnView, maintains sortable package-row wrappers, and
// keeps package selection wired into the details notebook controller.
// -----------------------------------------------------------------------------
#include "ui_helpers.hpp"

#include "i18n.hpp"
#include "package_info_controller.hpp"
#include "package_table_context_menu.hpp"
#include "package_table_status.hpp"
#include "package_table_view.hpp"
#include "pending_transaction_controller.hpp"
#include "widgets.hpp"

#include <string>

// -----------------------------------------------------------------------------
// Column model helpers
// -----------------------------------------------------------------------------
enum class PackageColumnKind {
  STATUS,
  PACKAGE,
  VERSION,
  ARCH,
  REPO,
  SUMMARY,
};

// -----------------------------------------------------------------------------
// Return the private key used to store package rows on GTK objects.
// -----------------------------------------------------------------------------
static GQuark
package_row_quark()
{
  static GQuark q = 0;
  if (G_UNLIKELY(q == 0)) {
    q = g_quark_from_static_string("package-table-row");
  }

  return q;
}

// Package row wrapper used by the sortable GTK model.
struct PackageItem {
  PackageRow row;
  std::string status_text;
  int status_rank;
};

// -----------------------------------------------------------------------------
// Snapshot the visible status text and its sort order for one package row.
// -----------------------------------------------------------------------------
static void
fill_package_item_status(SearchWidgets *widgets, PackageItem &item)
{
  // Keep Status sorting tied to the stable package state so marking a pending
  // action does not move the row away from the user in the current view.
  PackageInstallState install_state = dnf_backend_get_package_install_state(item.row);
  item.status_rank = package_table_status_rank(install_state);

  for (const auto &a : widgets->transaction.actions) {
    if (a.nevra == item.row.nevra) {
      switch (a.type) {
      case PendingAction::INSTALL:
        item.status_text = _("Pending Install");
        break;
      case PendingAction::REINSTALL:
        item.status_text = _("Pending Reinstall");
        break;
      case PendingAction::REMOVE:
        item.status_text = _("Pending Removal");
        break;
      }
      return;
    }
  }

  item.status_text = package_table_status_text(install_state);
}

// -----------------------------------------------------------------------------
// Package row object helpers
// -----------------------------------------------------------------------------
// Wrap one package row in a GObject so GTK list models can sort and select it.
// -----------------------------------------------------------------------------
static GObject *
make_package_object(SearchWidgets *widgets, const PackageRow &row)
{
  GObject *obj = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
  auto *item = new PackageItem { row, {}, 0 };
  fill_package_item_status(widgets, *item);
  g_object_set_qdata_full(obj, package_row_quark(), item, +[](gpointer p) { delete static_cast<PackageItem *>(p); });
  return obj;
}

// -----------------------------------------------------------------------------
// Read the sortable package wrapper stored on a GTK list item.
// -----------------------------------------------------------------------------
static const PackageItem *
package_item_from_object(GObject *obj)
{
  if (!obj) {
    return nullptr;
  }

  return static_cast<const PackageItem *>(g_object_get_qdata(obj, package_row_quark()));
}

// -----------------------------------------------------------------------------
// Read the mutable package wrapper stored on a GTK list item.
// -----------------------------------------------------------------------------
static PackageItem *
mutable_package_item_from_object(GObject *obj)
{
  if (!obj) {
    return nullptr;
  }

  return static_cast<PackageItem *>(g_object_get_qdata(obj, package_row_quark()));
}

// -----------------------------------------------------------------------------
// Map a package wrapper back to the package row used elsewhere in the UI.
// -----------------------------------------------------------------------------
static const PackageRow *
package_row_from_object(GObject *obj)
{
  const PackageItem *item = package_item_from_object(obj);
  if (!item) {
    return nullptr;
  }

  return &item->row;
}

// -----------------------------------------------------------------------------
// Column sorter helpers
// -----------------------------------------------------------------------------
// Return the visible text for one package table cell.
// -----------------------------------------------------------------------------
static std::string
column_text(const PackageItem &item, PackageColumnKind kind)
{
  switch (kind) {
  case PackageColumnKind::STATUS:
    return item.status_text;
  case PackageColumnKind::PACKAGE:
    return item.row.name;
  case PackageColumnKind::VERSION:
    return item.row.display_version();
  case PackageColumnKind::ARCH:
    return item.row.arch;
  case PackageColumnKind::REPO:
    return item.row.repo;
  case PackageColumnKind::SUMMARY:
    return item.row.summary;
  }

  return {};
}

// Per-column data carried by the custom GTK sorter.
struct ColumnSorterData {
  PackageColumnKind kind;
};

// -----------------------------------------------------------------------------
// Free data owned by one package table column sorter.
// -----------------------------------------------------------------------------
static void
column_sorter_data_free(gpointer p)
{
  delete static_cast<ColumnSorterData *>(p);
}

// -----------------------------------------------------------------------------
// Compare two strings case-insensitively while keeping a stable fallback order.
// -----------------------------------------------------------------------------
static int
compare_text(const std::string &lhs, const std::string &rhs)
{
  char *lhs_folded = g_utf8_casefold(lhs.c_str(), -1);
  char *rhs_folded = g_utf8_casefold(rhs.c_str(), -1);

  int result = g_utf8_collate(lhs_folded, rhs_folded);
  if (result == 0) {
    result = g_utf8_collate(lhs.c_str(), rhs.c_str());
  }

  g_free(lhs_folded);
  g_free(rhs_folded);
  return result;
}

// -----------------------------------------------------------------------------
// Compare two package items for the active package table column.
// -----------------------------------------------------------------------------
static int
compare_package_items(const PackageItem &lhs, const PackageItem &rhs, PackageColumnKind kind)
{
  int result = 0;

  switch (kind) {
  case PackageColumnKind::STATUS:
    result = lhs.status_rank - rhs.status_rank;
    break;
  case PackageColumnKind::PACKAGE:
    result = compare_text(lhs.row.name, rhs.row.name);
    break;
  case PackageColumnKind::VERSION:
    result = compare_text(lhs.row.display_version(), rhs.row.display_version());
    break;
  case PackageColumnKind::ARCH:
    result = compare_text(lhs.row.arch, rhs.row.arch);
    break;
  case PackageColumnKind::REPO:
    result = compare_text(lhs.row.repo, rhs.row.repo);
    break;
  case PackageColumnKind::SUMMARY:
    result = compare_text(lhs.row.summary, rhs.row.summary);
    break;
  }

  if (result != 0) {
    return result;
  }

  result = compare_text(lhs.row.name, rhs.row.name);
  if (result != 0) {
    return result;
  }

  return compare_text(lhs.row.nevra, rhs.row.nevra);
}

// -----------------------------------------------------------------------------
// Adapter from GTK's custom sorter callback to the package item comparator.
// -----------------------------------------------------------------------------
static int
column_sorter_compare(gconstpointer item1, gconstpointer item2, gpointer user_data)
{
  const auto *data = static_cast<const ColumnSorterData *>(user_data);
  const PackageItem *lhs = package_item_from_object(G_OBJECT(const_cast<gpointer>(item1)));
  const PackageItem *rhs = package_item_from_object(G_OBJECT(const_cast<gpointer>(item2)));
  if (!data || !lhs || !rhs) {
    return 0;
  }

  return compare_package_items(*lhs, *rhs, data->kind);
}

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
      fill_package_item_status(widgets, *item);
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
// Select the package row that owns the context menu action.
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
                       std::string text = column_text(*package_item, kind);
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

  GtkSorter *sorter =
      GTK_SORTER(gtk_custom_sorter_new(column_sorter_compare, new ColumnSorterData { kind }, column_sorter_data_free));
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
// Builds a virtualized GTK4 ColumnView with structured package metadata while
// preserving the selected NEVRA across list refreshes when possible.
// -----------------------------------------------------------------------------
void
package_table_fill_package_view(SearchWidgets *widgets, const std::vector<PackageRow> &items)
{
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
  gtk_column_view_set_show_column_separators(view, TRUE);

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
