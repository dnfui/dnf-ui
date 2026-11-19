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
#include "ui/transaction/pending_transaction_view.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"
#include "ui/package_query/package_query_controller.hpp"
#include "ui/package_query/package_query_state.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/package_table/package_table_status.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Task data for one package details load.
// Snapshot generation at dispatch time.
// Outdated results can be dropped after a Base rebuild.
struct InfoTaskData {
  std::string selected_nevra;
  std::string details_query_nevra;
  std::optional<PackageRow> upgrade_row_override;
  PackageDetailsContext details_context = PackageDetailsContext::INSTALLED_CONTEXT;
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
  PackageDetailsContext details_context = PackageDetailsContext::INSTALLED_CONTEXT;
  uint64_t generation;
  DeferredDetailsPage page;
  std::optional<std::vector<std::string>> file_paths;
  std::vector<PackageRequirementLink> requirement_links;
};

// Widgets and request state used by one deferred details tab.
struct DeferredDetailsUiState {
  GtkTextBuffer *buffer = nullptr;
  GtkStringList *files_model = nullptr;
  std::string *loaded_nevra = nullptr;
  GCancellable **cancellable = nullptr;
  const char *loading_text = nullptr;
  const char *error_text = nullptr;
};

static void load_visible_package_details_page(MainWindowUiState *widgets, GtkStack *stack);

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
    return { nullptr,
             widgets->results.files_model,
             &widgets->results.files_loaded_nevra,
             &widgets->results.package_files_cancellable,
             _("Fetching file list..."),
             _("Error loading file list.") };
  case DeferredDetailsPage::DEPENDENCIES:
    return { widgets->results.deps_buffer,        nullptr,
             &widgets->results.deps_loaded_nevra, &widgets->results.package_deps_cancellable,
             _("Fetching dependencies..."),       _("Error loading dependencies.") };
  case DeferredDetailsPage::CHANGELOG:
    return { widgets->results.changelog_buffer,
             nullptr,
             &widgets->results.changelog_loaded_nevra,
             &widgets->results.package_changelog_cancellable,
             _("Fetching changelog..."),
             _("Error loading changelog.") };
  }

  return {};
}

// -----------------------------------------------------------------------------
// Return the NEVRA used to load one deferred details tab.
// -----------------------------------------------------------------------------
static const std::string *
deferred_details_query_nevra(const MainWindowUiState *widgets, DeferredDetailsPage page)
{
  if (!widgets) {
    return nullptr;
  }

  if (page == DeferredDetailsPage::CHANGELOG) {
    return &widgets->results.changelog_query_nevra;
  }

  return &widgets->results.details_query_nevra;
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

static void
set_package_description_header(MainWindowUiState *widgets, const std::string &name, const std::string &summary)
{
  if (!widgets || !widgets->results.description_header) {
    return;
  }

  gtk_label_set_text(widgets->results.description_name_label, name.c_str());
  gtk_label_set_text(widgets->results.description_summary_label, summary.c_str());
  gtk_widget_set_visible(widgets->results.description_header, !name.empty());
}

static void
set_package_copy_button_sensitive(MainWindowUiState *widgets, bool available)
{
  if (!widgets) {
    return;
  }

  if (widgets->results.copy_description_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->results.copy_description_button), available);
  }
}

// -----------------------------------------------------------------------------
// Replace the file list in one model update.
// -----------------------------------------------------------------------------
static void
set_files_text(GtkStringList *model, const char *text)
{
  if (!model) {
    return;
  }

  char **lines = g_strsplit(text ? text : "", "\n", -1);
  std::vector<const char *> entries;
  for (char **line = lines; *line; ++line) {
    if ((*line)[0] == '\0' && line[1] == nullptr) {
      break;
    }
    entries.push_back(*line);
  }
  entries.push_back(nullptr);

  guint current_count = g_list_model_get_n_items(G_LIST_MODEL(model));
  gtk_string_list_splice(model, 0, current_count, entries.data());
  g_strfreev(lines);
}

