// -----------------------------------------------------------------------------
// src/ui/details/package_details_controller.cpp
// Package selection and details panel controller
// Handles package selection, action-button sensitivity, and background loading for the package details tabs.
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

#include <optional>
#include <string>

// Task data for one package details load.
// Snapshot generation at dispatch time.
// Outdated results can be dropped after a Base rebuild.
struct InfoTaskData {
  std::string selected_nevra;
  std::string details_query_nevra;
  std::string status_text;
  std::optional<PackageRow> upgrade_row_override;
  uint64_t generation;
};

// Details tabs that load only after the user opens them.
enum class DeferredDetailsPage {
  FILES,
  DEPENDENCIES,
  CHANGELOG,
};

// Task data for one deferred details load.
// The selected NEVRA and generation are checked before the result is shown.
struct DeferredDetailsTaskData {
  std::string selected_nevra;
  std::string details_query_nevra;
  uint64_t generation;
  DeferredDetailsPage page;
};

// Widgets and request state used by one deferred details tab.
struct DeferredDetailsUiState {
  GtkTextBuffer *buffer = nullptr;
  std::string *loaded_nevra = nullptr;
  GCancellable **cancellable = nullptr;
  const char *loading_text = nullptr;
  const char *error_text = nullptr;
};

static void load_visible_package_details_page(MainWindowUiState *widgets, GtkStack *stack);

// -----------------------------------------------------------------------------
// Build a package row from the daemon target attached to one List Upgradable row.
// -----------------------------------------------------------------------------
static PackageRow
package_row_from_upgrade_target(const TransactionServiceUpgradeTarget &target)
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
// Free data owned by one package details task.
// -----------------------------------------------------------------------------
static void
info_task_data_free(gpointer p)
{
  delete static_cast<InfoTaskData *>(p);
}

// -----------------------------------------------------------------------------
// Free data owned by one deferred details task.
// -----------------------------------------------------------------------------
static void
deferred_details_task_data_free(gpointer p)
{
  delete static_cast<DeferredDetailsTaskData *>(p);
}

// -----------------------------------------------------------------------------
// Return the widgets and state owned by one deferred details tab.
// -----------------------------------------------------------------------------
static DeferredDetailsUiState
deferred_details_ui_state(MainWindowUiState *widgets, DeferredDetailsPage page)
{
  if (!widgets) {
    return {};
  }

  switch (page) {
  case DeferredDetailsPage::FILES:
    return { widgets->results.files_buffer,
             &widgets->results.files_loaded_nevra,
             &widgets->results.package_files_cancellable,
             _("Fetching file list..."),
             _("Error loading file list.") };
  case DeferredDetailsPage::DEPENDENCIES:
    return { widgets->results.deps_buffer,
             &widgets->results.deps_loaded_nevra,
             &widgets->results.package_deps_cancellable,
             _("Fetching dependencies..."),
             _("Error loading dependencies.") };
  case DeferredDetailsPage::CHANGELOG:
    return { widgets->results.changelog_buffer,
             &widgets->results.changelog_loaded_nevra,
             &widgets->results.package_changelog_cancellable,
             _("Fetching changelog..."),
             _("Error loading changelog.") };
  }

  return {};
}

