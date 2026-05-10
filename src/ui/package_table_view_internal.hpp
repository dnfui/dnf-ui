// -----------------------------------------------------------------------------
// package_table_view_internal.hpp
// Private helpers for the package table view.
// These declarations are shared only by the package table source files.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <gtk/gtk.h>

#include <string>

struct SearchWidgets;

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

// Package row wrapper used by the sortable GTK model.
struct PackageItem {
  PackageRow row;
  std::string status_text;
  int status_rank;
};

// Per-column data carried by the custom GTK sorter.
struct ColumnSorterData {
  PackageColumnKind kind;
};

GObject *make_package_object(SearchWidgets *widgets, const PackageRow &row);
const PackageItem *package_item_from_object(GObject *obj);
PackageItem *mutable_package_item_from_object(GObject *obj);
const PackageRow *package_row_from_object(GObject *obj);
void fill_package_item_status(SearchWidgets *widgets, PackageItem &item);

std::string column_text(const PackageItem &item, PackageColumnKind kind);
void column_sorter_data_free(gpointer p);
int column_sorter_compare(gconstpointer item1, gconstpointer item2, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
