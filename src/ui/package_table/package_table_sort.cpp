// -----------------------------------------------------------------------------
// package_table_sort.cpp
// Sorting and cell text helpers for the package table.
// Keeps package comparison rules separate from the GTK ColumnView setup code.
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_view_internal.hpp"

// -----------------------------------------------------------------------------
// Return the install size for the version shown in the package table.
// -----------------------------------------------------------------------------
static const std::optional<std::uint64_t> &
package_table_item_install_size(const PackageItem &item)
{
  return item.display_values ? item.display_values->install_size : item.row.install_size;
}

// -----------------------------------------------------------------------------
// Return the visible text for one package table cell.
// -----------------------------------------------------------------------------
std::string
package_table_column_text(const PackageItem &item, PackageColumnKind kind)
{
  const PackageTableDisplayValues *display = item.display_values.get();

  switch (kind) {
  case PackageColumnKind::STATUS:
    return item.status_text;
  case PackageColumnKind::PACKAGE:
    return item.row.name;
  case PackageColumnKind::VERSION:
    return display ? display->version : item.row.version;
  case PackageColumnKind::UPDATE_VERSION:
    return display ? display->update_version : std::string();
  case PackageColumnKind::RELEASE:
    return display ? display->release : item.row.release;
  case PackageColumnKind::UPDATE_RELEASE:
    return display ? display->update_release : std::string();
  case PackageColumnKind::ARCH:
    return item.row.arch;
  case PackageColumnKind::REPO:
    return display ? display->repo : item.row.repo;
  case PackageColumnKind::SIZE: {
    const auto &size = package_table_item_install_size(item);
    if (!size) {
      return {};
    }

    char *formatted = g_format_size(*size);
    std::string text = formatted;
    g_free(formatted);
    return text;
  }
  case PackageColumnKind::SUMMARY:
    return item.row.summary;
  }

  return {};
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
// Compare stored package values used by Version and Update Version.
// -----------------------------------------------------------------------------
static int
compare_sort_epoch_version(const PackageTableSortEvr &lhs, const PackageTableSortEvr &rhs)
{
  if (!lhs.present || !rhs.present) {
    if (!lhs.present && !rhs.present) {
      return 0;
    }
    return lhs.present ? 1 : -1;
  }

  return dnf_backend_compare_epoch_version_text(lhs.epoch, lhs.version, rhs.epoch, rhs.version);
}

// -----------------------------------------------------------------------------
// Compare stored package values used by Release and Update Release.
// -----------------------------------------------------------------------------
static int
compare_sort_release(const PackageTableSortEvr &lhs, const PackageTableSortEvr &rhs)
{
  if (!lhs.present || !rhs.present) {
    if (!lhs.present && !rhs.present) {
      return 0;
    }
    return lhs.present ? 1 : -1;
  }

  return dnf_backend_compare_rpm_version_text(lhs.release, rhs.release);
}

// -----------------------------------------------------------------------------
// Compare package sizes numerically and place unknown sizes before known sizes.
// -----------------------------------------------------------------------------
static int
compare_install_size(const std::optional<std::uint64_t> &lhs, const std::optional<std::uint64_t> &rhs)
{
  if (!lhs || !rhs) {
    if (!lhs && !rhs) {
      return 0;
    }
    return lhs ? 1 : -1;
  }

  if (*lhs < *rhs) {
    return -1;
  }
  if (*lhs > *rhs) {
    return 1;
  }
  return 0;
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
    result = compare_sort_epoch_version(lhs.current_evr, rhs.current_evr);
    break;
  case PackageColumnKind::UPDATE_VERSION:
    result = compare_sort_epoch_version(lhs.update_evr, rhs.update_evr);
    break;
  case PackageColumnKind::RELEASE:
    result = compare_sort_release(lhs.current_evr, rhs.current_evr);
    break;
  case PackageColumnKind::UPDATE_RELEASE:
    result = compare_sort_release(lhs.update_evr, rhs.update_evr);
    break;
  case PackageColumnKind::ARCH:
    result = compare_text(lhs.row.arch, rhs.row.arch);
    break;
  case PackageColumnKind::REPO:
    result = compare_text(package_table_column_text(lhs, PackageColumnKind::REPO),
                          package_table_column_text(rhs, PackageColumnKind::REPO));
    break;
  case PackageColumnKind::SIZE:
    result = compare_install_size(package_table_item_install_size(lhs), package_table_item_install_size(rhs));
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
  const PackageItem *lhs = package_item_from_object(G_OBJECT(const_cast<gpointer>(item1)));
  const PackageItem *rhs = package_item_from_object(G_OBJECT(const_cast<gpointer>(item2)));
  const int kind_value = GPOINTER_TO_INT(user_data);
  if (kind_value <= 0 || !lhs || !rhs) {
    return 0;
  }

  PackageColumnKind kind = static_cast<PackageColumnKind>(kind_value - 1);
  return compare_package_items(*lhs, *rhs, kind);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