// -----------------------------------------------------------------------------
// Complete the package details task when the user cancels the current request.
// -----------------------------------------------------------------------------
static void
return_package_details_task_cancelled(GTask *task)
{
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("Package details load was cancelled."));
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
// Insert the UI row status between package metadata and summary text.
// -----------------------------------------------------------------------------
static std::string
package_info_text_with_status(const char *info_text, const char *status_text)
{
  std::string text = info_text ? info_text : _("No details found.");
  if (!status_text || !*status_text) {
    return text;
  }

  std::string status_line = _("Status");
  status_line += ": ";
  status_line += status_text;

  size_t summary_separator = text.find("\n\n");
  if (summary_separator == std::string::npos) {
    text += "\n";
    text += status_line;
    return text;
  }
  text.insert(summary_separator, "\n\n" + status_line);

  return text;
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

  widgets->results.files_loaded_nevra.clear();
  widgets->results.deps_loaded_nevra.clear();
  widgets->results.changelog_loaded_nevra.clear();
  widgets->results.details_query_nevra.clear();
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
  widgets->results.details_query_nevra.clear();
  widgets->results.files_loaded_nevra.clear();
  widgets->results.deps_loaded_nevra.clear();
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

  if (widgets->results.package_files_cancellable) {
    g_cancellable_cancel(widgets->results.package_files_cancellable);
    g_object_unref(widgets->results.package_files_cancellable);
    widgets->results.package_files_cancellable = nullptr;
  }

  if (widgets->results.package_deps_cancellable) {
    g_cancellable_cancel(widgets->results.package_deps_cancellable);
    g_object_unref(widgets->results.package_deps_cancellable);
    widgets->results.package_deps_cancellable = nullptr;
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
update_selected_package_actions(MainWindowUiState *widgets, const PackageTableRow &selected)
{
  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
      selected.row, selected.upgrade_target ? &selected.upgrade_target.value() : nullptr, selected.upgrade_generation);

  // Install and upgrade use the available package row.
  // Remove and reinstall use the installed package row.
  // Self-protected packages stay viewable, but the running app must not remove
  // or replace the RPM that owns its current executable.
  bool self_protected =
      action_rows.has_installed_row && dnf_backend_is_package_self_protected(action_rows.installed_row);
  bool install_blocked = pending_transaction_install_action_blocked_by_self_protection(action_rows, self_protected);

  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.install_button),
                           action_rows.has_install_row && !install_blocked);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.remove_button),
                           action_rows.has_installed_row && !self_protected);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.reinstall_button),
                           action_rows.can_try_reinstall && !self_protected);

  const std::string install_nevra = action_rows.has_install_row ? action_rows.install_row.nevra : selected.row.nevra;
  const std::string installed_nevra =
      action_rows.has_installed_row ? action_rows.installed_row.nevra : selected.row.nevra;
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
    DNFUI_TRACE("Package info task start nevra=%s", td ? td->details_query_nevra.c_str() : "");
    std::string info = td && td->upgrade_row_override.has_value()
        ? dnf_backend_get_package_info(td->details_query_nevra, &td->upgrade_row_override.value())
        : dnf_backend_get_package_info(td ? td->details_query_nevra : "");
    DNFUI_TRACE(
        "Package info details loaded nevra=%s bytes=%zu", td ? td->details_query_nevra.c_str() : "", info.size());

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      return_package_details_task_cancelled(task);
      return;
    }

    DNFUI_TRACE("Package info task done nevra=%s", td ? td->details_query_nevra.c_str() : "");
    g_task_return_pointer(task, g_strdup(info.c_str()), g_free);
  } catch (const std::exception &e) {
    DNFUI_TRACE("Package info task failed nevra=%s error=%s", td ? td->details_query_nevra.c_str() : "", e.what());
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
  char *info = static_cast<char *>(g_task_propagate_pointer(task, &error));

  if (!td) {
    g_free(info);
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

  if (td->generation != BaseManager::instance().current_generation() ||
      widgets->results.selected_nevra != td->selected_nevra ||
      widgets->results.details_query_nevra != td->details_query_nevra) {
    g_free(info);
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (!info) {
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : _("Error loading info."), "red");
    if (error) {
      g_error_free(error);
    }
    return;
  }

  // Show the row status even when the Status column is hidden.
  std::string info_text = package_info_text_with_status(info, td->status_text.c_str());

  // Display general package information.
  set_details_text(widgets->results.details_buffer, info_text.c_str());

  ui_helpers_set_status(widgets->query.status_label, _("Package info loaded."), "green");
  g_free(info);

  if (widgets->results.details_stack) {
    load_visible_package_details_page(widgets, widgets->results.details_stack);
  }
}

// -----------------------------------------------------------------------------
// Load text for one deferred details tab on a worker thread.
// -----------------------------------------------------------------------------
static void
on_deferred_details_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    return_package_details_task_cancelled(task);
    return;
  }

  DeferredDetailsTaskData *td = static_cast<DeferredDetailsTaskData *>(task_data);
  try {
    std::string text;
    switch (td->page) {
    case DeferredDetailsPage::FILES:
      DNFUI_TRACE("Package file list load start nevra=%s", td->details_query_nevra.c_str());
      // NOTE: Limit displayed files so very large file lists can still be copied.
      text = dnf_backend_get_installed_package_files(td->details_query_nevra, 1500);
      DNFUI_TRACE("Package file list loaded nevra=%s bytes=%zu", td->details_query_nevra.c_str(), text.size());
      break;
    case DeferredDetailsPage::DEPENDENCIES:
      DNFUI_TRACE("Package dependencies load start nevra=%s", td->details_query_nevra.c_str());
      text = dnf_backend_get_package_deps(td->details_query_nevra);
      DNFUI_TRACE("Package dependencies loaded nevra=%s bytes=%zu", td->details_query_nevra.c_str(), text.size());
      break;
    case DeferredDetailsPage::CHANGELOG:
      DNFUI_TRACE("Package changelog load start nevra=%s", td->details_query_nevra.c_str());
      text = dnf_backend_get_package_changelog(td->details_query_nevra);
      DNFUI_TRACE("Package changelog loaded nevra=%s bytes=%zu", td->details_query_nevra.c_str(), text.size());
      break;
    }

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      return_package_details_task_cancelled(task);
      return;
    }

    g_task_return_pointer(task, g_strdup(text.c_str()), g_free);
  } catch (const std::exception &e) {
    DNFUI_TRACE(
        "Deferred package details load failed nevra=%s error=%s", td ? td->details_query_nevra.c_str() : "", e.what());
    g_task_return_pointer(task, g_strdup(e.what()), g_free);
  }
}

