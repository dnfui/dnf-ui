// -----------------------------------------------------------------------------
// src/ui/repository/repository_view.cpp
// Repository list window
// -----------------------------------------------------------------------------
#include "ui/repository/repository_view.hpp"

#include "dnf_backend/base_manager.hpp"
#include "dnf5daemon_client/repository_service_client.hpp"
#include "i18n.hpp"
#include "ui/details/package_details_controller.hpp"
#include "ui/package_query/package_query_controller.hpp"
#include "ui/package_query/package_query_controller_internal.hpp"
#include "ui/refresh/repository_refresh_controller.hpp"
#include "ui/repository/repository_apply_feedback.hpp"
#include "ui/repository/repository_apply_operation.hpp"
#include "ui/transaction/pending_transaction_apply.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <gio/gio.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kRepositoryStateWidthPx = 90;
constexpr int kRepositoryIdWidthPx = 240;

struct RepositoryWindowState;

GtkWindow *g_repository_window = nullptr;
std::weak_ptr<RepositoryWindowState> g_repository_window_state;

struct RepositoryWindowState {
  GtkWindow *window = nullptr;
  GtkColumnView *column_view = nullptr;
  GtkLabel *status_label = nullptr;
  GtkSpinner *spinner = nullptr;
  GtkButton *refresh_button = nullptr;
  GtkButton *clear_pending_button = nullptr;
  GtkButton *apply_button = nullptr;
  GCancellable *cancellable = nullptr;
  std::weak_ptr<MainWindowUiState> main_widgets;
  std::vector<RepositoryInfo> repositories;
  std::map<std::string, bool> pending_enabled;
  uint64_t load_id = 0;
  bool needs_reload = false;
  bool loading = false;
  bool applying = false;
  bool destroyed = false;
};

struct RepositoryLoadTaskData {
  std::shared_ptr<RepositoryWindowState> state;
  uint64_t load_id = 0;
};

struct RepositoryToggleData {
  std::shared_ptr<RepositoryWindowState> state;
  std::string repository_id;
  bool original_enabled = false;
  gulong signal_handler_id = 0;
};

struct RepositoryApplyTaskData {
  std::shared_ptr<RepositoryWindowState> state;
  uint64_t load_id = 0;
  std::vector<std::string> enable_ids;
  std::vector<std::string> disable_ids;
};

struct RepositoryReviewDialogData {
  std::shared_ptr<RepositoryWindowState> state;
  std::vector<std::string> enable_ids;
  std::vector<std::string> disable_ids;
};

void repository_view_start_load(const std::shared_ptr<RepositoryWindowState> &state);
bool repository_view_effective_enabled(const std::shared_ptr<RepositoryWindowState> &state,
                                       const RepositoryInfo &repository);

enum class RepositoryTextColumn {
  ID,
  NAME,
};

enum class RepositorySortColumn {
  ENABLED,
  ID,
  NAME,
};

struct RepositorySortData {
  RepositorySortColumn column = RepositorySortColumn::ID;
  std::weak_ptr<RepositoryWindowState> state;
};

// -----------------------------------------------------------------------------
// Return the widget that should receive row-color classes for one repository cell.
// -----------------------------------------------------------------------------
GtkWidget *
repository_view_cell_color_target(GtkWidget *cell)
{
  GtkWidget *parent = gtk_widget_get_parent(cell);
  if (!parent || GTK_IS_COLUMN_VIEW(parent)) {
    return cell;
  }

  return parent;
}

// -----------------------------------------------------------------------------
// Remove pending repository color classes from one widget.
// -----------------------------------------------------------------------------
void
repository_view_clear_pending_css(GtkWidget *widget)
{
  if (!widget) {
    return;
  }

  gtk_widget_remove_css_class(widget, "repository-row-pending-enable");
  gtk_widget_remove_css_class(widget, "repository-row-pending-disable");
}

// -----------------------------------------------------------------------------
// Return the pending repository color class for one displayed repository.
// -----------------------------------------------------------------------------
const char *
repository_view_pending_css_class(const std::shared_ptr<RepositoryWindowState> &state, const std::string &repo_id)
{
  if (!state) {
    return nullptr;
  }

  auto pending = state->pending_enabled.find(repo_id);
  if (pending == state->pending_enabled.end()) {
    return nullptr;
  }

  return pending->second ? "repository-row-pending-enable" : "repository-row-pending-disable";
}

// -----------------------------------------------------------------------------
// Store the repository id currently bound to one realized cell.
// -----------------------------------------------------------------------------
void
repository_view_set_cell_repository_id(GtkWidget *cell, const std::string &repo_id)
{
  if (!cell) {
    return;
  }

  auto *old_id = static_cast<std::string *>(g_object_steal_data(G_OBJECT(cell), "repository-row-id"));
  delete old_id;

  auto *new_id = new std::string(repo_id);
  g_object_set_data_full(
      G_OBJECT(cell), "repository-row-id", new_id, [](gpointer p) { delete static_cast<std::string *>(p); });
}

// -----------------------------------------------------------------------------
// Clear the repository id stored on one realized cell.
// -----------------------------------------------------------------------------
void
repository_view_clear_cell_repository_id(GtkWidget *cell)
{
  if (!cell) {
    return;
  }

  auto *old_id = static_cast<std::string *>(g_object_steal_data(G_OBJECT(cell), "repository-row-id"));
  delete old_id;
}

