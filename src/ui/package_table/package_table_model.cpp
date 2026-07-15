// -----------------------------------------------------------------------------
// package_table_model.cpp
// Package row storage for the GTK package table.
// Wraps PackageRow values in GObjects so the ColumnView can sort and select rows without owning backend data directly.
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_view_internal.hpp"

#include "i18n.hpp"
#include "ui/package_table/package_table_status.hpp"
#include "ui/common/widgets.hpp"

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

// -----------------------------------------------------------------------------
// Snapshot the visible status text and its sort order for one package row.
// -----------------------------------------------------------------------------
void
package_table_fill_item_status(MainWindowUiState *widgets, PackageItem &item)
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
      case PendingAction::UPGRADE:
        item.status_text = _("Pending Upgrade");
        break;
      case PendingAction::DOWNGRADE:
        item.status_text = _("Pending Downgrade");
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
// Wrap one package row in a GObject so GTK list models can sort and select it.
// -----------------------------------------------------------------------------
GObject *
make_package_object(MainWindowUiState *widgets, const PackageRow &row)
{
  GObject *obj = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
  auto *item = new PackageItem { row, {}, 0 };
  package_table_fill_item_status(widgets, *item);
  g_object_set_qdata_full(obj, package_row_quark(), item, +[](gpointer p) { delete static_cast<PackageItem *>(p); });

  return obj;
}

// -----------------------------------------------------------------------------
// Read the sortable package wrapper stored on a GTK list item.
// -----------------------------------------------------------------------------
const PackageItem *
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
PackageItem *
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
const PackageRow *
package_row_from_object(GObject *obj)
{
  const PackageItem *item = package_item_from_object(obj);
  if (!item) {
    return nullptr;
  }

  return &item->row;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
