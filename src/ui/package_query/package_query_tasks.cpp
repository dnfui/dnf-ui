// -----------------------------------------------------------------------------
// package_query_tasks.cpp
// Background workers for package query controller
//
// Owns the GTask worker callbacks and their GTK-thread completion handlers.
// The public controller decides when a query should start.
// -----------------------------------------------------------------------------
#include "ui/package_query/package_query_controller_internal.hpp"

#include "dnf_backend/base_manager.hpp"
#include "debug_trace.hpp"
#include "i18n.hpp"
#include "ui/package_query/package_query_cache.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"
#include "upgrade/daemon_upgrade_state.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"

#include <set>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef DNFUI_DEBUG_TRACE
static long long
elapsed_ms_since(gint64 started_at_us)
{
  return static_cast<long long>((g_get_monotonic_time() - started_at_us) / 1000);
}
#endif

// -----------------------------------------------------------------------------
// Data passed to one background search task.
// -----------------------------------------------------------------------------
struct SearchTaskData {
  char *term;
  char *cache_key;
  uint64_t request_id;
  gint64 started_at_us;
  // BaseManager generation recorded when the task starts.
  // Used to drop outdated results if the backend Base is rebuilt before the task ends.
  uint64_t generation;
  // Search-cache epoch recorded when the task starts.
  // Used to avoid storing rows back into a cache state the UI invalidated while the worker was still running.
  uint64_t cache_epoch;
  bool search_in_description;
  bool exact_match;
};

// -----------------------------------------------------------------------------
// Free data owned by one background search task.
// -----------------------------------------------------------------------------
static void
search_task_data_free(gpointer p)
{
  SearchTaskData *d = static_cast<SearchTaskData *>(p);
  if (!d) {
    return;
  }
  g_free(d->term);
  g_free(d->cache_key);
  g_free(d);
}

// Data passed to one background package list task.
// The generation lets completion ignore results from an older backend Base.
// The request id keeps the Stop button matched to the task that controls it.
struct PackageListTaskData {
  uint64_t request_id;
  uint64_t generation;
  gint64 started_at_us;
};

struct UpgradeablePackageListResult {
  DaemonUpgradeRefreshId refresh_id = 0;
  std::vector<TransactionServiceUpgradeTarget> targets;
  std::vector<PackageRow> metadata_rows;
  uint64_t daemon_generation = 0;
  bool refresh_closed = false;

  ~UpgradeablePackageListResult()
  {
    if (refresh_id != 0 && !refresh_closed) {
      DaemonUpgradeState::instance().abandon_refresh(refresh_id);
    }
  }
};

// Data passed to one exact selected-package reload task.
// The selected NEVRA and generation are checked again before the table is updated.
struct ExactPackageReloadTaskData {
  char *nevra;
  uint64_t request_id;
  uint64_t generation;
  gint64 started_at_us;
};

// -----------------------------------------------------------------------------
// Free data owned by one exact selected-package reload task.
// -----------------------------------------------------------------------------
static void
exact_package_reload_task_data_free(gpointer p)
{
  ExactPackageReloadTaskData *d = static_cast<ExactPackageReloadTaskData *>(p);
  if (!d) {
    return;
  }
  g_free(d->nevra);
  g_free(d);
}

struct QueryBackendBaseDropGuard {
  explicit QueryBackendBaseDropGuard(GCancellable *cancellable = nullptr)
      : cancellable(cancellable)
  {
  }

  ~QueryBackendBaseDropGuard()
  {
    // A stopped list worker may still be waiting for BaseManager.
    // Do not block again just to release cached metadata.
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      return;
    }
    BaseManager::instance().drop_cached_base();
  }

  private:
  GCancellable *cancellable = nullptr;
};

// -----------------------------------------------------------------------------
// Build a basic package row from one daemon upgrade target.
// libdnf5 metadata can enrich this later, but the daemon target itself is the
// authority for whether the row appears in List Upgradable.
// -----------------------------------------------------------------------------
static PackageRow
package_row_from_daemon_upgrade_target(const TransactionServiceUpgradeTarget &target)
{
  PackageRow row;
  row.nevra = target.nevra.empty() ? target.full_nevra : target.nevra;
  row.name = target.name;
  row.epoch = target.epoch;
  row.version = target.version;
  row.release = target.release;
  row.arch = target.arch;
  row.repo = target.repo_id;
  return row;
}

