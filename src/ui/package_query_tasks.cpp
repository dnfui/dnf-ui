// -----------------------------------------------------------------------------
// package_query_tasks.cpp
// Background workers for package query controller
//
// Owns the GTask worker callbacks and their GTK-thread completion handlers.
// The public controller decides when a query should start.
// -----------------------------------------------------------------------------
#include "package_query_controller_internal.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "i18n.hpp"
#include "package_query_cache.hpp"
#include "package_table_view.hpp"
#include "ui_helpers.hpp"
#include "widgets.hpp"
#include "widgets_internal.hpp"

#include <string>
#include <vector>

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
  // Used to avoid storing rows back into a cache state the UI invalidated
  // while the worker was still running.
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
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
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
    package_table_fill_package_view(widgets, *packages);
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
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
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
    package_table_fill_package_view(widgets, *packages);
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

  try {
    auto *results = new std::vector<PackageRow>(dnf_backend_get_upgradeable_package_rows_interruptible(cancellable));
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
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
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
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
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Release this task's spinner slot.
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    package_query_end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_UPGRADEABLE);
  }

  if (packages) {
    package_query_set_displayed_query_kind(widgets, DisplayedPackageQueryKind::LIST_UPGRADEABLE);

    if (widgets->query_state.preserve_selection_on_reload) {
      widgets->results.selected_nevra = widgets->query_state.reload_selected_nevra;
    } else {
      widgets->results.selected_nevra.clear();
    }
    package_table_fill_package_view(widgets, *packages);
    std::string msg =
        dnfui_i18n_format_count(packages->size(), "Found %zu upgradable package.", "Found %zu upgradable packages.");
    ui_helpers_set_status(widgets->query.status_label, msg, packages->empty() ? "gray" : "green");
    package_query_show_duration_label(widgets, _("List Upgradable"), td ? td->started_at_us : 0);
    package_query_finish_results_refresh(widgets);
    delete packages;
  } else {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
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
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GCancellable *c = g_task_get_cancellable(task);
  const SearchTaskData *td = static_cast<const SearchTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed && c && g_cancellable_is_cancelled(c) && td) {
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
    package_table_fill_package_view(widgets, *packages);
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
// Start one installed-package list worker.
// -----------------------------------------------------------------------------
void
package_query_start_list_installed_task(SearchWidgets *widgets)
{
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();
  td->started_at_us = g_get_monotonic_time();

  package_query_begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, on_list_task_finished);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start one package browse worker.
// -----------------------------------------------------------------------------
void
package_query_start_list_available_task(SearchWidgets *widgets)
{
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();
  td->started_at_us = g_get_monotonic_time();

  package_query_begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, on_list_available_task_finished);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_available_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start one upgradable-package list worker.
// -----------------------------------------------------------------------------
void
package_query_start_list_upgradeable_task(SearchWidgets *widgets)
{
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();
  td->started_at_us = g_get_monotonic_time();

  package_query_begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_UPGRADEABLE);
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, on_list_upgradeable_task_finished);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_upgradeable_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start one package search worker.
// -----------------------------------------------------------------------------
void
package_query_start_search_task(SearchWidgets *widgets,
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
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, on_search_task_finished);
  g_task_set_task_data(task, td, search_task_data_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
