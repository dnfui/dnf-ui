// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_export.cpp
// Package table export helpers
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_export.hpp"

#include "i18n.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "ui/package_table/package_table_columns.hpp"
#include "ui/package_table/package_table_export_csv.hpp"
#include "ui/package_table/package_table_view_internal.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

struct ExportDialogData {
  std::shared_ptr<MainWindowUiState> widgets;
  std::string csv;
};

// -----------------------------------------------------------------------------
// Return visible columns in table order.
// -----------------------------------------------------------------------------
std::vector<const PackageTableColumnDefinition *>
visible_export_columns()
{
  std::set<std::string> visible = package_table_load_visible_column_ids();
  std::vector<const PackageTableColumnDefinition *> columns;

  for (const auto &column : package_table_column_definitions()) {
    if (visible.count(column.id) > 0) {
      columns.push_back(&column);
    }
  }

  return columns;
}

// -----------------------------------------------------------------------------
// Build CSV text from the current GTK table model.
// -----------------------------------------------------------------------------
bool
build_visible_table_csv(MainWindowUiState *widgets, std::string &csv_out)
{
  csv_out.clear();
  if (!widgets || !widgets->results.list_scroller) {
    return false;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return false;
  }

  GtkSelectionModel *selection_model = gtk_column_view_get_model(GTK_COLUMN_VIEW(child));
  if (!selection_model) {
    return false;
  }

  GListModel *model = G_LIST_MODEL(selection_model);
  guint count = g_list_model_get_n_items(model);
  if (count == 0) {
    return false;
  }

  std::vector<const PackageTableColumnDefinition *> columns = visible_export_columns();
  if (columns.empty()) {
    return false;
  }

  std::vector<std::string> headers;
  headers.reserve(columns.size());
  for (const auto *column : columns) {
    headers.push_back(_(column->title));
  }

  std::vector<std::vector<std::string>> rows;
  rows.reserve(count);
  for (guint i = 0; i < count; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(model, i));
    const PackageItem *item = package_item_from_object(obj);
    if (!item) {
      g_object_unref(obj);
      continue;
    }

    std::vector<std::string> row;
    row.reserve(columns.size());
    for (const auto *column : columns) {
      row.push_back(package_table_column_text(*item, column->kind));
    }
    rows.push_back(std::move(row));
    g_object_unref(obj);
  }

  if (rows.empty()) {
    return false;
  }

  csv_out = package_table_export_csv_text(headers, rows);
  return true;
}

// -----------------------------------------------------------------------------
// Save the prepared CSV text to the selected file.
// -----------------------------------------------------------------------------
void
on_export_dialog_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
  auto *data = static_cast<ExportDialogData *>(user_data);
  MainWindowUiState *widgets = data && data->widgets ? data->widgets.get() : nullptr;

  if (data && widgets) {
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!file) {
      if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        ui_helpers_set_status(widgets->query.status_label, error->message, "red");
      }
      if (error) {
        g_error_free(error);
      }
      delete data;
      return;
    }

    gboolean saved = g_file_replace_contents(
        file, data->csv.c_str(), data->csv.size(), nullptr, FALSE, G_FILE_CREATE_NONE, nullptr, nullptr, &error);
    if (saved) {
      ui_helpers_set_status(widgets->query.status_label, _("Package list exported."), "green");
    } else {
      ui_helpers_set_status(
          widgets->query.status_label, error ? error->message : _("Could not export package list."), "red");
    }

    if (error) {
      g_error_free(error);
    }
    if (file) {
      g_object_unref(file);
    }
  }

  delete data;
}

}

// -----------------------------------------------------------------------------
// Export the current package table to a user-selected CSV file.
// -----------------------------------------------------------------------------
void
package_table_export_visible_rows_to_csv(MainWindowUiState *widgets, GtkWindow *parent)
{
  std::string csv;
  if (!build_visible_table_csv(widgets, csv)) {
    if (widgets) {
      ui_helpers_set_status(widgets->query.status_label, _("No package rows to export."), "blue");
    }
    return;
  }

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, _("Export Package List"));
  gtk_file_dialog_set_initial_name(dialog, "dnf-ui-package-list.csv");

  auto *data = new ExportDialogData { widgets ? widgets->shared_from_this() : nullptr, std::move(csv) };
  gtk_file_dialog_save(dialog, parent, nullptr, on_export_dialog_response, data);
  g_object_unref(dialog);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