// -----------------------------------------------------------------------------
// Replace the file list with individual paths from the backend.
// -----------------------------------------------------------------------------
static void
set_files_paths(GtkStringList *model, const std::vector<std::string> &paths)
{
  std::vector<const char *> entries;
  entries.reserve(paths.size() + 1);
  for (const std::string &path : paths) {
    entries.push_back(path.c_str());
  }
  entries.push_back(nullptr);

  guint current_count = g_list_model_get_n_items(G_LIST_MODEL(model));
  gtk_string_list_splice(model, 0, current_count, entries.data());
}

// -----------------------------------------------------------------------------
// Mark requirements that can open a provider search.
// -----------------------------------------------------------------------------
static void
apply_requirement_link_tags(GtkTextBuffer *buffer, const std::vector<PackageRequirementLink> &links)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  GtkTextTag *tag = gtk_text_tag_table_lookup(table, "requirement-link");
  if (!tag) {
    return;
  }

  for (const auto &link : links) {
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(buffer, &start, link.start);
    gtk_text_buffer_get_iter_at_offset(buffer, &end, link.end);
    gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
  }
}

// -----------------------------------------------------------------------------
// Find the linked requirement under a position in the Dependencies tab.
// -----------------------------------------------------------------------------
static const PackageRequirementLink *
requirement_link_at_position(MainWindowUiState *widgets, gdouble x, gdouble y)
{
  GtkTextView *view = widgets->results.deps_view;
  int buffer_x = 0;
  int buffer_y = 0;
  gtk_text_view_window_to_buffer_coords(
      view, GTK_TEXT_WINDOW_WIDGET, static_cast<int>(x), static_cast<int>(y), &buffer_x, &buffer_y);

  GtkTextIter iter;
  if (!gtk_text_view_get_iter_at_position(view, &iter, nullptr, buffer_x, buffer_y)) {
    return nullptr;
  }

  int offset = gtk_text_iter_get_offset(&iter);
  for (const auto &link : widgets->results.requirement_links) {
    if (offset >= link.start && offset < link.end) {
      return &link;
    }
  }
  return nullptr;
}

// -----------------------------------------------------------------------------
// Show the pointer cursor only over linked requirements.
// -----------------------------------------------------------------------------
static void
on_requirement_motion(GtkEventControllerMotion *, gdouble x, gdouble y, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  GtkWidget *view = GTK_WIDGET(widgets->results.deps_view);
  bool linked =
      gtk_widget_is_sensitive(GTK_WIDGET(widgets->query.mode_dropdown)) && requirement_link_at_position(widgets, x, y);
  const char *cursor_name = linked ? "pointer" : "text";
  GdkCursor *current = gtk_widget_get_cursor(view);
  if (g_strcmp0(current ? gdk_cursor_get_name(current) : nullptr, cursor_name) != 0) {
    gtk_widget_set_cursor_from_name(view, cursor_name);
  }
}

// -----------------------------------------------------------------------------
// Restore the usual cursor when the pointer leaves the Dependencies tab.
// -----------------------------------------------------------------------------
static void
on_requirement_motion_left(GtkEventControllerMotion *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  gtk_widget_set_cursor_from_name(GTK_WIDGET(widgets->results.deps_view), nullptr);
}

// -----------------------------------------------------------------------------
// Clear a pending click when the displayed dependencies change.
// -----------------------------------------------------------------------------
static void
on_dependencies_changed(GtkTextBuffer *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  widgets->results.pressed_requirement_start = -1;
  gtk_widget_set_cursor_from_name(GTK_WIDGET(widgets->results.deps_view), "text");
}