// -----------------------------------------------------------------------------
// Build table rows from one complete daemon upgrade snapshot and optional metadata.
// Missing metadata does not hide daemon-reported upgrades.
// -----------------------------------------------------------------------------
static std::vector<PackageTableRow>
package_table_rows_from_daemon_targets(const DaemonUpgradeSnapshot &snapshot,
                                       const std::vector<PackageRow> &metadata_rows)
{
  std::map<std::string, PackageRow> metadata_by_nevra;
  for (const auto &row : metadata_rows) {
    metadata_by_nevra.emplace(row.nevra, row);
  }

  std::vector<PackageTableRow> rows;
  rows.reserve(snapshot.targets_by_name_arch.size());
  for (const auto &[key, target] : snapshot.targets_by_name_arch) {
    PackageRow row = package_row_from_daemon_upgrade_target(target);
    auto metadata = metadata_by_nevra.find(row.nevra);
    if (metadata != metadata_by_nevra.end()) {
      row = metadata->second;
    }

    rows.push_back({
        .row = row,
        .upgrade_target = target,
        .upgrade_generation = snapshot.generation,
    });
  }

  return rows;
}

// -----------------------------------------------------------------------------
// Query installed packages on a worker thread.
// -----------------------------------------------------------------------------
static void
on_list_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  QueryBackendBaseDropGuard base_drop_guard(cancellable);

  try {
    // Query all installed packages.
    auto *results = new std::vector<PackageRow>(dnf_backend_get_installed_package_rows_interruptible(cancellable));
    // Let GTask free results if completion is skipped or cancelled.
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish installed package listing on the GTK thread.
// -----------------------------------------------------------------------------
static void
on_list_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
          widgets_spinner_release(widgets->query.spinner);
          package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
        }
      }
    }
    return;
  }

  // Drop outdated results if the backend Base changed while the worker was running.
  // This prevents showing rows that no longer match the current repository state.
  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Release this task's spinner slot.
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  }

  if (packages) {
    package_query_set_displayed_query_kind(widgets, DisplayedPackageQueryKind::LIST_INSTALLED);

    // Fill the package table and update status.
    if (widgets->query_state.preserve_selection_on_reload) {
      widgets->results.selected_nevra = widgets->query_state.reload_selected_nevra;
    } else {
      widgets->results.selected_nevra.clear();
    }
    package_table_fill_package_view(
        widgets, *packages, packages->empty() ? PackageTableEmptyState::NO_RESULTS : PackageTableEmptyState::READY);
    std::string msg =
        dnfui_i18n_format_count(packages->size(), "Found %zu installed package.", "Found %zu installed packages.");
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    package_query_show_duration_label(widgets, _("List Installed"), td ? td->started_at_us : 0);
    package_query_finish_results_refresh(widgets);
    delete packages;
  } else {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : _("Error listing packages."), "red");
    package_query_show_duration_label(widgets, _("List Installed"), td ? td->started_at_us : 0);
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Query the merged package list on a worker thread.
// The result includes repository candidates and installed-only local RPMs.
// -----------------------------------------------------------------------------
static void
on_list_available_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  QueryBackendBaseDropGuard base_drop_guard(cancellable);

  try {
    auto *results = new std::vector<PackageRow>(dnf_backend_get_browse_package_rows_interruptible(cancellable));
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish merged package listing on the GTK thread.
// Refresh the installed snapshot before updating package status in the table.
// -----------------------------------------------------------------------------
static void
on_list_available_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
          widgets_spinner_release(widgets->query.spinner);
          package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
        }
      }
    }
    return;
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Release this task's spinner slot.
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
  }

  if (packages) {
    package_query_set_displayed_query_kind(widgets, DisplayedPackageQueryKind::LIST_AVAILABLE);

    if (widgets->query_state.preserve_selection_on_reload) {
      widgets->results.selected_nevra = widgets->query_state.reload_selected_nevra;
    } else {
      widgets->results.selected_nevra.clear();
    }
    package_table_fill_package_view(
        widgets, *packages, packages->empty() ? PackageTableEmptyState::NO_RESULTS : PackageTableEmptyState::READY);
    std::string msg = dnfui_i18n_format_count(packages->size(), "Found %zu package.", "Found %zu packages.");
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    package_query_show_duration_label(widgets, _("List Packages"), td ? td->started_at_us : 0);
    package_query_finish_results_refresh(widgets);
    delete packages;
  } else {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : _("Error listing packages."), "red");
    package_query_show_duration_label(widgets, _("List Packages"), td ? td->started_at_us : 0);
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Query packages that have available updates on a worker thread.
// -----------------------------------------------------------------------------
static void
on_list_upgradeable_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  QueryBackendBaseDropGuard base_drop_guard(cancellable);
  std::optional<DaemonUpgradeRefreshId> refresh_id;
  bool refresh_state_closed = false;

  try {
#ifdef DNFUI_DEBUG_TRACE
    const gint64 started_at_us = g_get_monotonic_time();
    DNFUI_TRACE("Upgradable list task start");
#endif

    refresh_id = DaemonUpgradeState::instance().begin_refresh();
    if (!refresh_id.has_value()) {
      throw std::runtime_error(_("dnf5daemon upgrade information is already being refreshed."));
    }

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      DaemonUpgradeState::instance().abandon_refresh(refresh_id.value());
      refresh_state_closed = true;
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("List Upgradable was cancelled."));
      return;
    }

    std::vector<TransactionServiceUpgradeTarget> targets;
    std::string error;
    if (!transaction_service_client_list_upgrade_targets(targets, error, cancellable)) {
      if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        DaemonUpgradeState::instance().abandon_refresh(refresh_id.value());
        refresh_state_closed = true;
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("List Upgradable was cancelled."));
        return;
      }

      DaemonUpgradeState::instance().publish_failure(refresh_id.value(), error);
      refresh_state_closed = true;
      throw std::runtime_error(error.empty() ? _("Unable to load upgradable packages from dnf5daemon.") : error);
    }
