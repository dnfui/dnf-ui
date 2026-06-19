// -----------------------------------------------------------------------------
// package_table_sort.cpp
// Sorting and cell text helpers for the package table.
// Keeps package comparison rules separate from the GTK ColumnView setup code.
// -----------------------------------------------------------------------------
#include "package_table_view_internal.hpp"

// -----------------------------------------------------------------------------
// Return the visible text for one package table cell.
// -----------------------------------------------------------------------------
std::string
package_table_column_text(const PackageItem &item, PackageColumnKind kind)
{
  switch (kind) {
  case PackageColumnKind::STATUS:
    return item.status_text;
  case PackageColumnKind::PACKAGE:
    return item.row.name;
  case PackageColumnKind::VERSION: {
    PackageRow installed_row;
    if (dnf_backend_get_package_install_state(item.row) != PackageInstallState::UPGRADEABLE &&
        dnf_backend_get_installed_package_row_by_name_arch(item.row, installed_row) &&
        installed_row.nevra != item.row.nevra) {
      // The table column is named Version, so keep it aligned with the Info tab Version field.
      // Release remains available in the details pane.
      return installed_row.version;
    }
    return item.row.version;
  }
  case PackageColumnKind::ARCH:
    return item.row.arch;
  case PackageColumnKind::REPO: {
    PackageRow installed_row;
    // A repository name such as "fedora" means the row is available from that repo.
    // "@System" means the row comes from the local installed rpmdb.
    // For upgradable rows, show the repo that provides the update candidate.
    if (dnf_backend_get_package_install_state(item.row) != PackageInstallState::UPGRADEABLE &&
        dnf_backend_get_installed_package_row_by_name_arch(item.row, installed_row)) {
      return installed_row.repo;
    }
    return item.row.repo;
  }
  case PackageColumnKind::SUMMARY:
    return item.row.summary;
  }

  return {};
}

// -----------------------------------------------------------------------------
// Free data owned by one package table column sorter.
// -----------------------------------------------------------------------------
void
package_table_column_sorter_data_free(gpointer p)
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
    result = compare_text(package_table_column_text(lhs, PackageColumnKind::VERSION),
                          package_table_column_text(rhs, PackageColumnKind::VERSION));
    break;
  case PackageColumnKind::ARCH:
    result = compare_text(lhs.row.arch, rhs.row.arch);
    break;
  case PackageColumnKind::REPO:
    result = compare_text(package_table_column_text(lhs, PackageColumnKind::REPO),
                          package_table_column_text(rhs, PackageColumnKind::REPO));
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
int
package_table_column_sorter_compare(gconstpointer item1, gconstpointer item2, gpointer user_data)
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
// EOF
// -----------------------------------------------------------------------------