// -----------------------------------------------------------------------------
// Show deferred text if it still belongs to the selected package.
// -----------------------------------------------------------------------------
static void
on_deferred_details_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  const DeferredDetailsTaskData *td = static_cast<const DeferredDetailsTaskData *>(g_task_get_task_data(task));
  GError *error = nullptr;
  char *text = static_cast<char *>(g_task_propagate_pointer(task, &error));

  if (!td) {
    g_free(text);
    if (error) {
      g_error_free(error);
    }
    return;
  }

  DeferredDetailsUiState ui = deferred_details_ui_state(widgets, td->page);

  GCancellable *c = g_task_get_cancellable(task);
  if (c && ui.cancellable && *ui.cancellable == c) {
    g_object_unref(*ui.cancellable);
    *ui.cancellable = nullptr;
  }

  if (td->generation != BaseManager::instance().current_generation() ||
      widgets->results.selected_nevra != td->selected_nevra ||
      widgets->results.details_query_nevra != td->details_query_nevra) {
    g_free(text);
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (!text) {
    set_details_text(ui.buffer, error ? error->message : ui.error_text);
    if (error) {
      g_error_free(error);
    }
    return;
  }

  *ui.loaded_nevra = td->details_query_nevra;
  set_details_text(ui.buffer, text);
  g_free(text);
}