#ifdef DNFUI_DEBUG_TRACE
    DNFUI_TRACE(
        "Upgradable list task daemon targets=%zu elapsed_ms=%lld", targets.size(), elapsed_ms_since(started_at_us));
#endif

    std::vector<std::string> target_nevras;
    target_nevras.reserve(targets.size());
    for (const auto &target : targets) {
      target_nevras.push_back(target.nevra.empty() ? target.full_nevra : target.nevra);
    }

    std::vector<PackageRow> metadata_rows;
    try {
      metadata_rows = dnf_backend_get_available_package_metadata_by_nevras_interruptible(target_nevras, cancellable);
    } catch (const std::exception &) {
      // Metadata enrichment is best effort. The daemon target remains visible
      // because dnf5daemon is the authority for List Upgradable.
      metadata_rows.clear();
    }
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      DaemonUpgradeState::instance().abandon_refresh(refresh_id.value());
      refresh_state_closed = true;
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("List Upgradable was cancelled."));
      return;
    }

#ifdef DNFUI_DEBUG_TRACE
    DNFUI_TRACE(
        "Upgradable list task metadata=%zu total_ms=%lld", metadata_rows.size(), elapsed_ms_since(started_at_us));
#endif

    auto *results = new UpgradeablePackageListResult;
    results->refresh_id = refresh_id.value();
    results->targets = std::move(targets);
    results->metadata_rows = std::move(metadata_rows);
    refresh_state_closed = true;
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<UpgradeablePackageListResult *>(p); });
  } catch (const std::exception &e) {
    if (refresh_id.has_value() && !refresh_state_closed) {
      if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        DaemonUpgradeState::instance().abandon_refresh(refresh_id.value());
      } else {
        DaemonUpgradeState::instance().publish_failure(refresh_id.value(), e.what());
      }
      refresh_state_closed = true;
    }
    if (refresh_id.has_value() && cancellable && g_cancellable_is_cancelled(cancellable)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("List Upgradable was cancelled."));
      return;
    }
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish upgradable package listing on the GTK thread.
// Refresh the installed snapshot before updating package status in the table.
// -----------------------------------------------------------------------------
static void
on_list_upgradeable_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
          widgets_spinner_release(widgets->query.spinner);
          package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_UPGRADEABLE);
        }
      }
    }
    return;
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_UPGRADEABLE);
    return;
  }

  GError *error = nullptr;
  UpgradeablePackageListResult *result =
      static_cast<UpgradeablePackageListResult *>(g_task_propagate_pointer(task, &error));

  // Release this task's spinner slot.
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_UPGRADEABLE);
  }

  if (result) {
    std::string publish_error;
    if (!DaemonUpgradeState::instance().publish_success(result->refresh_id, result->targets, publish_error)) {
      result->refresh_closed = true;
      const bool refresh_no_longer_active = publish_error == "dnf5daemon upgrade refresh is no longer active.";
      delete result;
      if (!refresh_no_longer_active) {
        ui_helpers_set_status(widgets->query.status_label,
                              publish_error.empty() ? _("Unable to publish dnf5daemon upgrade information.")
                                                    : publish_error.c_str(),
                              "red");
      }
      return;
    }
    result->refresh_closed = true;

    DaemonUpgradeSnapshot snapshot = DaemonUpgradeState::instance().snapshot();
    if (snapshot.status != DaemonUpgradeSnapshotStatus::READY) {
      delete result;
      return;
    }

    auto rows = package_table_rows_from_daemon_targets(snapshot, result->metadata_rows);

    package_query_set_displayed_query_kind(widgets, DisplayedPackageQueryKind::LIST_UPGRADEABLE);

    if (widgets->query_state.preserve_selection_on_reload) {
      widgets->results.selected_nevra = widgets->query_state.reload_selected_nevra;
    } else {
      widgets->results.selected_nevra.clear();
    }
    package_table_fill_package_view(
        widgets, rows, rows.empty() ? PackageTableEmptyState::NO_RESULTS : PackageTableEmptyState::READY);
    std::string msg =
        dnfui_i18n_format_count(rows.size(), "Found %zu upgradable package.", "Found %zu upgradable packages.");
    ui_helpers_set_status(widgets->query.status_label, msg, rows.empty() ? "gray" : "green");
    package_query_show_duration_label(widgets, _("List Upgradable"), td ? td->started_at_us : 0);
    package_query_finish_results_refresh(widgets);
    delete result;
  } else {
    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_error_free(error);
      return;
    }

    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    widgets->results.selected_nevra.clear();
    package_query_set_displayed_query_kind(widgets, DisplayedPackageQueryKind::LIST_UPGRADEABLE);
    std::vector<PackageRow> empty_packages;
    package_table_fill_package_view(widgets, empty_packages);
    package_query_finish_results_refresh(widgets);
    ui_helpers_set_status(
        widgets->query.status_label, error ? error->message : _("Error listing upgradable packages."), "red");
    package_query_show_duration_label(widgets, _("List Upgradable"), td ? td->started_at_us : 0);
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Search the merged package list on a worker thread.
// -----------------------------------------------------------------------------
static void
on_search_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  QueryBackendBaseDropGuard base_drop_guard(cancellable);

  const SearchTaskData *td = static_cast<const SearchTaskData *>(task_data);
  const char *pattern = td ? td->term : "";
  try {
    DNFUI_TRACE(
        "Search task start request=%llu pattern=%s", td ? static_cast<unsigned long long>(td->request_id) : 0, pattern);
    auto *results = new std::vector<PackageRow>(dnf_backend_search_package_rows_interruptible(pattern, cancellable));
    DNFUI_TRACE("Search task done request=%llu results=%zu",
                td ? static_cast<unsigned long long>(td->request_id) : 0,
                results->size());
    // Let GTask free results if completion is skipped or cancelled.
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    DNFUI_TRACE(
        "Search task failed request=%llu error=%s", td ? static_cast<unsigned long long>(td->request_id) : 0, e.what());
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish a package search on the GTK thread.
// -----------------------------------------------------------------------------
static void
on_search_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  GCancellable *c = g_task_get_cancellable(task);
  const SearchTaskData *td = static_cast<const SearchTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed && c && g_cancellable_is_cancelled(c) && td) {
      widgets_spinner_release(widgets->query.spinner);
      package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
    }
    return;
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Release this task's spinner slot.
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
  }

  if (packages) {
    // Save rows so the same search can be shown faster next time.
    // Dropping the cached Base releases memory, but it does not make this result stale.
    if (td && td->cache_key) {
      package_query_cache_store(td->cache_key, td->generation, td->cache_epoch, *packages);
    }

    if (td) {
      package_query_set_displayed_search_query(
          widgets, td->term ? td->term : "", td->search_in_description, td->exact_match);
    }

    // Fill the package table and display the result count.
    if (widgets->query_state.preserve_selection_on_reload) {
      widgets->results.selected_nevra = widgets->query_state.reload_selected_nevra;
    } else {
      widgets->results.selected_nevra.clear();
    }
    package_table_fill_package_view(
        widgets, *packages, packages->empty() ? PackageTableEmptyState::NO_RESULTS : PackageTableEmptyState::READY);
    std::string msg = dnfui_i18n_format_count(packages->size(), "Found %zu package.", "Found %zu packages.");
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    package_query_show_duration_label(widgets, _("Search"), td ? td->started_at_us : 0);
    package_query_finish_results_refresh(widgets);
    delete packages;
  } else {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : _("Error or no results."), "red");
    package_query_show_duration_label(widgets, _("Search"), td ? td->started_at_us : 0);
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Reload one exact selected package on a worker thread.
// This path is used for one-package views from the pending-actions sidebar.
// -----------------------------------------------------------------------------
static void
on_exact_package_reload_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  QueryBackendBaseDropGuard base_drop_guard(cancellable);

  const ExactPackageReloadTaskData *td = static_cast<const ExactPackageReloadTaskData *>(task_data);
  const char *nevra = td && td->nevra ? td->nevra : "";

  try {
    dnf_backend_refresh_installed_nevras();

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Reload cancelled."));
      return;
    }

    auto *results = new std::vector<PackageRow>(dnf_backend_get_installed_package_rows_by_nevra(nevra));
    if (results->empty()) {
      *results = dnf_backend_get_available_package_rows_by_nevra(nevra);
    }
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish exact selected-package reload on the GTK thread.
// -----------------------------------------------------------------------------
static void
on_exact_package_reload_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  const ExactPackageReloadTaskData *td = static_cast<const ExactPackageReloadTaskData *>(g_task_get_task_data(task));
  const std::string task_nevra = td && td->nevra ? td->nevra : "";

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
          widgets_spinner_release(widgets->query.spinner);
          package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::EXACT_RELOAD);
        }
      }
    }
    return;
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::EXACT_RELOAD);
    return;
  }

  if (widgets->results.selected_nevra != task_nevra || widgets->query_state.reload_selected_nevra != task_nevra) {
    widgets_spinner_release(widgets->query.spinner);
    if (td) {
      package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::EXACT_RELOAD);
    }
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::EXACT_RELOAD);
  }

  if (packages) {
    widgets->results.selected_nevra = packages->empty() ? "" : task_nevra;
    package_table_fill_package_view(
        widgets, *packages, packages->empty() ? PackageTableEmptyState::NO_RESULTS : PackageTableEmptyState::READY);
    package_query_show_duration_label(widgets, _("Refresh"), td ? td->started_at_us : 0);
    package_query_finish_results_refresh(widgets);
    delete packages;
  } else {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : _("Error refreshing package."), "red");
    package_query_show_duration_label(widgets, _("Refresh"), td ? td->started_at_us : 0);
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Start one installed-package list worker.
// -----------------------------------------------------------------------------
void
package_query_start_list_installed_task(MainWindowUiState *widgets)
{
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();
  td->started_at_us = g_get_monotonic_time();

  package_query_begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_list_task_finished);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start one package browse worker.
