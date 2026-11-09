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

#include <memory>
#include <string>

struct MainWindowUiState;

struct PackageTableDisplayValues {
  std::string version;
  std::string update_version;
  std::optional<std::uint64_t> install_size;
  std::string release;
  std::string update_release;
  std::string repo;
};

using PackageTableDisplayValuesPtr = std::shared_ptr<const PackageTableDisplayValues>;

struct PackageTableSortEvr {
  bool present = false;
  std::string epoch;
  std::string version;
  std::string release;
};

// Package row wrapper used by the sortable GTK model.
struct PackageItem {
  PackageRow row;
  DaemonUpgradeRowContextPtr daemon_upgrade;
  PackageTableDisplayValuesPtr display_values;
  PackageTableSortEvr current_evr;
  PackageTableSortEvr update_evr;
  std::string status_text;
  int status_rank = 0;

  const DaemonUpgradeTarget *upgrade_target() const
  {
    return daemon_upgrade ? &daemon_upgrade->target : nullptr;
  }
};

GObject *make_package_object(MainWindowUiState *widgets, const PackageRow &row);
GObject *make_package_object(MainWindowUiState *widgets, const PackageTableRow &row);
const PackageItem *package_item_from_object(GObject *obj);
bool package_table_quick_filter_matches(const PackageRow &row, const char *folded_text);
PackageItem *mutable_package_item_from_object(GObject *obj);
const PackageRow *package_row_from_object(GObject *obj);
PackageTableRow package_table_row_from_item(const PackageItem &item);
void package_table_fill_item_status(MainWindowUiState *widgets,
                                    PackageItem &item,
                                    const InstalledPackageResolution &resolution);
void package_table_fill_item_display_values(PackageItem &item,
                                            const InstalledPackageResolution &resolution,
                                            bool compact_view);

std::string package_table_column_text(const PackageItem &item, PackageColumnKind kind);
int package_table_column_sorter_compare(gconstpointer item1, gconstpointer item2, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