// -----------------------------------------------------------------------------
// Apply pending repository color to one realized cell.
// -----------------------------------------------------------------------------
void
repository_view_update_pending_css_for_cell(GtkWidget *cell, const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!cell) {
    return;
  }

  GtkWidget *target = repository_view_cell_color_target(cell);
  repository_view_clear_pending_css(cell);
  repository_view_clear_pending_css(target);

  auto *repo_id = static_cast<std::string *>(g_object_get_data(G_OBJECT(cell), "repository-row-id"));
  if (!repo_id) {
    return;
  }

  const char *css_class = repository_view_pending_css_class(state, *repo_id);
  if (css_class) {
    gtk_widget_add_css_class(target, css_class);
  }
}

// -----------------------------------------------------------------------------
// Refresh pending repository colors in currently realized rows.
// -----------------------------------------------------------------------------
void
repository_view_refresh_visible_pending_css(GtkWidget *widget, const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!widget) {
    return;
  }

  repository_view_update_pending_css_for_cell(widget, state);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    repository_view_refresh_visible_pending_css(child, state);
  }
}

// -----------------------------------------------------------------------------
// Refresh pending repository colors for the repository table.
// -----------------------------------------------------------------------------
void
repository_view_refresh_visible_pending_css(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state || !state->column_view) {
    return;
  }

  repository_view_refresh_visible_pending_css(GTK_WIDGET(state->column_view), state);
}

// -----------------------------------------------------------------------------
// Return the main application state if it is still alive.
// -----------------------------------------------------------------------------
std::shared_ptr<MainWindowUiState>
repository_view_main_widgets(const std::shared_ptr<RepositoryWindowState> &state)
{
  return state ? state->main_widgets.lock() : nullptr;
}

// -----------------------------------------------------------------------------
// Return the main window widget from the shared main UI state.
// -----------------------------------------------------------------------------
GtkWidget *
repository_view_main_window_widget(const std::shared_ptr<MainWindowUiState> &widgets)
{
  if (!widgets || !widgets->query.entry) {
    return nullptr;
  }

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
  return root && GTK_IS_WIDGET(root) ? GTK_WIDGET(root) : nullptr;
}

// -----------------------------------------------------------------------------
// Enable or disable the main window while repository changes are applying.
// -----------------------------------------------------------------------------
void
repository_view_set_main_window_sensitive(const std::shared_ptr<RepositoryWindowState> &state, bool sensitive)
{
  std::shared_ptr<MainWindowUiState> widgets = repository_view_main_widgets(state);
  GtkWidget *window = repository_view_main_window_widget(widgets);
  if (window) {
    gtk_widget_set_sensitive(window, sensitive);
  }
}

// -----------------------------------------------------------------------------
// Show why repository changes cannot be applied right now.
// -----------------------------------------------------------------------------
bool
repository_view_check_apply_preconditions(const std::shared_ptr<RepositoryWindowState> &state)
{
  std::shared_ptr<MainWindowUiState> widgets = repository_view_main_widgets(state);
  if (!state || !widgets || widgets->window_state.destroyed) {
    if (state && state->status_label) {
      gtk_label_set_text(state->status_label, _("The main window is not available."));
    }
    return false;
  }

  const char *message = nullptr;
  if (package_query_has_active_package_list_request(widgets.get())) {
    message = _("Wait for the current package query to finish.");
  } else if (repository_refresh_is_running()) {
    message = _("Wait for repository refresh to finish.");
  } else if (pending_transaction_preview_is_busy(widgets.get())) {
    message = pending_transaction_preview_busy_message();
  } else if (pending_transaction_apply_is_busy(widgets.get())) {
    message = pending_transaction_apply_busy_message();
  } else if (!widgets->transaction_state.preview_transaction_path.empty()) {
    message = _("Close the transaction preview before changing repositories.");
  }

  if (!message) {
    return true;
  }

  if (state->status_label) {
    gtk_label_set_text(state->status_label, message);
  }
  ui_helpers_set_status(widgets->query.status_label, message, "blue");
  return false;
}

// -----------------------------------------------------------------------------
// Return true once a daemon write call reached dnf5daemon.
// -----------------------------------------------------------------------------
bool
repository_view_write_was_attempted(RepositoryWriteOutcome outcome)
{
  return outcome != RepositoryWriteOutcome::NOT_ATTEMPTED;
}

// -----------------------------------------------------------------------------
// Update the main package view after repository configuration may have changed.
// -----------------------------------------------------------------------------
void
repository_view_refresh_main_after_apply(const std::shared_ptr<MainWindowUiState> &widgets,
                                         const RepositoryApplyResult &result)
{
  if (!widgets || widgets->window_state.destroyed) {
    return;
  }

  package_query_clear_search_cache();
  DaemonUpgradeState::instance().mark_stale();
  package_query_refresh_upgrade_indicator(widgets.get());

  if (result.backend_sync_result == RepositoryBackendSyncResult::FAILED) {
    BaseManager::instance().drop_cached_base();
    package_query_on_clear_button_clicked(nullptr, widgets.get());
    ui_helpers_set_status(widgets->query.status_label, repository_view_main_status_for_result(result), "red");
    return;
  }

  bool cleared_upgradeable_table = package_query_clear_displayed_upgradeable_table(widgets.get());
  if (!cleared_upgradeable_table) {
    package_query_reload_current_view(widgets.get());
  }
  package_query_start_upgrade_indicator_refresh(widgets.get());
  ui_helpers_set_status(widgets->query.status_label,
                        repository_view_main_status_for_result(result),
                        repository_view_main_status_color_for_result(result));
}