// -----------------------------------------------------------------------------
void
package_query_start_list_available_task(MainWindowUiState *widgets)
{
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();
  td->started_at_us = g_get_monotonic_time();

  package_query_begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_list_available_task_finished);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_available_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start one upgradable-package list worker.
// -----------------------------------------------------------------------------
void
package_query_start_list_upgradeable_task(MainWindowUiState *widgets)
{
  package_query_clear_displayed_upgradeable_table(widgets);
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();
  td->started_at_us = g_get_monotonic_time();

  package_query_begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_UPGRADEABLE);
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_list_upgradeable_task_finished);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_upgradeable_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start one package search worker.
// -----------------------------------------------------------------------------
void
package_query_start_search_task(MainWindowUiState *widgets,
                                const std::string &term,
                                const std::string &cache_key,
                                uint64_t generation,
                                uint64_t cache_epoch,
                                const DnfBackendSearchOptions &search_options)
{
  widgets_spinner_acquire(widgets->query.spinner);

  SearchTaskData *td = static_cast<SearchTaskData *>(g_malloc0(sizeof *td));
  td->term = g_strdup(term.c_str());
  td->cache_key = g_strdup(cache_key.c_str());
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->started_at_us = g_get_monotonic_time();
  td->generation = generation;
  td->cache_epoch = cache_epoch;
  td->search_in_description = search_options.search_in_description;
  td->exact_match = search_options.exact_match;

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  package_query_begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::SEARCH);
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_search_task_finished);
  g_task_set_task_data(task, td, search_task_data_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start one exact selected-package reload worker.
// -----------------------------------------------------------------------------
void
package_query_start_exact_package_reload_task(MainWindowUiState *widgets, const std::string &nevra)
{
  if (!widgets || nevra.empty()) {
    return;
  }

  widgets_spinner_acquire(widgets->query.spinner);

  ExactPackageReloadTaskData *td = static_cast<ExactPackageReloadTaskData *>(g_malloc0(sizeof *td));
  td->nevra = g_strdup(nevra.c_str());
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();
  td->started_at_us = g_get_monotonic_time();

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  package_query_begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::EXACT_RELOAD);
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_exact_package_reload_task_finished);
  g_task_set_task_data(task, td, exact_package_reload_task_data_free);
  g_task_run_in_thread(task, on_exact_package_reload_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
