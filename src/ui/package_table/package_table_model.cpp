// -----------------------------------------------------------------------------
// package_table_model.cpp
// Package row storage for the GTK package table.
// Stores package rows together and creates GTK list items on demand.
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_view_internal.hpp"

#include "ui/package_table/package_table_status.hpp"
#include "ui/common/widgets.hpp"

#include <utility>
#include <vector>

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
// Return the EVR value used for version-like table sorting.
// -----------------------------------------------------------------------------
static PackageTableSortEvr
sort_evr_from_values(const std::string &epoch, const std::string &version, const std::string &release)
{
  if (version.empty() && release.empty()) {
    return {};
  }

  return PackageTableSortEvr {
    .present = true,
    .epoch = epoch,
    .version = version,
    .release = release,
  };
}

// -----------------------------------------------------------------------------
// Return the EVR value used for sorting one package row.
// -----------------------------------------------------------------------------
static PackageTableSortEvr
sort_evr_from_row(const PackageRow &row)
{
  return sort_evr_from_values(row.epoch, row.version, row.release);
}

// -----------------------------------------------------------------------------
// Return the EVR value used for sorting one daemon upgrade target.
// -----------------------------------------------------------------------------
static PackageTableSortEvr
sort_evr_from_daemon_target(const DaemonUpgradeTarget &target)
{
  return sort_evr_from_values(target.epoch, target.version, target.release);
}

// -----------------------------------------------------------------------------
// Return true when the current table uses compact package rows.
// -----------------------------------------------------------------------------
static bool
package_table_uses_compact_rows(const MainWindowUiState *widgets)
{
  return widgets && displayed_package_query_uses_compact_rows(widgets->query_state.displayed_query);
}

// -----------------------------------------------------------------------------
// Return true when the current table shows exact available repository rows.
// -----------------------------------------------------------------------------
static bool
package_table_uses_exact_available_rows(const MainWindowUiState *widgets)
{
  return widgets && displayed_package_query_uses_exact_available_rows(widgets->query_state.displayed_query);
}

// -----------------------------------------------------------------------------
// Snapshot the visible status text and its sort order for one package row.
// -----------------------------------------------------------------------------
void
package_table_fill_item_status(MainWindowUiState *widgets,
                               PackageItem &item,
                               const InstalledPackageResolution &resolution)
{
  // Keep Status sorting tied to the stable package state so marking a pending
  // action does not move the row away from the user in the current view.
  PackageInstallState install_state = item.upgrade_target() ? PackageInstallState::UPGRADEABLE : resolution.state;
  item.status_rank = dnf_backend_get_install_state_sort_rank(install_state);

  const bool exact_available_rows = package_table_uses_exact_available_rows(widgets);
  const bool has_pending_actions = widgets && !widgets->transaction_state.actions.empty();

  // Only exact upgrade rows and pending actions need resolved transaction rows.
  if (!has_pending_actions && (!exact_available_rows || install_state != PackageInstallState::UPGRADEABLE)) {
    item.status_text = package_table_status_text(install_state);
    return;
  }

  PackageTableRow table_row {
    .row = item.row,
    .daemon_upgrade = item.daemon_upgrade,
  };
  const bool exact_installonly_actions =
      widgets && displayed_package_query_uses_exact_installonly_actions(widgets->query_state.displayed_query);
  const std::vector<PendingAction> &actions = widgets->transaction_state.actions;
  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_resolved_selection_with_pending(
      item.row, item.upgrade_target(), table_row.upgrade_generation(), resolution, exact_installonly_actions, actions);
  PendingAction::Type action_type;
  if (package_table_pending_action_for_resolved_row(widgets, table_row, action_rows, action_type)) {
    item.status_text = package_table_pending_action_status_text(action_type, install_state);
    return;
  }

  item.status_text = package_table_status_text_for_resolved_row(table_row, action_rows, exact_available_rows);
}