// -----------------------------------------------------------------------------
// Update action buttons from the current repository window state.
// -----------------------------------------------------------------------------
void
repository_view_update_action_buttons(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state) {
    return;
  }

  bool has_pending_changes = !state->pending_enabled.empty();
  if (state->refresh_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->refresh_button),
                             !state->loading && !state->applying && !has_pending_changes);
  }
  if (state->clear_pending_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->clear_pending_button),
                             has_pending_changes && !state->loading && !state->applying);
  }
  if (state->apply_button) {
    bool sensitive = has_pending_changes && !state->loading && !state->applying && !state->needs_reload;
    gtk_widget_set_sensitive(GTK_WIDGET(state->apply_button), sensitive);
  }
}

// -----------------------------------------------------------------------------
// Show the current pending repository change count.
// -----------------------------------------------------------------------------
void
repository_view_show_pending_status(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state || !state->status_label) {
    return;
  }

  if (state->pending_enabled.empty()) {
    gtk_label_set_text(state->status_label, _("No repository changes pending."));
    return;
  }

  std::string message = dnfui_i18n_format_count(
      state->pending_enabled.size(), "%zu repository change is pending.", "%zu repository changes are pending.");
  gtk_label_set_text(state->status_label, message.c_str());
}

// -----------------------------------------------------------------------------
// Return the private key used to store repository rows on GTK objects.
// -----------------------------------------------------------------------------
GQuark
repository_info_quark()
{
  static GQuark q = 0;
  if (G_UNLIKELY(q == 0)) {
    q = g_quark_from_static_string("repository-info");
  }

  return q;
}

// -----------------------------------------------------------------------------
// Wrap one repository row in a plain GTK object for the column view model.
// -----------------------------------------------------------------------------
GObject *
repository_view_make_object(const RepositoryInfo &repository)
{
  GObject *object = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
  g_object_set_qdata_full(object, repository_info_quark(), new RepositoryInfo(repository), [](gpointer p) {
    delete static_cast<RepositoryInfo *>(p);
  });
  return object;
}

// -----------------------------------------------------------------------------
// Return the repository row stored on one GTK model object.
// -----------------------------------------------------------------------------
const RepositoryInfo *
repository_view_info_from_object(GObject *object)
{
  return object ? static_cast<const RepositoryInfo *>(g_object_get_qdata(object, repository_info_quark())) : nullptr;
}

// -----------------------------------------------------------------------------
// Compare repository text values for table sorting.
// -----------------------------------------------------------------------------
int
repository_view_compare_text(const std::string &left, const std::string &right)
{
  return g_ascii_strcasecmp(left.c_str(), right.c_str());
}

// -----------------------------------------------------------------------------
// Compare two repository rows by the requested column.
// -----------------------------------------------------------------------------
int
repository_view_compare_repositories(const RepositoryInfo &left,
                                     const RepositoryInfo &right,
                                     RepositorySortColumn column,
                                     const std::shared_ptr<RepositoryWindowState> &state)
{
  int result = 0;
  switch (column) {
  case RepositorySortColumn::ENABLED: {
    const bool left_enabled = repository_view_effective_enabled(state, left);
    const bool right_enabled = repository_view_effective_enabled(state, right);
    result = static_cast<int>(left_enabled) - static_cast<int>(right_enabled);
    break;
  }
  case RepositorySortColumn::ID:
    result = repository_view_compare_text(left.id, right.id);
    break;
  case RepositorySortColumn::NAME:
    result = repository_view_compare_text(left.name, right.name);
    break;
  }

  if (result != 0) {
    return result;
  }

  return repository_view_compare_text(left.id, right.id);
}

// -----------------------------------------------------------------------------
// Adapter from GTK's custom sorter callback to the repository row comparator.
// -----------------------------------------------------------------------------
int
repository_view_sorter_compare(gconstpointer item1, gconstpointer item2, gpointer user_data)
{
  auto *sort_data = static_cast<RepositorySortData *>(user_data);
  if (!sort_data) {
    return 0;
  }

  const RepositoryInfo *left = repository_view_info_from_object(G_OBJECT(const_cast<gpointer>(item1)));
  const RepositoryInfo *right = repository_view_info_from_object(G_OBJECT(const_cast<gpointer>(item2)));
  if (!left || !right) {
    return 0;
  }

  return repository_view_compare_repositories(*left, *right, sort_data->column, sort_data->state.lock());
}

// -----------------------------------------------------------------------------
// Create one repository column sorter.
// -----------------------------------------------------------------------------
GtkSorter *
repository_view_create_sorter(RepositorySortColumn column, const std::shared_ptr<RepositoryWindowState> &state = {})
{
  auto *sort_data = new RepositorySortData {
    column,
    state,
  };
  return GTK_SORTER(gtk_custom_sorter_new(
      repository_view_sorter_compare, sort_data, [](gpointer p) { delete static_cast<RepositorySortData *>(p); }));
}

// -----------------------------------------------------------------------------
// Return the currently displayed enabled state for one repository row.
// -----------------------------------------------------------------------------
bool
repository_view_effective_enabled(const std::shared_ptr<RepositoryWindowState> &state, const RepositoryInfo &repository)
{
  if (!state) {
    return repository.enabled;
  }

  auto pending = state->pending_enabled.find(repository.id);
  return pending == state->pending_enabled.end() ? repository.enabled : pending->second;
}

// -----------------------------------------------------------------------------
// Replace the repository model while keeping the column view and column widths.
// -----------------------------------------------------------------------------
void
repository_view_set_model(GtkColumnView *column_view, const std::vector<RepositoryInfo> &repositories)
{
  if (!column_view) {
    return;
  }

  GListStore *store = g_list_store_new(G_TYPE_OBJECT);
  for (const RepositoryInfo &repository : repositories) {
    GObject *object = repository_view_make_object(repository);
    g_list_store_append(store, object);
    g_object_unref(object);
  }

  GtkSortListModel *sort_model = gtk_sort_list_model_new(nullptr, nullptr);
  gtk_sort_list_model_set_model(sort_model, G_LIST_MODEL(store));
  gtk_sort_list_model_set_sorter(sort_model, gtk_column_view_get_sorter(column_view));
  g_object_unref(store);

  GtkNoSelection *selection = gtk_no_selection_new(G_LIST_MODEL(sort_model));

  gtk_column_view_set_model(column_view, GTK_SELECTION_MODEL(selection));
  g_object_unref(selection);
}