// -----------------------------------------------------------------------------
// Record where a link click began.
// -----------------------------------------------------------------------------
static void
on_requirement_pressed(GtkGestureClick *, gint n_press, gdouble x, gdouble y, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  widgets->results.pressed_requirement_start = -1;
  if (n_press != 1 || !gtk_widget_is_sensitive(GTK_WIDGET(widgets->query.mode_dropdown))) {
    return;
  }

  const PackageRequirementLink *link = requirement_link_at_position(widgets, x, y);
  if (link) {
    widgets->results.pressed_requirement_start = link->start;
    widgets->results.pressed_requirement_x = static_cast<int>(x);
    widgets->results.pressed_requirement_y = static_cast<int>(y);
  }
}

// -----------------------------------------------------------------------------
// Find packages that provide a clicked requirement.
// -----------------------------------------------------------------------------
static void
on_requirement_released(GtkGestureClick *, gint n_press, gdouble x, gdouble y, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  int pressed_start = widgets->results.pressed_requirement_start;
  widgets->results.pressed_requirement_start = -1;
  if (n_press != 1 || pressed_start < 0 || !gtk_widget_is_sensitive(GTK_WIDGET(widgets->query.mode_dropdown)) ||
      gtk_drag_check_threshold(GTK_WIDGET(widgets->results.deps_view),
                               widgets->results.pressed_requirement_x,
                               widgets->results.pressed_requirement_y,
                               static_cast<int>(x),
                               static_cast<int>(y))) {
    return;
  }

  const PackageRequirementLink *link = requirement_link_at_position(widgets, x, y);
  if (!link || link->start != pressed_start) {
    return;
  }

  std::string requirement = link->requirement;
  const DnfBackendSearchMode mode = package_query_mode_for_requirement(requirement);
  gtk_drop_down_set_selected(widgets->query.mode_dropdown, mode == DnfBackendSearchMode::FILES ? 2 : 1);
  gtk_editable_set_text(GTK_EDITABLE(widgets->query.entry), requirement.c_str());
  package_query_on_search_button_clicked(nullptr, widgets);
}

// -----------------------------------------------------------------------------
// Connect provider links in the Dependencies tab.
// -----------------------------------------------------------------------------
void
package_details_connect_requirement_links(MainWindowUiState *widgets)
{
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect(click, "pressed", G_CALLBACK(on_requirement_pressed), widgets);
  g_signal_connect(click, "released", G_CALLBACK(on_requirement_released), widgets);
  gtk_widget_add_controller(GTK_WIDGET(widgets->results.deps_view), GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(on_requirement_motion), widgets);
  g_signal_connect(motion, "leave", G_CALLBACK(on_requirement_motion_left), widgets);
  gtk_widget_add_controller(GTK_WIDGET(widgets->results.deps_view), motion);
  g_signal_connect(widgets->results.deps_buffer, "changed", G_CALLBACK(on_dependencies_changed), widgets);
}

// -----------------------------------------------------------------------------
// Replace the content of one deferred details tab.
// -----------------------------------------------------------------------------
static void
set_deferred_details_text(const DeferredDetailsUiState &ui, const char *text)
{
  if (ui.files_model) {
    set_files_text(ui.files_model, text);
  } else {
    set_details_text(ui.buffer, text);
  }
}

// -----------------------------------------------------------------------------
// Show or hide the selected package Status label in the Description tab.
// -----------------------------------------------------------------------------
static void
set_details_status_text(MainWindowUiState *widgets, const char *status_text)
{
  if (!widgets || !widgets->results.details_status_box || !widgets->results.details_status_label) {
    return;
  }

  if (package_table_column_is_visible(widgets, PackageColumnKind::STATUS) || !status_text || !*status_text) {
    gtk_label_set_text(widgets->results.details_status_label, "");
    gtk_widget_set_visible(widgets->results.details_status_box, FALSE);
    return;
  }

  gtk_label_set_text(widgets->results.details_status_label, status_text);
  gtk_widget_set_visible(widgets->results.details_status_box, TRUE);
}