// -----------------------------------------------------------------------------
// Start loading one deferred tab for the selected package if it needs data.
// -----------------------------------------------------------------------------
static void
load_selected_package_details_page(MainWindowUiState *widgets, DeferredDetailsPage page)
{
  if (!widgets || widgets->results.selected_nevra.empty() || widgets->results.details_query_nevra.empty() ||
      widgets->results.package_details_cancellable) {
    return;
  }
  PackageTableRow selected;
  if (!package_table_get_selected_package(widgets, selected) || selected.row.nevra != widgets->results.selected_nevra) {
    return;
  }

  DeferredDetailsUiState ui = deferred_details_ui_state(widgets, page);
  if (!ui.buffer || !ui.loaded_nevra || !ui.cancellable || *ui.cancellable ||
      *ui.loaded_nevra == widgets->results.details_query_nevra) {
    return;
  }

  set_details_text(ui.buffer, ui.loading_text);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_deferred_details_task_finished);
  *ui.cancellable = G_CANCELLABLE(g_object_ref(c));

  DeferredDetailsTaskData *td = new DeferredDetailsTaskData;
  td->selected_nevra = widgets->results.selected_nevra;
  td->details_query_nevra = widgets->results.details_query_nevra;
  td->generation = BaseManager::instance().current_generation();
  td->page = page;
  g_task_set_task_data(task, td, deferred_details_task_data_free);

  g_task_run_in_thread(task, on_deferred_details_task);

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Load deferred content for the currently visible details tab.
// -----------------------------------------------------------------------------
static void
load_visible_package_details_page(MainWindowUiState *widgets, GtkStack *stack)
{
  const char *page = stack ? gtk_stack_get_visible_child_name(stack) : nullptr;
  if (g_strcmp0(page, "files") == 0) {
    load_selected_package_details_page(widgets, DeferredDetailsPage::FILES);
  } else if (g_strcmp0(page, "dependencies") == 0) {
    load_selected_package_details_page(widgets, DeferredDetailsPage::DEPENDENCIES);
  } else if (g_strcmp0(page, "changelog") == 0) {
    load_selected_package_details_page(widgets, DeferredDetailsPage::CHANGELOG);
  }
}

// -----------------------------------------------------------------------------
// Start the async package details load for the newly selected package row.
// -----------------------------------------------------------------------------
void
package_details_load_selected_package_info(MainWindowUiState *widgets, const PackageTableRow &selected)
{
  if (!widgets) {
    return;
  }

  package_details_cancel_active_load(widgets);

  std::string details_query_nevra = selected.row.nevra;
  std::optional<PackageRow> upgrade_row_override;
  if (selected.upgrade_target.has_value()) {
    upgrade_row_override = package_row_from_upgrade_target(selected.upgrade_target.value());
    PackageRow installed_row;
    if (dnf_backend_get_installed_package_row_by_name_arch(selected.row, installed_row)) {
      details_query_nevra = installed_row.nevra;
    }
  }

  widgets->results.selected_nevra = selected.row.nevra;
  widgets->results.details_query_nevra = details_query_nevra;
  widgets->results.files_loaded_nevra.clear();
  widgets->results.deps_loaded_nevra.clear();
  widgets->results.changelog_loaded_nevra.clear();
  set_details_text(widgets->results.files_buffer, _("Open the Files tab to load the file list."));
  set_details_text(widgets->results.deps_buffer, _("Open the Dependencies tab to load dependencies."));
  set_details_text(widgets->results.changelog_buffer, _("Open the Changelog tab to load the changelog."));
  ui_helpers_set_status(widgets->query.status_label, _("Fetching package info..."), "blue");
  update_selected_package_actions(widgets, selected);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_package_details_task_finished);
  widgets->results.package_details_cancellable = G_CANCELLABLE(g_object_ref(c));

  // Pass selected row state to the background task.
  PackageInstallState selected_state = selected.upgrade_target.has_value()
      ? PackageInstallState::UPGRADEABLE
      : dnf_backend_get_package_install_state(selected.row);

  InfoTaskData *td = new InfoTaskData;
  td->selected_nevra = selected.row.nevra;
  td->details_query_nevra = details_query_nevra;
  td->status_text = package_table_status_text(selected_state);
  td->upgrade_row_override = upgrade_row_override;
  td->generation = BaseManager::instance().current_generation();
  g_task_set_task_data(task, td, info_task_data_free);

  // Run background task to fetch metadata using dnf_backend
  g_task_run_in_thread(task, on_package_details_task);

  g_object_unref(task);
  g_object_unref(c);
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

  load_visible_package_details_page(widgets, stack);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
