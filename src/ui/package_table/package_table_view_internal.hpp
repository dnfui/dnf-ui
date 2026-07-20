// -----------------------------------------------------------------------------
// package_table_view_internal.hpp
// Private helpers for the package table view.
// These declarations are shared only by the package table source files.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"
#include "ui/package_table/package_table_columns.hpp"
#include "ui/package_table/package_table_view.hpp"

#include <gtk/gtk.h>

#include <string>

struct MainWindowUiState;

// Package row wrapper used by the sortable GTK model.
struct PackageItem {
  PackageRow row;
  DaemonUpgradeRowContextPtr daemon_upgrade;
  std::string status_text;
  int status_rank;

  const TransactionServiceUpgradeTarget *upgrade_target() const
  {
    return daemon_upgrade ? &daemon_upgrade->target : nullptr;
  }
};

GObject *make_package_object(MainWindowUiState *widgets, const PackageRow &row);
GObject *make_package_object(MainWindowUiState *widgets, const PackageTableRow &row);
const PackageItem *package_item_from_object(GObject *obj);
PackageItem *mutable_package_item_from_object(GObject *obj);
const PackageRow *package_row_from_object(GObject *obj);
PackageTableRow package_table_row_from_item(const PackageItem &item);
void package_table_fill_item_status(MainWindowUiState *widgets, PackageItem &item);

std::string package_table_column_text(const PackageItem &item, PackageColumnKind kind);
int package_table_column_sorter_compare(gconstpointer item1, gconstpointer item2, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