// -----------------------------------------------------------------------------
// Return the Status text for the selected row and current pending actions.
// -----------------------------------------------------------------------------
static const char *
selected_package_status_text(MainWindowUiState *widgets,
                             const PackageTableRow &selected,
                             const PendingTransactionActionRows &action_rows,
                             std::optional<PendingAction::Type> &pending_action)
{
  PendingAction::Type action_type;
  if (package_table_pending_action_for_resolved_row(widgets, selected, action_rows, action_type)) {
    pending_action = action_type;
    return package_table_pending_action_status_text(action_type, action_rows.state);
  }

  const bool exact_available_rows =
      widgets && displayed_package_query_uses_exact_available_rows(widgets->query_state.displayed_query);
  return package_table_status_text_for_resolved_row(selected, action_rows, exact_available_rows);
}

// -----------------------------------------------------------------------------
// Return the package version context used by the details tabs.
// -----------------------------------------------------------------------------
static PackageDetailsContext
package_details_context_for_selection(const MainWindowUiState *widgets,
                                      const PackageTableRow &selected,
                                      const InstalledPackageResolution &resolution)
{
  if (selected.upgrade_target()) {
    return PackageDetailsContext::INSTALLED_CONTEXT;
  }

  if (resolution.exact_installed) {
    return PackageDetailsContext::INSTALLED_CONTEXT;
  }

  const bool compact_view = widgets && displayed_package_query_uses_compact_rows(widgets->query_state.displayed_query);

  if (compact_view && resolution.state == PackageInstallState::UPGRADEABLE && selected.row.is_newest_available) {
    return PackageDetailsContext::INSTALLED_CONTEXT;
  }

  return PackageDetailsContext::SELECTED_VERSION;
}

// -----------------------------------------------------------------------------
// Return the visible update candidate that should be shown for this selected row.
// -----------------------------------------------------------------------------
static std::optional<PackageRow>
package_details_upgrade_override_for_selection(const PackageTableRow &selected,
                                               const InstalledPackageResolution &resolution)
{
  if (selected.upgrade_target()) {
    return selected.row;
  }

  if (!resolution.exact_installed || selected.row.repo_candidate_relation != PackageRepoCandidateRelation::NEWER ||
      selected.row.repo_candidate_nevra.empty()) {
    return std::nullopt;
  }

  if (selected.row.repo_candidate_is_newest_available) {
    return std::nullopt;
  }

  PackageRow update_row;
  update_row.nevra = selected.row.repo_candidate_nevra;
  update_row.name = selected.row.name;
  update_row.epoch = selected.row.repo_candidate_epoch;
  update_row.version = selected.row.repo_candidate_version;
  update_row.release = selected.row.repo_candidate_release;
  update_row.arch = selected.row.arch;
  update_row.repo = selected.row.repo_candidate_repo;
  return update_row;
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
  widgets->results.changelog_query_nevra.clear();
  set_details_status_text(widgets, nullptr);
  widgets->results.files_total_count.reset();
  widgets->results.requirement_links.clear();
  gtk_widget_set_visible(GTK_WIDGET(widgets->results.files_count_label), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->results.files_search_entry), FALSE);
  gtk_editable_set_text(GTK_EDITABLE(widgets->results.files_search_entry), "");
  set_package_description_header(widgets, "", "");
  set_package_copy_button_sensitive(widgets, false);
  set_details_text(widgets->results.description_buffer, _("Select a package for details."));
  set_details_text(widgets->results.details_buffer, _("Select a package for details."));
  set_files_text(widgets->results.files_model, _("Select an installed package to view its file list."));
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
  widgets->results.changelog_query_nevra.clear();
  widgets->results.files_loaded_nevra.clear();
  widgets->results.deps_loaded_nevra.clear();
  widgets->results.changelog_loaded_nevra.clear();
  widgets->results.requirement_links.clear();
  widgets->results.pressed_requirement_start = -1;
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(widgets->results.deps_buffer, &start, &end);
  gtk_text_buffer_remove_tag_by_name(widgets->results.deps_buffer, "requirement-link", &start, &end);
  gtk_widget_set_cursor_from_name(GTK_WIDGET(widgets->results.deps_view), "text");
  set_package_description_header(widgets, "", "");
  set_package_copy_button_sensitive(widgets, false);
  set_details_status_text(widgets, nullptr);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.install_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.remove_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.reinstall_button), FALSE);
  pending_transaction_update_action_button_labels_for_selection(widgets, "", "", "", false);
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
update_selected_package_actions(MainWindowUiState *widgets,
                                const PackageTableRow &selected,
                                const InstalledPackageResolution &resolution)
{
  PendingTransactionSelectionActions actions =
      pending_transaction_actions_for_resolved_selection(selected.row,
                                                         selected.upgrade_target(),
                                                         selected.upgrade_generation(),
                                                         resolution,
                                                         widgets->query_state.displayed_query,
                                                         widgets->transaction_state.actions);

  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.install_button), actions.can_install);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.remove_button), actions.can_remove);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.reinstall_button), actions.can_reinstall);

  pending_transaction_update_action_button_labels_for_selection(widgets,
                                                                actions.install_action_nevra,
                                                                actions.installed_action_nevra,
                                                                actions.installed_action_nevra,
                                                                actions.rows.install_is_upgrade,
                                                                actions.rows.install_is_downgrade);
  std::optional<PendingAction::Type> pending_action;
  const char *status_text = selected_package_status_text(widgets, selected, actions.rows, pending_action);
  set_details_status_text(widgets, status_text);
  if (gtk_widget_get_visible(widgets->results.details_status_box)) {
    package_table_apply_resolved_status_label(
        widgets->results.details_status_box, widgets, selected, actions.rows, status_text, pending_action);
  }
}