// -----------------------------------------------------------------------------
// Snapshot display values that depend on installed-package state.
// -----------------------------------------------------------------------------
void
package_table_fill_item_display_values(PackageItem &item,
                                       const InstalledPackageResolution &resolution,
                                       bool compact_view)
{
  PackageTableDisplayValues values {
    .epoch = item.row.epoch,
    .version = item.row.version,
    .update_version = {},
    .install_size = item.row.install_size,
    .release = item.row.release,
    .update_release = {},
    .repo = item.row.repo,
    .update_evr = {},
  };

  if (const DaemonUpgradeTarget *upgrade_target = item.upgrade_target()) {
    if (resolution.has_installed_row) {
      values.version = resolution.installed_row.version;
      values.release = resolution.installed_row.release;
      values.install_size = resolution.installed_row.install_size;
      values.epoch = resolution.installed_row.epoch;
    } else {
      values.version.clear();
      values.release.clear();
      values.install_size.reset();
      values.epoch.clear();
    }

    values.update_version = upgrade_target->version;
    values.update_release = upgrade_target->release;
    values.repo = upgrade_target->repo_id;
    values.update_evr = sort_evr_from_daemon_target(*upgrade_target);
    item.display_values = std::make_shared<PackageTableDisplayValues>(std::move(values));
    return;
  }

  const bool newest_actionable_upgrade =
      resolution.state == PackageInstallState::UPGRADEABLE && item.row.is_newest_available;

  const bool project_available_upgrade =
      compact_view && !resolution.exact_installed && resolution.has_installed_row && newest_actionable_upgrade;

  if (project_available_upgrade) {
    // The table column is named Version, so keep it aligned with the Details tab Version field.
    values.version = resolution.installed_row.version;
    values.release = resolution.installed_row.release;
    values.install_size = resolution.installed_row.install_size;
    values.repo = resolution.installed_row.repo;
    values.epoch = resolution.installed_row.epoch;
  }

  if (resolution.state == PackageInstallState::UPGRADEABLE &&
      (resolution.exact_installed || project_available_upgrade)) {
    const auto *candidate = item.row.repo_candidate.get();
    const bool has_candidate_version = candidate && !candidate->version.empty();
    const bool has_candidate_release = candidate && !candidate->release.empty();
    values.update_version = has_candidate_version ? candidate->version : item.row.version;
    values.update_release = has_candidate_release ? candidate->release : item.row.release;

    values.repo = candidate && !candidate->repo.empty() ? candidate->repo : item.row.repo;

    if (has_candidate_version || has_candidate_release) {
      values.update_evr = sort_evr_from_values(candidate->epoch, candidate->version, candidate->release);
    } else {
      values.update_evr = sort_evr_from_row(item.row);
    }
  }

  const bool display_values_differ = values.epoch != item.row.epoch || values.version != item.row.version ||
      !values.update_version.empty() || values.install_size != item.row.install_size ||
      values.release != item.row.release || !values.update_release.empty() || values.repo != item.row.repo ||
      values.update_evr.present;
  if (display_values_differ) {
    item.display_values = std::make_shared<PackageTableDisplayValues>(std::move(values));
  } else {
    item.display_values.reset();
  }
}

// -----------------------------------------------------------------------------
// Prepare the display and status values once for each row.
// -----------------------------------------------------------------------------
static PackageItem
make_package_item(MainWindowUiState *widgets, PackageTableRow row)
{
  PackageItem item;
  item.row = std::move(row.row);
  item.daemon_upgrade = std::move(row.daemon_upgrade);
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_display_values(item, resolution, package_table_uses_compact_rows(widgets));
  package_table_fill_item_status(widgets, item, resolution);
  return item;
}

static PackageItem
make_package_item(MainWindowUiState *widgets, PackageRow row)
{
  return make_package_item(widgets, PackageTableRow { .row = std::move(row), .daemon_upgrade = {} });
}

// The vector is never resized after construction, so item addresses remain valid.
// Weak references preserve object identity without keeping unused wrappers alive.
struct PackageListStorage {
  explicit PackageListStorage(std::vector<PackageItem> rows)
      : items(std::move(rows)), objects(items.size())
  {
    for (auto &object : objects) {
      g_weak_ref_init(&object, nullptr);
    }
  }

  ~PackageListStorage()
  {
    for (auto &object : objects) {
      g_weak_ref_clear(&object);
    }
  }

  PackageListStorage(const PackageListStorage &) = delete;
  PackageListStorage &operator=(const PackageListStorage &) = delete;

  std::vector<PackageItem> items;
  std::vector<GWeakRef> objects;
};

// A wrapper may outlive the model. Keep its row storage alive until it is released.
struct PackageObjectData {
  std::shared_ptr<PackageListStorage> storage;
  PackageItem *item;
};

struct PackageListModel {
  GObject parent_instance;
  std::shared_ptr<PackageListStorage> *storage;
};

struct PackageListModelClass {
  GObjectClass parent_class;
};

static void package_list_model_list_model_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(PackageListModel,
                        package_list_model,
                        G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, package_list_model_list_model_init))

static GType
package_list_model_get_item_type(GListModel *)
{
  return G_TYPE_OBJECT;
}

static guint
package_list_model_get_n_items(GListModel *model)
{
  auto *self = reinterpret_cast<PackageListModel *>(model);
  return self->storage ? static_cast<guint>((*self->storage)->items.size()) : 0;
}