// -----------------------------------------------------------------------------
// Update loading controls for the repository window.
// -----------------------------------------------------------------------------
void
repository_view_set_loading(const std::shared_ptr<RepositoryWindowState> &state, bool loading)
{
  if (!state || state->destroyed) {
    return;
  }

  state->loading = loading;
  if (state->column_view) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->column_view),
                             !state->loading && !state->applying && !state->needs_reload);
  }
  repository_view_update_action_buttons(state);

  if (!state->spinner) {
    return;
  }

  if (state->loading || state->applying) {
    gtk_spinner_start(state->spinner);
    gtk_widget_set_visible(GTK_WIDGET(state->spinner), TRUE);
  } else {
    gtk_spinner_stop(state->spinner);
    gtk_widget_set_visible(GTK_WIDGET(state->spinner), FALSE);
  }
}

// -----------------------------------------------------------------------------
// Create one resizable text column for the repository table.
// -----------------------------------------------------------------------------
GtkColumnViewColumn *
repository_view_create_text_column(const char *title,
                                   RepositoryTextColumn column_kind,
                                   int fixed_width,
                                   bool expand,
                                   const std::shared_ptr<RepositoryWindowState> &state)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_object_set_data(G_OBJECT(factory), "repository-text-column", GINT_TO_POINTER(static_cast<int>(column_kind)));

  g_signal_connect(factory,
                   "setup",
                   G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
                     GtkWidget *label = gtk_label_new(nullptr);
                     gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
                     gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
                     gtk_label_set_selectable(GTK_LABEL(label), TRUE);
                     gtk_widget_set_hexpand(label, TRUE);
                     gtk_widget_set_margin_start(label, 6);
                     gtk_widget_set_margin_end(label, 6);
                     gtk_widget_set_margin_top(label, 4);
                     gtk_widget_set_margin_bottom(label, 4);
                     gtk_list_item_set_child(item, label);
                   }),
                   nullptr);

  auto *state_holder = new std::shared_ptr<RepositoryWindowState>(state);
  g_signal_connect_data(
      factory,
      "bind",
      G_CALLBACK(+[](GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
        auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
        RepositoryTextColumn column_kind = static_cast<RepositoryTextColumn>(
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(factory), "repository-text-column")));
        GtkWidget *label = gtk_list_item_get_child(item);
        GObject *object = G_OBJECT(gtk_list_item_get_item(item));
        const RepositoryInfo *repository = repository_view_info_from_object(object);
        if (!repository) {
          gtk_label_set_text(GTK_LABEL(label), "");
          repository_view_clear_cell_repository_id(label);
          repository_view_update_pending_css_for_cell(label, state_holder ? *state_holder : nullptr);
          return;
        }

        const std::string &text = column_kind == RepositoryTextColumn::ID ? repository->id : repository->name;
        gtk_label_set_text(GTK_LABEL(label), text.c_str());
        repository_view_set_cell_repository_id(label, repository->id);
        repository_view_update_pending_css_for_cell(label, state_holder ? *state_holder : nullptr);
      }),
      state_holder,
      [](gpointer p, GClosure *) { delete static_cast<std::shared_ptr<RepositoryWindowState> *>(p); },
      G_CONNECT_DEFAULT);

  GtkColumnViewColumn *column = gtk_column_view_column_new(title, nullptr);
  gtk_column_view_column_set_factory(column, factory);
  g_object_unref(factory);
  gtk_column_view_column_set_resizable(column, TRUE);
  gtk_column_view_column_set_expand(column, expand);
  RepositorySortColumn sort_column =
      column_kind == RepositoryTextColumn::ID ? RepositorySortColumn::ID : RepositorySortColumn::NAME;
  GtkSorter *sorter = repository_view_create_sorter(sort_column);
  gtk_column_view_column_set_sorter(column, sorter);
  g_object_unref(sorter);
  if (fixed_width > 0) {
    gtk_column_view_column_set_fixed_width(column, fixed_width);
  }
  return column;
}

