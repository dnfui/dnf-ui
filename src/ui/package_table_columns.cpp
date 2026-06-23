// -----------------------------------------------------------------------------
// package_table_columns.cpp
// Package table column metadata and saved visibility settings.
// -----------------------------------------------------------------------------
#include "package_table_columns.hpp"

#include "config.hpp"
#include "i18n.hpp"

constexpr const char *kPackageTableHiddenColumnsConfigKey = "package_table_hidden_columns";
constexpr const char *kPackageTableLegacyColumnsConfigKey = "package_table_columns";

// -----------------------------------------------------------------------------
// Return the package table column definitions.
// -----------------------------------------------------------------------------
const std::vector<PackageTableColumnDefinition> &
package_table_column_definitions()
{
  static const std::vector<PackageTableColumnDefinition> columns = {
    { PackageColumnKind::STATUS, "status", N_("Status"), 160, false, true },
    { PackageColumnKind::PACKAGE, "package", N_("Package"), 180, false, true },
    { PackageColumnKind::VERSION, "version", N_("Version"), 150, false, true },
    { PackageColumnKind::UPDATE_VERSION, "update-version", N_("Update"), 150, false, true },
    { PackageColumnKind::RELEASE, "release", N_("Release"), 150, false, false },
    { PackageColumnKind::UPDATE_RELEASE, "update-release", N_("Update Release"), 150, false, false },
    { PackageColumnKind::ARCH, "arch", N_("Arch"), 95, false, true },
    { PackageColumnKind::REPO, "repo", N_("Repo"), 130, false, true },
    { PackageColumnKind::SUMMARY, "summary", N_("Summary"), 0, true, true },
  };

  return columns;
}

// -----------------------------------------------------------------------------
// Return the table column definition for one persistent column id.
// -----------------------------------------------------------------------------
const PackageTableColumnDefinition *
package_table_column_definition_by_id(const char *column_id)
{
  if (!column_id) {
    return nullptr;
  }

  for (const auto &column : package_table_column_definitions()) {
    if (column.id == std::string(column_id)) {
      return &column;
    }
  }

  return nullptr;
}

// -----------------------------------------------------------------------------
// Return true when one column id is known by the table.
// -----------------------------------------------------------------------------
static bool
package_table_column_id_exists(const std::string &column_id)
{
  return package_table_column_definition_by_id(column_id.c_str()) != nullptr;
}

// -----------------------------------------------------------------------------
// Return all known package table column ids.
// -----------------------------------------------------------------------------
static std::set<std::string>
package_table_all_column_ids()
{
  std::set<std::string> ids;
  for (const auto &column : package_table_column_definitions()) {
    ids.insert(column.id);
  }

  return ids;
}

// -----------------------------------------------------------------------------
// Return the default package table columns.
// -----------------------------------------------------------------------------
static std::set<std::string>
package_table_default_visible_column_ids()
{
  std::set<std::string> visible;
  for (const auto &column : package_table_column_definitions()) {
    if (column.default_visible) {
      visible.insert(column.id);
    }
  }

  return visible;
}

// -----------------------------------------------------------------------------
// Parse saved package table column ids and ignore ids unknown to this version.
// -----------------------------------------------------------------------------
static std::set<std::string>
package_table_parse_column_ids(const std::string &value)
{
  std::set<std::string> ids;
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find(',', start);
    std::string id = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (package_table_column_id_exists(id)) {
      ids.insert(id);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  return ids;
}

// -----------------------------------------------------------------------------
// Store hidden column ids so future default-visible columns appear by default.
// -----------------------------------------------------------------------------
static void
package_table_save_hidden_column_ids(const std::set<std::string> &hidden)
{
  auto config = config_load_map();
  std::string value;
  for (const auto &column : package_table_column_definitions()) {
    if (hidden.count(column.id) == 0) {
      continue;
    }
    if (!value.empty()) {
      value += ",";
    }
    value += column.id;
  }

  config[kPackageTableHiddenColumnsConfigKey] = value;
  config.erase(kPackageTableLegacyColumnsConfigKey);
  config_save_map(config);
}

// -----------------------------------------------------------------------------
// Read visible package table columns from the user config.
// -----------------------------------------------------------------------------
std::set<std::string>
package_table_load_visible_column_ids()
{
  auto config = config_load_map();
  const std::set<std::string> all_columns = package_table_all_column_ids();

  auto hidden_it = config.find(kPackageTableHiddenColumnsConfigKey);
  if (hidden_it != config.end()) {
    std::set<std::string> hidden = package_table_parse_column_ids(hidden_it->second);
    if (hidden.empty() && !hidden_it->second.empty()) {
      return package_table_default_visible_column_ids();
    }
    std::set<std::string> visible;
    for (const auto &id : all_columns) {
      if (hidden.count(id) == 0) {
        visible.insert(id);
      }
    }
    return visible.empty() ? package_table_default_visible_column_ids() : visible;
  }

  auto legacy_it = config.find(kPackageTableLegacyColumnsConfigKey);
  if (legacy_it == config.end()) {
    return package_table_default_visible_column_ids();
  }

  std::set<std::string> visible = package_table_parse_column_ids(legacy_it->second);
  if (visible.empty()) {
    return package_table_default_visible_column_ids();
  }

  std::set<std::string> hidden;
  for (const auto &id : all_columns) {
    if (visible.count(id) == 0) {
      hidden.insert(id);
    }
  }
  package_table_save_hidden_column_ids(hidden);

  return visible;
}

// -----------------------------------------------------------------------------
// Save visible package table columns to the user config.
// -----------------------------------------------------------------------------
void
package_table_save_visible_column_ids(const std::set<std::string> &visible)
{
  std::set<std::string> hidden;
  for (const auto &column : package_table_column_definitions()) {
    if (visible.count(column.id) == 0) {
      hidden.insert(column.id);
    }
  }

  package_table_save_hidden_column_ids(hidden);
}

// -----------------------------------------------------------------------------
// Return the package table columns exposed to the View menu.
// -----------------------------------------------------------------------------
std::vector<PackageTableColumnInfo>
package_table_column_infos()
{
  std::vector<PackageTableColumnInfo> infos;
  infos.reserve(package_table_column_definitions().size());
  for (const auto &column : package_table_column_definitions()) {
    infos.push_back({ column.id, column.title });
  }

  return infos;
}

// -----------------------------------------------------------------------------
// Return whether one package table column is enabled in user settings.
// -----------------------------------------------------------------------------
bool
package_table_column_is_visible(const char *column_id)
{
  if (!package_table_column_definition_by_id(column_id)) {
    return false;
  }

  std::set<std::string> visible = package_table_load_visible_column_ids();
  return visible.count(column_id) > 0;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