// -----------------------------------------------------------------------------
// Refresh the selected package action controls from the current table and pending action state.
// -----------------------------------------------------------------------------
void
package_details_refresh_selected_package_actions(MainWindowUiState *widgets)
{
  if (!widgets || widgets->results.selected_nevra.empty()) {
    set_details_status_text(widgets, nullptr);
    return;
  }

  PackageTableRow selected;
  if (!package_table_get_selected_package(widgets, selected) || selected.row.nevra != widgets->results.selected_nevra) {
    set_details_status_text(widgets, nullptr);
    return;
  }

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(selected.row);
  update_selected_package_actions(widgets, selected, resolution);
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
    const PackageRow *upgrade_row_override = nullptr;
    if (td && td->upgrade_row_override.has_value()) {
      upgrade_row_override = &td->upgrade_row_override.value();
    }

    PackageInfoPresentation info =
        dnf_backend_get_package_info_presentation(td ? td->details_query_nevra : "",
                                                  td ? td->details_context : PackageDetailsContext::INSTALLED_CONTEXT,
                                                  upgrade_row_override);
    DNFUI_TRACE("Package info details loaded nevra=%s bytes=%zu",
                td ? td->details_query_nevra.c_str() : "",
                info.full_text.size());

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      return_package_details_task_cancelled(task);
      return;
    }

    DNFUI_TRACE("Package info task done nevra=%s", td ? td->details_query_nevra.c_str() : "");
    g_task_return_pointer(
        task, new PackageInfoPresentation(std::move(info)), +[](gpointer p) {
          delete static_cast<PackageInfoPresentation *>(p);
        });
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
  std::unique_ptr<PackageInfoPresentation> info(
      static_cast<PackageInfoPresentation *>(g_task_propagate_pointer(task, &error)));

  if (!td) {
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
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (!info) {
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : _("Error loading info."), "red");
    set_details_text(widgets->results.description_buffer, error ? error->message : _("Error loading info."));
    set_details_text(widgets->results.details_buffer, error ? error->message : _("Error loading info."));
    if (error) {
      g_error_free(error);
    }
    return;
  }

  // Display general package information.
  set_package_description_header(widgets, info->name, info->summary);
  set_details_text(widgets->results.description_buffer,
                   info->name.empty() ? info->full_text.c_str() : info->description.c_str());
  set_details_text(widgets->results.details_buffer,
                   info->name.empty() ? info->full_text.c_str() : info->common.c_str());
  if (!info->name.empty()) {
    set_package_copy_button_sensitive(widgets, true);
  }

  ui_helpers_set_status(widgets->query.status_label, _("Package info loaded."), "gray");

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
      // NOTE: Load every file path because the list view copies one selected path at a time.
      text = dnf_backend_get_installed_package_files(td->details_query_nevra, td->details_context, 0, &td->file_paths);
      DNFUI_TRACE("Package file list loaded nevra=%s bytes=%zu", td->details_query_nevra.c_str(), text.size());
      break;
    case DeferredDetailsPage::DEPENDENCIES:
      DNFUI_TRACE("Package dependencies load start nevra=%s", td->details_query_nevra.c_str());
      text = dnf_backend_get_package_deps(td->details_query_nevra, td->details_context, &td->requirement_links);
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
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", e.what());
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
  const std::string *current_query_nevra = deferred_details_query_nevra(widgets, td->page);

  GCancellable *c = g_task_get_cancellable(task);
  if (c && ui.cancellable && *ui.cancellable == c) {
    g_object_unref(*ui.cancellable);
    *ui.cancellable = nullptr;
  }

  if (td->generation != BaseManager::instance().current_generation() ||
      widgets->results.selected_nevra != td->selected_nevra || !current_query_nevra ||
      *current_query_nevra != td->details_query_nevra) {
    g_free(text);
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (!text) {
    if (td->page == DeferredDetailsPage::FILES) {
      widgets->results.files_total_count.reset();
      gtk_widget_set_visible(GTK_WIDGET(widgets->results.files_count_label), FALSE);
    }
    if (td->page == DeferredDetailsPage::DEPENDENCIES) {
      widgets->results.requirement_links.clear();
    }
    set_deferred_details_text(ui, ui.error_text);
    if (error && error->message) {
      ui_helpers_set_status(widgets->query.status_label, error->message, "red");
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  *ui.loaded_nevra = td->details_query_nevra;
  if (td->page == DeferredDetailsPage::FILES) {
    if (td->file_paths) {
      widgets->results.files_total_count = td->file_paths->size();
    } else {
      widgets->results.files_total_count.reset();
    }
  }
  if (td->page == DeferredDetailsPage::DEPENDENCIES) {
    widgets->results.requirement_links = td->requirement_links;
  }
  if (td->page == DeferredDetailsPage::FILES && td->file_paths && !td->file_paths->empty()) {
    set_files_paths(ui.files_model, *td->file_paths);
  } else {
    set_deferred_details_text(ui, text);
  }
  if (td->page == DeferredDetailsPage::DEPENDENCIES) {
    apply_requirement_link_tags(ui.buffer, td->requirement_links);
  }
  if (td->page == DeferredDetailsPage::FILES) {
    gtk_widget_set_visible(GTK_WIDGET(widgets->results.files_count_label), td->file_paths.has_value());
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->results.files_search_entry),
                             td->file_paths && !td->file_paths->empty());
  }
  g_free(text);
}