// -----------------------------------------------------------------------------
// Create the enabled column with one reusable checkbox per realized row.
// -----------------------------------------------------------------------------
GtkColumnViewColumn *
repository_view_create_enabled_column(const std::shared_ptr<RepositoryWindowState> &state)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  auto *state_holder = new std::shared_ptr<RepositoryWindowState>(state);

  g_signal_connect_data(
      factory,
      "setup",
      G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer user_data) {
        auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
        GtkWidget *check_button = gtk_check_button_new();
        gtk_widget_set_halign(check_button, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(check_button, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(check_button, 4);
        gtk_widget_set_margin_bottom(check_button, 4);

        auto *toggle_data = new RepositoryToggleData;
        toggle_data->state = state_holder ? *state_holder : nullptr;
        g_object_set_data_full(G_OBJECT(check_button), "repository-toggle-data", toggle_data, [](gpointer p) {
          delete static_cast<RepositoryToggleData *>(p);
        });

        toggle_data->signal_handler_id = g_signal_connect(
            check_button,
            "toggled",
            G_CALLBACK(+[](GtkCheckButton *check_button, gpointer user_data) {
              auto *toggle_data = static_cast<RepositoryToggleData *>(user_data);
              if (!toggle_data || !toggle_data->state || toggle_data->state->destroyed || toggle_data->state->loading ||
                  toggle_data->state->applying || toggle_data->repository_id.empty()) {
                return;
              }

              bool enabled = gtk_check_button_get_active(check_button);
              if (enabled == toggle_data->original_enabled) {
                toggle_data->state->pending_enabled.erase(toggle_data->repository_id);
              } else {
                toggle_data->state->pending_enabled[toggle_data->repository_id] = enabled;
              }

              repository_view_update_action_buttons(toggle_data->state);
              repository_view_show_pending_status(toggle_data->state);
              repository_view_refresh_visible_pending_css(toggle_data->state);
            }),
            toggle_data);

        gtk_list_item_set_child(item, check_button);
      }),
      state_holder,
      [](gpointer p, GClosure *) { delete static_cast<std::shared_ptr<RepositoryWindowState> *>(p); },
      G_CONNECT_DEFAULT);

  g_signal_connect(
      factory,
      "bind",
      G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
        GtkWidget *check_button = gtk_list_item_get_child(item);
        auto *toggle_data =
            static_cast<RepositoryToggleData *>(g_object_get_data(G_OBJECT(check_button), "repository-toggle-data"));
        GObject *object = G_OBJECT(gtk_list_item_get_item(item));
        const RepositoryInfo *repository = repository_view_info_from_object(object);

        if (!toggle_data || !repository) {
          if (toggle_data) {
            toggle_data->repository_id.clear();
            toggle_data->original_enabled = false;
            if (toggle_data->signal_handler_id != 0) {
              g_signal_handler_block(check_button, toggle_data->signal_handler_id);
            }
            gtk_check_button_set_active(GTK_CHECK_BUTTON(check_button), FALSE);
            if (toggle_data->signal_handler_id != 0) {
              g_signal_handler_unblock(check_button, toggle_data->signal_handler_id);
            }
          }
          gtk_widget_set_sensitive(check_button, FALSE);
          repository_view_clear_cell_repository_id(check_button);
          repository_view_update_pending_css_for_cell(check_button, toggle_data ? toggle_data->state : nullptr);
          return;
        }

        toggle_data->repository_id = repository->id;
        toggle_data->original_enabled = repository->enabled;
        bool enabled = repository_view_effective_enabled(toggle_data->state, *repository);
        if (toggle_data->signal_handler_id != 0) {
          g_signal_handler_block(check_button, toggle_data->signal_handler_id);
        }
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check_button), enabled);
        if (toggle_data->signal_handler_id != 0) {
          g_signal_handler_unblock(check_button, toggle_data->signal_handler_id);
        }
        gtk_widget_set_sensitive(check_button, TRUE);
        repository_view_set_cell_repository_id(check_button, repository->id);
        repository_view_update_pending_css_for_cell(check_button, toggle_data->state);
      }),
      nullptr);

  GtkColumnViewColumn *column = gtk_column_view_column_new(_("Enabled"), nullptr);
  gtk_column_view_column_set_factory(column, factory);
  g_object_unref(factory);
  gtk_column_view_column_set_resizable(column, TRUE);
  gtk_column_view_column_set_fixed_width(column, kRepositoryStateWidthPx);
  GtkSorter *sorter = repository_view_create_sorter(RepositorySortColumn::ENABLED, state);
  gtk_column_view_column_set_sorter(column, sorter);
  g_object_unref(sorter);
  return column;
}

// -----------------------------------------------------------------------------
// Append one repository table column and release the caller reference.
// -----------------------------------------------------------------------------
void
repository_view_append_column(GtkColumnView *view, GtkColumnViewColumn *column)
{
  gtk_column_view_append_column(view, column);
  g_object_unref(column);
}

// -----------------------------------------------------------------------------
// Create the repository column view.
// -----------------------------------------------------------------------------
GtkWidget *
repository_view_create_column_view(const std::shared_ptr<RepositoryWindowState> &state)
{
  GtkColumnView *view = GTK_COLUMN_VIEW(gtk_column_view_new(nullptr));
  gtk_widget_set_hexpand(GTK_WIDGET(view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(view), TRUE);
  gtk_column_view_set_show_row_separators(view, TRUE);
  gtk_column_view_set_show_column_separators(view, TRUE);

  repository_view_append_column(view, repository_view_create_enabled_column(state));
  repository_view_append_column(view,
                                repository_view_create_text_column(
                                    _("Repository ID"), RepositoryTextColumn::ID, kRepositoryIdWidthPx, false, state));
  repository_view_append_column(
      view, repository_view_create_text_column(_("Name"), RepositoryTextColumn::NAME, 0, true, state));
  repository_view_set_model(view, {});
  return GTK_WIDGET(view);
}

// -----------------------------------------------------------------------------
// Replace the visible repository rows with one daemon list result.
// -----------------------------------------------------------------------------
void
repository_view_render_repositories(const std::shared_ptr<RepositoryWindowState> &state,
                                    const std::vector<RepositoryInfo> &repositories)
{
  if (!state || state->destroyed || !state->column_view) {
    return;
  }

  state->repositories = repositories;
  repository_view_set_model(state->column_view, repositories);
  repository_view_update_action_buttons(state);
}

// -----------------------------------------------------------------------------
// Clear staged repository changes without applying them.
// -----------------------------------------------------------------------------
void
repository_view_clear_pending_changes(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state || state->destroyed || state->loading || state->applying) {
    return;
  }

  if (state->pending_enabled.empty()) {
    repository_view_show_pending_status(state);
    return;
  }

  state->pending_enabled.clear();
  repository_view_set_model(state->column_view, state->repositories);
  repository_view_update_action_buttons(state);
  repository_view_show_pending_status(state);
}