static gpointer
package_list_model_get_item(GListModel *model, guint position)
{
  auto *self = reinterpret_cast<PackageListModel *>(model);
  if (position >= package_list_model_get_n_items(model)) {
    return nullptr;
  }

  const auto &storage = *self->storage;
  GWeakRef &weak = storage->objects[position];
  if (auto *object = g_weak_ref_get(&weak)) {
    return object;
  }

  GObject *object = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
  auto *data = new PackageObjectData { storage, &storage->items[position] };
  g_object_set_qdata_full(object, package_row_quark(), data, +[](gpointer p) {
    delete static_cast<PackageObjectData *>(p);
  });
  g_weak_ref_set(&weak, object);
  return object;
}

static void
package_list_model_list_model_init(GListModelInterface *iface)
{
  iface->get_item_type = package_list_model_get_item_type;
  iface->get_n_items = package_list_model_get_n_items;
  iface->get_item = package_list_model_get_item;
}

static void
package_list_model_finalize(GObject *object)
{
  auto *self = reinterpret_cast<PackageListModel *>(object);
  delete self->storage;
  G_OBJECT_CLASS(package_list_model_parent_class)->finalize(object);
}

enum {
  PROP_0,
  PROP_ITEM_TYPE,
  PROP_N_ITEMS,
};

static void
package_list_model_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
  switch (property_id) {
  case PROP_ITEM_TYPE:
    g_value_set_gtype(value, G_TYPE_OBJECT);
    break;
  case PROP_N_ITEMS:
    g_value_set_uint(value, package_list_model_get_n_items(G_LIST_MODEL(object)));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    break;
  }
}

static void
package_list_model_class_init(PackageListModelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = package_list_model_finalize;
  object_class->get_property = package_list_model_get_property;
  const auto flags = static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property(
      object_class, PROP_ITEM_TYPE, g_param_spec_gtype("item-type", "Item type", "List item type", G_TYPE_OBJECT, flags));
  g_object_class_install_property(
      object_class, PROP_N_ITEMS, g_param_spec_uint("n-items", "Item count", "Number of list items", 0, G_MAXUINT, 0, flags));
}

static void
package_list_model_init(PackageListModel *self)
{
  self->storage = nullptr;
}

// -----------------------------------------------------------------------------
// Build a fixed-size model from the query result before attaching it to the view.
// -----------------------------------------------------------------------------
template <typename Row>
static GListModel *
make_package_list_model(MainWindowUiState *widgets, std::vector<Row> rows)
{
  std::vector<PackageItem> items;
  items.reserve(rows.size());
  for (auto &row : rows) {
    items.push_back(make_package_item(widgets, std::move(row)));
  }

  // GObject allocates instance memory without running C++ constructors.
  auto storage = std::make_unique<std::shared_ptr<PackageListStorage>>(
      std::make_shared<PackageListStorage>(std::move(items)));
  auto *model = static_cast<PackageListModel *>(g_object_new(package_list_model_get_type(), nullptr));
  model->storage = storage.release();
  return G_LIST_MODEL(model);
}

GListModel *
make_package_model(MainWindowUiState *widgets, std::vector<PackageRow> rows)
{
  return make_package_list_model(widgets, std::move(rows));
}

GListModel *
make_package_model(MainWindowUiState *widgets, std::vector<PackageTableRow> rows)
{
  return make_package_list_model(widgets, std::move(rows));
}

// -----------------------------------------------------------------------------
// Match package names and summaries against case-folded filter text.
// -----------------------------------------------------------------------------
bool
package_table_quick_filter_matches(const PackageRow &row, const char *folded_text)
{
  if (!folded_text || !*folded_text) {
    return true;
  }

  gchar *name = g_utf8_casefold(row.name.c_str(), -1);
  bool matches = g_strstr_len(name, -1, folded_text) != nullptr;
  g_free(name);

  if (!matches) {
    gchar *summary = g_utf8_casefold(row.summary.c_str(), -1);
    matches = g_strstr_len(summary, -1, folded_text) != nullptr;
    g_free(summary);
  }

  return matches;
}

// -----------------------------------------------------------------------------
// Read the sortable package wrapper stored on a GTK list item.
// -----------------------------------------------------------------------------
const PackageItem *
package_item_from_object(GObject *obj)
{
  return mutable_package_item_from_object(obj);
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

  auto *data = static_cast<PackageObjectData *>(g_object_get_qdata(obj, package_row_quark()));
  return data ? data->item : nullptr;
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
// Convert the stored package wrapper back to the public table row value.
// -----------------------------------------------------------------------------
PackageTableRow
package_table_row_from_item(const PackageItem &item)
{
  return {
    .row = item.row,
    .daemon_upgrade = item.daemon_upgrade,
  };
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
