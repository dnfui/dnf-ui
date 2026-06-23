// -----------------------------------------------------------------------------
// package_table_columns.hpp
// Package table column metadata and saved visibility settings.
// These declarations are shared only by the package table and menu code.
// -----------------------------------------------------------------------------
#pragma once

#include <set>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Package table columns.
// -----------------------------------------------------------------------------
enum class PackageColumnKind {
  STATUS,
  PACKAGE,
  VERSION,
  UPDATE_VERSION,
  RELEASE,
  UPDATE_RELEASE,
  ARCH,
  REPO,
  SUMMARY,
};

struct PackageTableColumnDefinition {
  PackageColumnKind kind;
  const char *id;
  const char *title;
  int fixed_width;
  bool expand;
  bool default_visible;
};

struct PackageTableColumnInfo {
  const char *id;
  const char *title;
};

// -----------------------------------------------------------------------------
// Return the package table column definitions.
// -----------------------------------------------------------------------------
const std::vector<PackageTableColumnDefinition> &package_table_column_definitions();
// -----------------------------------------------------------------------------
// Return the table column definition for one persistent column id.
// -----------------------------------------------------------------------------
const PackageTableColumnDefinition *package_table_column_definition_by_id(const char *column_id);
// -----------------------------------------------------------------------------
// Read visible package table columns from the user config.
// -----------------------------------------------------------------------------
std::set<std::string> package_table_load_visible_column_ids();
// -----------------------------------------------------------------------------
// Save visible package table columns to the user config.
// -----------------------------------------------------------------------------
void package_table_save_visible_column_ids(const std::set<std::string> &visible);
// -----------------------------------------------------------------------------
// Return the package table columns exposed to the View menu.
// -----------------------------------------------------------------------------
std::vector<PackageTableColumnInfo> package_table_column_infos();
// -----------------------------------------------------------------------------
// Return whether one package table column is enabled in user settings.
// -----------------------------------------------------------------------------
bool package_table_column_is_visible(const char *column_id);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