// -----------------------------------------------------------------------------
// Load repositories on a worker thread.
// -----------------------------------------------------------------------------
void
on_repository_load_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  auto *repositories = new std::vector<RepositoryInfo>();
  std::string error;
  if (!repository_service_client_list(*repositories, error, cancellable)) {
    delete repositories;
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("Repository load was cancelled."));
    } else {
      g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, error.c_str()));
    }
    return;
  }

  g_task_return_pointer(task, repositories, [](gpointer p) { delete static_cast<std::vector<RepositoryInfo> *>(p); });
}

// -----------------------------------------------------------------------------
// Finish loading repositories on the GTK thread.
// -----------------------------------------------------------------------------
void
on_repository_load_finished(GObject *, GAsyncResult *result, gpointer)
{
  GTask *task = G_TASK(result);
  auto *task_data = static_cast<RepositoryLoadTaskData *>(g_task_get_task_data(task));
  std::shared_ptr<RepositoryWindowState> state = task_data ? task_data->state : nullptr;
  uint64_t load_id = task_data ? task_data->load_id : 0;

  if (!state || state->destroyed || load_id != state->load_id) {
    return;
  }

  repository_view_set_loading(state, false);

  if (state->cancellable) {
    g_object_unref(state->cancellable);
    state->cancellable = nullptr;
  }

  GError *error = nullptr;
  auto *repositories = static_cast<std::vector<RepositoryInfo> *>(g_task_propagate_pointer(task, &error));
  if (!repositories) {
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      std::string message = _("Failed to load repositories.");
      message += " ";
      message += error->message;
      gtk_label_set_text(state->status_label, message.c_str());
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  state->pending_enabled.clear();
  state->needs_reload = false;
  repository_view_render_repositories(state, *repositories);

  std::string message =
      dnfui_i18n_format_count(repositories->size(), "Showing %zu repository.", "Showing %zu repositories.");
  gtk_label_set_text(state->status_label, message.c_str());

  delete repositories;
}

// -----------------------------------------------------------------------------
// Start or restart the repository load.
// -----------------------------------------------------------------------------
void
repository_view_start_load(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state || state->destroyed) {
    return;
  }

  if (!state->pending_enabled.empty()) {
    repository_view_show_pending_status(state);
    return;
  }

  if (state->cancellable) {
    g_cancellable_cancel(state->cancellable);
    g_object_unref(state->cancellable);
  }

  state->cancellable = g_cancellable_new();
  ++state->load_id;
  state->pending_enabled.clear();
  state->repositories.clear();

  repository_view_set_model(state->column_view, {});
  repository_view_set_loading(state, true);
  gtk_label_set_text(state->status_label, _("Loading repositories..."));

  auto *task_data = new RepositoryLoadTaskData {
    state,
    state->load_id,
  };
  GTask *task = g_task_new(nullptr, state->cancellable, on_repository_load_finished, nullptr);
  g_task_set_task_data(task, task_data, [](gpointer p) { delete static_cast<RepositoryLoadTaskData *>(p); });
  g_task_run_in_thread(task, on_repository_load_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Apply pending repository changes on a worker thread.
// -----------------------------------------------------------------------------
void
on_repository_apply_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  const auto *task_data = static_cast<const RepositoryApplyTaskData *>(g_task_get_task_data(task));
  const std::vector<std::string> desired_enable_ids = task_data ? task_data->enable_ids : std::vector<std::string> {};
  const std::vector<std::string> desired_disable_ids = task_data ? task_data->disable_ids : std::vector<std::string> {};

  auto *apply_result = new RepositoryApplyResult(repository_apply_run(desired_enable_ids, desired_disable_ids));
  g_task_return_pointer(task, apply_result, [](gpointer p) { delete static_cast<RepositoryApplyResult *>(p); });
}

// -----------------------------------------------------------------------------
// Finish applying pending repository changes on the GTK thread.
// -----------------------------------------------------------------------------
void
on_repository_apply_finished(GObject *, GAsyncResult *result, gpointer)
{
  GTask *task = G_TASK(result);
  auto *task_data = static_cast<RepositoryApplyTaskData *>(g_task_get_task_data(task));
  std::shared_ptr<RepositoryWindowState> state = task_data ? task_data->state : nullptr;
  uint64_t load_id = task_data ? task_data->load_id : 0;

  if (!state || load_id != state->load_id) {
    return;
  }

  std::shared_ptr<MainWindowUiState> main_widgets = repository_view_main_widgets(state);
  repository_view_set_main_window_sensitive(state, true);

  state->applying = false;

  GError *error = nullptr;
  auto *apply_result = static_cast<RepositoryApplyResult *>(g_task_propagate_pointer(task, &error));
  if (!apply_result) {
    if (state->destroyed) {
      if (error) {
        g_error_free(error);
      }
      return;
    }

    repository_view_set_loading(state, false);
    std::string message = _("Failed to apply repository changes.");
    if (error && error->message) {
      message += " ";
      message += error->message;
    }
    gtk_label_set_text(state->status_label, message.c_str());
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (apply_result->sync_needed) {
    repository_view_refresh_main_after_apply(main_widgets, *apply_result);
  }

  if (apply_result->clear_pending && apply_result->repository_state_loaded) {
    state->pending_enabled.clear();
    state->needs_reload = false;
    repository_view_render_repositories(state, apply_result->repositories);
  } else if (apply_result->clear_pending && repository_view_write_was_attempted(apply_result->write_outcome)) {
    state->pending_enabled.clear();
    repository_view_refresh_visible_pending_css(state);
    state->needs_reload = true;
  }

  if (state->destroyed) {
    delete apply_result;
    return;
  }

  repository_view_set_loading(state, false);

  std::string message = repository_view_window_status_for_result(*apply_result);
  if (!apply_result->error.empty()) {
    message += " ";
    message += apply_result->error;
  }
  gtk_label_set_text(state->status_label, message.c_str());
  delete apply_result;
}

// -----------------------------------------------------------------------------
// Start applying repository changes after the user has reviewed them.
// -----------------------------------------------------------------------------
void
repository_view_start_apply_changes(const std::shared_ptr<RepositoryWindowState> &state,
                                    std::vector<std::string> enable_ids,
                                    std::vector<std::string> disable_ids)
{
  if (!state || state->destroyed || state->loading || state->applying || (enable_ids.empty() && disable_ids.empty())) {
    return;
  }

  if (!repository_view_check_apply_preconditions(state)) {
    return;
  }

  std::shared_ptr<MainWindowUiState> main_widgets = repository_view_main_widgets(state);
  state->applying = true;
  ++state->load_id;
  package_details_cancel_active_load(main_widgets.get());
  repository_view_set_main_window_sensitive(state, false);
  repository_view_set_loading(state, false);
  gtk_label_set_text(state->status_label, _("Applying repository changes..."));

  auto *task_data = new RepositoryApplyTaskData {
    state,
    state->load_id,
    std::move(enable_ids),
    std::move(disable_ids),
  };
  GTask *task = g_task_new(nullptr, nullptr, on_repository_apply_finished, nullptr);
  g_task_set_task_data(task, task_data, [](gpointer p) { delete static_cast<RepositoryApplyTaskData *>(p); });
  g_task_run_in_thread(task, on_repository_apply_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Add one repository review section to the confirmation dialog.
// -----------------------------------------------------------------------------
void
repository_view_add_review_section(GtkBox *box, const char *title, const std::vector<std::string> &repo_ids)
{
  if (!box || repo_ids.empty()) {
    return;
  }

  GtkWidget *heading = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(box, heading);

  for (const auto &repo_id : repo_ids) {
    GtkWidget *label = gtk_label_new(repo_id.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_focusable(label, FALSE);
    gtk_box_append(box, label);
  }
}

// -----------------------------------------------------------------------------
// Show a review dialog before applying repository changes.
// -----------------------------------------------------------------------------
void
repository_view_confirm_apply_changes(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state || state->destroyed || state->loading || state->applying || state->pending_enabled.empty()) {
    return;
  }

  std::vector<std::string> enable_ids;
  std::vector<std::string> disable_ids;
  for (const auto &[repo_id, enabled] : state->pending_enabled) {
    if (enabled) {
      enable_ids.push_back(repo_id);
    } else {
      disable_ids.push_back(repo_id);
    }
  }

  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dialog, _("Apply Repository Changes"));
  gtk_window_set_modal(dialog, TRUE);
  gtk_window_set_transient_for(dialog, state->window);
  gtk_window_set_destroy_with_parent(dialog, TRUE);
  gtk_window_set_default_size(dialog, 460, 360);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *question = gtk_label_new(_("Apply the following repository changes?"));
  gtk_label_set_xalign(GTK_LABEL(question), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(question), TRUE);
  gtk_box_append(GTK_BOX(outer), question);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_append(GTK_BOX(outer), scroller);

  GtkWidget *contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(contents, 6);
  gtk_widget_set_margin_end(contents, 6);
  gtk_widget_set_margin_top(contents, 6);
  gtk_widget_set_margin_bottom(contents, 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), contents);

  repository_view_add_review_section(GTK_BOX(contents), _("To be enabled"), enable_ids);
  repository_view_add_review_section(GTK_BOX(contents), _("To be disabled"), disable_ids);

  GtkWidget *summary = gtk_label_new(nullptr);
  std::string summary_text = dnfui_i18n_format_count(enable_ids.size() + disable_ids.size(),
                                                     "%zu repository change will be applied.",
                                                     "%zu repository changes will be applied.");
  gtk_label_set_text(GTK_LABEL(summary), summary_text.c_str());
  gtk_label_set_xalign(GTK_LABEL(summary), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(summary), TRUE);
  gtk_box_append(GTK_BOX(outer), summary);

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  GtkWidget *cancel_button = gtk_button_new_with_label(_("Cancel"));
  gtk_box_append(GTK_BOX(button_box), cancel_button);

  GtkWidget *apply_button = gtk_button_new_with_label(_("Apply"));
  gtk_widget_add_css_class(apply_button, "suggested-action");
  gtk_box_append(GTK_BOX(button_box), apply_button);

  auto *dialog_data = new RepositoryReviewDialogData {
    state,
    std::move(enable_ids),
    std::move(disable_ids),
  };
  g_object_set_data_full(G_OBJECT(dialog), "dnfui-repository-review-data", dialog_data, [](gpointer p) {
    delete static_cast<RepositoryReviewDialogData *>(p);
  });

  g_signal_connect(cancel_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                   }),
                   nullptr);

  g_signal_connect(apply_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                     auto *dialog_data = static_cast<RepositoryReviewDialogData *>(user_data);
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     std::shared_ptr<RepositoryWindowState> state = dialog_data ? dialog_data->state : nullptr;
                     std::vector<std::string> enable_ids =
                         dialog_data ? dialog_data->enable_ids : std::vector<std::string> {};
                     std::vector<std::string> disable_ids =
                         dialog_data ? dialog_data->disable_ids : std::vector<std::string> {};

                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                     repository_view_start_apply_changes(state, std::move(enable_ids), std::move(disable_ids));
                   }),
                   dialog_data);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// Keep the repository window open while a non-cancellable apply is running.
