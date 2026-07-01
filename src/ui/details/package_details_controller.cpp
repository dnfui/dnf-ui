// -----------------------------------------------------------------------------
// src/ui/details/package_details_controller.cpp
// Package selection and details panel controller
// Handles package selection state, action-button sensitivity, and the async
// package details load that updates the details panel.
// -----------------------------------------------------------------------------
#include "ui/details/package_details_controller.hpp"

#include "dnf_backend/base_manager.hpp"
#include "debug_trace.hpp"
#include "i18n.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/package_table/package_table_status.hpp"

#include <cstring>
#include <string>

// Task data for one package details load.
// Snapshot generation at dispatch time.
// Outdated results can be dropped after a Base rebuild.
struct InfoTaskData {
  char *nevra;
  char *status_text;
  uint64_t generation;
};

// Text payload returned by the background package details task.
struct InfoTaskResult {
  char *info;
  char *files;
  char *deps;
};

// Task data for one changelog load.
// The selected NEVRA and generation are checked before the result is shown.
struct ChangelogTaskData {
  char *nevra;
  uint64_t generation;
};

// -----------------------------------------------------------------------------
// Free data owned by one package details task.
// -----------------------------------------------------------------------------
static void
info_task_data_free(gpointer p)
{
  InfoTaskData *d = static_cast<InfoTaskData *>(p);
  if (!d) {
    return;
  }
  g_free(d->nevra);
  g_free(d->status_text);
  g_free(d);
}

// -----------------------------------------------------------------------------
// Release the text payload returned by the background package details task.
// -----------------------------------------------------------------------------
static void
info_task_result_free(gpointer p)
{
  InfoTaskResult *r = static_cast<InfoTaskResult *>(p);
  if (!r) {
    return;
  }

  g_free(r->info);
  g_free(r->files);
  g_free(r->deps);
  g_free(r);
}

// -----------------------------------------------------------------------------
// Free data owned by one changelog task.
// -----------------------------------------------------------------------------
static void
changelog_task_data_free(gpointer p)
{
  ChangelogTaskData *d = static_cast<ChangelogTaskData *>(p);
  if (!d) {
    return;
  }

  g_free(d->nevra);
  g_free(d);
}

// -----------------------------------------------------------------------------
// Complete the package details task when the user cancels the current request.
// -----------------------------------------------------------------------------
static void
return_package_details_task_cancelled(GTask *task)
{
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("Package info load was cancelled."));
}

// -----------------------------------------------------------------------------
// Replace text in a details panel buffer.
// -----------------------------------------------------------------------------
static void
set_details_text(GtkTextBuffer *buffer, const char *text)
{
  if (!buffer) {
    return;
  }

  gtk_text_buffer_set_text(buffer, text ? text : "", -1);
}

// -----------------------------------------------------------------------------
// Reset the details panel after repopulating the main package view.
// -----------------------------------------------------------------------------
void
package_details_reset_details_view(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  widgets->results.changelog_loaded_nevra.clear();
  set_details_text(widgets->results.details_buffer, _("Select a package for details."));
  set_details_text(widgets->results.files_buffer, _("Select an installed package to view its file list."));
  set_details_text(widgets->results.deps_buffer, _("Select a package to view dependencies."));
  set_details_text(widgets->results.changelog_buffer, _("Select a package to view its changelog."));
}

// -----------------------------------------------------------------------------
// Disable transaction actions when no package row is currently selected.
// -----------------------------------------------------------------------------
void
package_details_clear_selected_package_state(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  widgets->results.selected_nevra.clear();
  widgets->results.changelog_loaded_nevra.clear();
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.install_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.remove_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.reinstall_button), FALSE);
  ui_helpers_update_action_button_labels(widgets, "");
}

// -----------------------------------------------------------------------------
// Stop the active package details load, if one is still running.
// -----------------------------------------------------------------------------
void
package_details_cancel_active_load(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  if (widgets->results.package_details_cancellable) {
    g_cancellable_cancel(widgets->results.package_details_cancellable);
    g_object_unref(widgets->results.package_details_cancellable);
    widgets->results.package_details_cancellable = nullptr;
  }

  if (widgets->results.package_changelog_cancellable) {
    g_cancellable_cancel(widgets->results.package_changelog_cancellable);
    g_object_unref(widgets->results.package_changelog_cancellable);
    widgets->results.package_changelog_cancellable = nullptr;
  }
}

// -----------------------------------------------------------------------------
// Enable only the transaction actions that make sense for the selected row.
// -----------------------------------------------------------------------------
static void
update_selected_package_actions(MainWindowUiState *widgets, const PackageRow &selected)
{
  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(selected);

  // Install and upgrade use the available package row.
  // Remove and reinstall use the installed package row.
  // Self-protected packages stay viewable, but the running app must not remove
  // or replace the RPM that owns its current executable.
  bool self_protected =
      action_rows.has_installed_row && dnf_backend_is_package_self_protected(action_rows.installed_row);

  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.install_button),
                           action_rows.has_install_row && !self_protected);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.remove_button),
                           action_rows.has_installed_row && !self_protected);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.reinstall_button),
                           action_rows.can_try_reinstall && !self_protected);

  const std::string install_nevra = action_rows.has_install_row ? action_rows.install_row.nevra : selected.nevra;
  const std::string installed_nevra = action_rows.has_installed_row ? action_rows.installed_row.nevra : selected.nevra;
  ui_helpers_update_action_button_labels_for_selection(
      widgets, install_nevra, installed_nevra, installed_nevra, action_rows.install_is_upgrade);
}