// -----------------------------------------------------------------------------
// Start loading one deferred tab for the selected package if it needs data.
// -----------------------------------------------------------------------------
static void
load_selected_package_details_page(MainWindowUiState *widgets, DeferredDetailsPage page)
{
  if (!widgets || widgets->results.selected_nevra.empty() || widgets->results.package_details_cancellable) {
    return;
  }

  const std::string *query_nevra = deferred_details_query_nevra(widgets, page);
  if (!query_nevra || query_nevra->empty()) {
    return;
  }

  PackageTableRow selected;
  if (!package_table_get_selected_package(widgets, selected) || selected.row.nevra != widgets->results.selected_nevra) {
    return;
  }
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(selected.row);
  PackageDetailsContext details_context = package_details_context_for_selection(widgets, selected, resolution);

  DeferredDetailsUiState ui = deferred_details_ui_state(widgets, page);
  if ((!ui.buffer && !ui.files_model) || !ui.loaded_nevra || !ui.cancellable || *ui.cancellable ||
      *ui.loaded_nevra == *query_nevra) {
    return;
  }

  if (page == DeferredDetailsPage::FILES) {
    widgets->results.files_total_count.reset();
    gtk_widget_set_visible(GTK_WIDGET(widgets->results.files_count_label), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->results.files_search_entry), FALSE);
  }
  if (page == DeferredDetailsPage::DEPENDENCIES) {
    widgets->results.requirement_links.clear();
  }
  set_deferred_details_text(ui, ui.loading_text);
  if (page == DeferredDetailsPage::FILES) {
    gtk_editable_set_text(GTK_EDITABLE(widgets->results.files_search_entry), "");
  }

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_deferred_details_task_finished);
  *ui.cancellable = G_CANCELLABLE(g_object_ref(c));

  DeferredDetailsTaskData *td = new DeferredDetailsTaskData;
  td->selected_nevra = widgets->results.selected_nevra;
  td->details_query_nevra = *query_nevra;
  td->details_context = details_context;
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
  std::string changelog_query_nevra = selected.row.nevra;
  InstalledPackageResolution selected_resolution = dnf_backend_resolve_installed_package(selected.row);
  PackageDetailsContext details_context = package_details_context_for_selection(widgets, selected, selected_resolution);
  std::optional<PackageRow> upgrade_row_override =
      package_details_upgrade_override_for_selection(selected, selected_resolution);
  if (const DaemonUpgradeTarget *target = selected.upgrade_target()) {
    if (!target->nevra.empty()) {
      changelog_query_nevra = target->nevra;
    }
  }
  if (selected.upgrade_target() && selected_resolution.has_installed_row) {
    details_query_nevra = selected_resolution.installed_row.nevra;
  }

  widgets->results.selected_nevra = selected.row.nevra;
  widgets->results.details_query_nevra = details_query_nevra;
  widgets->results.changelog_query_nevra = changelog_query_nevra;
  widgets->results.files_loaded_nevra.clear();
  widgets->results.deps_loaded_nevra.clear();
  widgets->results.changelog_loaded_nevra.clear();
  widgets->results.files_total_count.reset();
  widgets->results.requirement_links.clear();
  gtk_widget_set_visible(GTK_WIDGET(widgets->results.files_count_label), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->results.files_search_entry), FALSE);
  gtk_editable_set_text(GTK_EDITABLE(widgets->results.files_search_entry), "");
  set_package_description_header(widgets, "", "");
  set_package_copy_button_sensitive(widgets, false);
  set_details_text(widgets->results.description_buffer, _("Fetching package info..."));
  set_details_text(widgets->results.details_buffer, _("Fetching package info..."));
  set_files_text(widgets->results.files_model, _("Open the Files tab to load the file list."));
  set_details_text(widgets->results.deps_buffer, _("Open the Dependencies tab to load dependencies."));
  set_details_text(widgets->results.changelog_buffer, _("Open the Changelog tab to load the changelog."));
  ui_helpers_set_status(widgets->query.status_label, _("Fetching package info..."), "blue");
  update_selected_package_actions(widgets, selected, selected_resolution);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_package_details_task_finished);
  widgets->results.package_details_cancellable = G_CANCELLABLE(g_object_ref(c));

  InfoTaskData *td = new InfoTaskData;
  td->selected_nevra = selected.row.nevra;
  td->details_query_nevra = details_query_nevra;
  td->upgrade_row_override = upgrade_row_override;
  td->details_context = details_context;
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