// -----------------------------------------------------------------------------
gboolean
on_repository_window_close_request(GtkWindow *, gpointer user_data)
{
  auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
  std::shared_ptr<RepositoryWindowState> state = state_holder ? *state_holder : nullptr;
  if (!state || !state->applying) {
    return FALSE;
  }

  if (state->status_label) {
    gtk_label_set_text(state->status_label, _("Repository changes are still being applied."));
  }
  return TRUE;
}

// -----------------------------------------------------------------------------
// Mark the repository window as destroyed and cancel any active load.
// -----------------------------------------------------------------------------
void
on_repository_window_destroy(GtkWidget *, gpointer user_data)
{
  auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
  if (!state_holder || !*state_holder) {
    return;
  }

  auto state = *state_holder;
  state->destroyed = true;
  if (g_repository_window == state->window) {
    g_repository_window = nullptr;
  }
  if (state->cancellable) {
    g_cancellable_cancel(state->cancellable);
    g_object_unref(state->cancellable);
    state->cancellable = nullptr;
  }
}

} // namespace

// -----------------------------------------------------------------------------
// Close the repository list window if it is open.
// -----------------------------------------------------------------------------
void
repository_view_close_window()
{
  if (g_repository_window) {
    gtk_window_close(g_repository_window);
  }
}