// -----------------------------------------------------------------------------
// Load package detail text on a worker thread.
// -----------------------------------------------------------------------------
static void
on_package_details_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    return_package_details_task_cancelled(task);
    return;
  }

  InfoTaskData *td = static_cast<InfoTaskData *>(task_data);
  try {
    DNFUI_TRACE("Package info task start nevra=%s", td ? td->nevra : "");
    InfoTaskResult *result = static_cast<InfoTaskResult *>(g_malloc0(sizeof *result));

    result->info = g_strdup(dnf_backend_get_package_info(td->nevra).c_str());
    DNFUI_TRACE("Package info details loaded nevra=%s bytes=%zu",
                td ? td->nevra : "",
                result->info ? std::strlen(result->info) : 0);

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      info_task_result_free(result);
      return_package_details_task_cancelled(task);
      return;
    }

    try {
      DNFUI_TRACE("Package info files load start nevra=%s", td ? td->nevra : "");
      // NOTE: Limit displayed files so very large file lists can still be copied.
      result->files = g_strdup(dnf_backend_get_installed_package_files(td->nevra, 1500).c_str());
      DNFUI_TRACE("Package info files loaded nevra=%s bytes=%zu",
                  td ? td->nevra : "",
                  result->files ? std::strlen(result->files) : 0);
    } catch (const std::exception &e) {
      result->files = g_strdup(e.what());
      DNFUI_TRACE("Package info files failed nevra=%s error=%s", td ? td->nevra : "", e.what());
    }

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      info_task_result_free(result);
      return_package_details_task_cancelled(task);
      return;
    }

    try {
      DNFUI_TRACE("Package info dependencies load start nevra=%s", td ? td->nevra : "");
      result->deps = g_strdup(dnf_backend_get_package_deps(td->nevra).c_str());
      DNFUI_TRACE("Package info dependencies loaded nevra=%s bytes=%zu",
                  td ? td->nevra : "",
                  result->deps ? std::strlen(result->deps) : 0);
    } catch (const std::exception &e) {
      result->deps = g_strdup(e.what());
      DNFUI_TRACE("Package info dependencies failed nevra=%s error=%s", td ? td->nevra : "", e.what());
    }

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      info_task_result_free(result);
      return_package_details_task_cancelled(task);
      return;
    }

    DNFUI_TRACE("Package info task done nevra=%s", td ? td->nevra : "");
    g_task_return_pointer(task, result, info_task_result_free);
  } catch (const std::exception &e) {
    DNFUI_TRACE("Package info task failed nevra=%s error=%s", td ? td->nevra : "", e.what());
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Update the details panel after package text has loaded.
// -----------------------------------------------------------------------------
static void
on_package_details_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  const InfoTaskData *td = static_cast<const InfoTaskData *>(g_task_get_task_data(task));
  GError *error = nullptr;
  InfoTaskResult *result = static_cast<InfoTaskResult *>(g_task_propagate_pointer(task, &error));

  if (!td) {
    if (result) {
      info_task_result_free(result);
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  GCancellable *c = g_task_get_cancellable(task);
  if (c && widgets->results.package_details_cancellable == c) {
    g_object_unref(widgets->results.package_details_cancellable);
    widgets->results.package_details_cancellable = nullptr;
  }

  if (td->generation != BaseManager::instance().current_generation() || widgets->results.selected_nevra != td->nevra) {
    if (result) {
      info_task_result_free(result);
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (!result) {
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : _("Error loading info."), "red");
    if (error) {
      g_error_free(error);
    }
    return;
  }

  // Show the row status even when the Status column is hidden.
  const char *info_text = result->info ? result->info : _("No details found.");
  std::string details_text;
  if (td->status_text && *td->status_text) {
    details_text = _("Status");
    details_text += ": ";
    details_text += td->status_text;
    details_text += "\n\n";
    details_text += info_text;
    info_text = details_text.c_str();
  }

  // Display general package information.
  set_details_text(widgets->results.details_buffer, info_text);

  // Display the file list fetched by the background task.
  set_details_text(widgets->results.files_buffer,
                   result->files ? result->files : _("Select an installed package to view its file list."));

  // Display dependencies fetched by the background task.
  set_details_text(widgets->results.deps_buffer,
                   result->deps ? result->deps : _("Select a package to view dependencies."));

  if (!widgets->results.package_changelog_cancellable &&
      widgets->results.changelog_loaded_nevra != widgets->results.selected_nevra) {
    set_details_text(widgets->results.changelog_buffer, _("Open the Changelog tab to load the changelog."));
  }

  ui_helpers_set_status(widgets->query.status_label, _("Package info loaded."), "green");
  info_task_result_free(result);
}

// -----------------------------------------------------------------------------
// Load changelog text only when the user opens the Changelog tab.
// -----------------------------------------------------------------------------
static void
on_changelog_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    return_package_details_task_cancelled(task);
    return;
  }

  ChangelogTaskData *td = static_cast<ChangelogTaskData *>(task_data);
  try {
    DNFUI_TRACE("Package changelog load start nevra=%s", td ? td->nevra : "");
    std::string changelog = dnf_backend_get_package_changelog(td->nevra);
    DNFUI_TRACE("Package changelog loaded nevra=%s bytes=%zu", td ? td->nevra : "", changelog.size());
    g_task_return_pointer(task, g_strdup(changelog.c_str()), g_free);
  } catch (const std::exception &e) {
    DNFUI_TRACE("Package changelog failed nevra=%s error=%s", td ? td->nevra : "", e.what());
    g_task_return_pointer(task, g_strdup(e.what()), g_free);
  }
}

// -----------------------------------------------------------------------------
// Show changelog text if it still belongs to the selected package.
// -----------------------------------------------------------------------------
static void
on_changelog_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  const ChangelogTaskData *td = static_cast<const ChangelogTaskData *>(g_task_get_task_data(task));
  GError *error = nullptr;
  char *changelog = static_cast<char *>(g_task_propagate_pointer(task, &error));

  GCancellable *c = g_task_get_cancellable(task);
  if (c && widgets->results.package_changelog_cancellable == c) {
    g_object_unref(widgets->results.package_changelog_cancellable);
    widgets->results.package_changelog_cancellable = nullptr;
  }

  if (!td || td->generation != BaseManager::instance().current_generation() ||
      widgets->results.selected_nevra != td->nevra) {
    g_free(changelog);
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (!changelog) {
    set_details_text(widgets->results.changelog_buffer, error ? error->message : _("Error loading changelog."));
    if (error) {
      g_error_free(error);
    }
    return;
  }

  widgets->results.changelog_loaded_nevra = td->nevra;
  set_details_text(widgets->results.changelog_buffer, changelog);
  g_free(changelog);
}

// -----------------------------------------------------------------------------
// Start changelog loading for the selected package if the tab needs it.
// -----------------------------------------------------------------------------
static void
load_selected_package_changelog(MainWindowUiState *widgets)
{
  if (!widgets || widgets->results.selected_nevra.empty()) {
    return;
  }
  PackageRow selected;
  if (!package_table_get_selected_package_row(widgets, selected) || selected.nevra != widgets->results.selected_nevra) {
    return;
  }
  if (widgets->results.package_changelog_cancellable ||
      widgets->results.changelog_loaded_nevra == widgets->results.selected_nevra) {
    return;
  }

  set_details_text(widgets->results.changelog_buffer, _("Fetching changelog..."));

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_changelog_task_finished);
  widgets->results.package_changelog_cancellable = G_CANCELLABLE(g_object_ref(c));

  ChangelogTaskData *td = static_cast<ChangelogTaskData *>(g_malloc0(sizeof *td));
  td->nevra = g_strdup(widgets->results.selected_nevra.c_str());
  td->generation = BaseManager::instance().current_generation();
  g_task_set_task_data(task, td, changelog_task_data_free);

  g_task_run_in_thread(task, on_changelog_task);

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start the async package details load for the newly selected package row.
// -----------------------------------------------------------------------------
void
package_details_load_selected_package_info(MainWindowUiState *widgets, const PackageRow &selected)
{
  if (!widgets) {
    return;
  }

  package_details_cancel_active_load(widgets);

  widgets->results.selected_nevra = selected.nevra;
  widgets->results.changelog_loaded_nevra.clear();
  ui_helpers_set_status(widgets->query.status_label, _("Fetching package info..."), "blue");
  update_selected_package_actions(widgets, selected);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_package_details_task_finished);
  widgets->results.package_details_cancellable = G_CANCELLABLE(g_object_ref(c));

  // Pass selected row state to the background task.
  InfoTaskData *td = static_cast<InfoTaskData *>(g_malloc0(sizeof *td));
  td->nevra = g_strdup(selected.nevra.c_str());
  td->status_text = g_strdup(package_table_status_text(dnf_backend_get_package_install_state(selected)));
  td->generation = BaseManager::instance().current_generation();
  g_task_set_task_data(task, td, info_task_data_free);

  // Run background task to fetch metadata using dnf_backend
  g_task_run_in_thread(task, on_package_details_task);

  g_object_unref(task);
  g_object_unref(c);

  if (widgets->results.details_stack &&
      g_strcmp0(gtk_stack_get_visible_child_name(widgets->results.details_stack), "changelog") == 0) {
    load_selected_package_changelog(widgets);
  }
}

// -----------------------------------------------------------------------------
// Load deferred details content when the user opens that tab.
// -----------------------------------------------------------------------------
void
package_details_on_details_page_changed(GtkStack *stack, GParamSpec *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (!widgets || widgets->window_state.destroyed) {
    return;
  }

  if (g_strcmp0(gtk_stack_get_visible_child_name(stack), "changelog") == 0) {
    load_selected_package_changelog(widgets);
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