// -----------------------------------------------------------------------------
// Return true while repository changes are being applied.
// -----------------------------------------------------------------------------
bool
repository_view_is_applying_changes()
{
  std::shared_ptr<RepositoryWindowState> state = g_repository_window_state.lock();
  return state && state->applying;
}

// -----------------------------------------------------------------------------
// Open the repository list window.
// -----------------------------------------------------------------------------
void
repository_view_show_window(GtkWindow *parent, const std::shared_ptr<MainWindowUiState> &main_widgets)
{
  if (g_repository_window) {
    gtk_window_present(g_repository_window);
    return;
  }

  auto state = std::make_shared<RepositoryWindowState>();

  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  state->window = window;
  state->main_widgets = main_widgets;
  g_repository_window = window;
  g_repository_window_state = state;
  gtk_window_set_title(window, _("Repositories"));
  gtk_window_set_default_size(window, 720, 520);
  if (parent) {
    gtk_window_set_application(window, gtk_window_get_application(parent));
  }

  auto *state_holder = new std::shared_ptr<RepositoryWindowState>(state);
  g_object_set_data_full(G_OBJECT(window), "dnfui-repository-window-state", state_holder, [](gpointer p) {
    delete static_cast<std::shared_ptr<RepositoryWindowState> *>(p);
  });
  g_signal_connect(window, "close-request", G_CALLBACK(on_repository_window_close_request), state_holder);
  g_signal_connect(window, "destroy", G_CALLBACK(on_repository_window_destroy), state_holder);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(root, 12);
  gtk_widget_set_margin_end(root, 12);
  gtk_widget_set_margin_top(root, 12);
  gtk_widget_set_margin_bottom(root, 12);
  gtk_window_set_child(window, root);

  GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(root), top_row);

  GtkWidget *title_label = gtk_label_new(_("Configured repositories"));
  gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
  gtk_widget_add_css_class(title_label, "heading");
  gtk_widget_set_hexpand(title_label, TRUE);
  gtk_box_append(GTK_BOX(top_row), title_label);

  GtkWidget *refresh_button = ui_helpers_create_icon_button("view-refresh-symbolic", _("Refresh"));
  gtk_box_append(GTK_BOX(top_row), refresh_button);
  state->refresh_button = GTK_BUTTON(refresh_button);

  GtkWidget *clear_pending_button = ui_helpers_create_icon_button("edit-clear-symbolic", _("Clear Pending"));
  gtk_widget_set_sensitive(clear_pending_button, FALSE);
  gtk_box_append(GTK_BOX(top_row), clear_pending_button);
  state->clear_pending_button = GTK_BUTTON(clear_pending_button);

  GtkWidget *apply_button = ui_helpers_create_icon_button("system-run-symbolic", _("Apply"));
  gtk_widget_set_sensitive(apply_button, FALSE);
  gtk_box_append(GTK_BOX(top_row), apply_button);
  state->apply_button = GTK_BUTTON(apply_button);

  GtkWidget *status_label = gtk_label_new(_("Loading repositories..."));
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
  gtk_box_append(GTK_BOX(root), status_label);
  state->status_label = GTK_LABEL(status_label);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_box_append(GTK_BOX(root), scrolled);

  GtkWidget *column_view = repository_view_create_column_view(state);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), column_view);
  state->column_view = GTK_COLUMN_VIEW(column_view);

  GtkWidget *bottom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand(bottom_row, TRUE);
  gtk_box_append(GTK_BOX(root), bottom_row);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bottom_row), spacer);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(bottom_row), spinner);
  state->spinner = GTK_SPINNER(spinner);

  g_signal_connect(refresh_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
                     if (state_holder) {
                       repository_view_start_load(*state_holder);
                     }
                   }),
                   state_holder);
  g_signal_connect(clear_pending_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
                     if (state_holder) {
                       repository_view_clear_pending_changes(*state_holder);
                     }
                   }),
                   state_holder);
  g_signal_connect(apply_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
                     if (state_holder) {
                       repository_view_confirm_apply_changes(*state_holder);
                     }
                   }),
                   state_holder);

  repository_view_start_load(state);
  gtk_window_present(window);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
