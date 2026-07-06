// -----------------------------------------------------------------------------
// src/ui/history/transaction_history_view.cpp
// Read-only transaction history window
// -----------------------------------------------------------------------------
#include "ui/history/transaction_history_view.hpp"

#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "ui/common/ui_helpers.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <memory>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kHistoryPackageRowsPerPage = 100;

struct TransactionHistoryWindowState {
  GtkWindow *window = nullptr;
  GtkEntry *package_entry = nullptr;
  GtkEntry *text_entry = nullptr;
  GtkEntry *from_entry = nullptr;
  GtkEntry *to_entry = nullptr;
  GtkDropDown *action_dropdown = nullptr;
  GtkDropDown *result_dropdown = nullptr;
  GtkListBox *list_box = nullptr;
  GtkLabel *status_label = nullptr;
  GtkSpinner *spinner = nullptr;
  GtkButton *search_button = nullptr;
  GtkButton *newer_button = nullptr;
  GtkButton *older_button = nullptr;
  GtkSpinButton *page_spin_button = nullptr;
  GtkButton *goto_button = nullptr;
  GCancellable *cancellable = nullptr;
  std::vector<TransactionHistoryPackageRow> rows;
  TransactionHistoryCursor current_cursor;
  TransactionHistoryCursor next_cursor;
  TransactionHistoryFilter current_filter;
  size_t current_page = 1;
  bool has_older_history = false;
  uint64_t load_id = 0;
  bool destroyed = false;
};

struct HistoryFilters {
  TransactionHistoryFilter backend;
  std::string error;
};

struct HistoryTaskUserData {
  std::shared_ptr<TransactionHistoryWindowState> state;
  uint64_t load_id = 0;
  TransactionHistoryCursor cursor;
  TransactionHistoryFilter filter;
};

void history_start_load(const std::shared_ptr<TransactionHistoryWindowState> &state,
                        TransactionHistoryCursor cursor,
                        const TransactionHistoryFilter &filter);

// -----------------------------------------------------------------------------
// Return true when the selected action row enables an action filter.
// -----------------------------------------------------------------------------
bool
history_filter_action_from_index(guint action_index, TransactionHistoryAction &action)
{
  switch (action_index) {
  case 1:
    action = TransactionHistoryAction::INSTALL;
    return true;
  case 2:
    action = TransactionHistoryAction::UPGRADE;
    return true;
  case 3:
    action = TransactionHistoryAction::DOWNGRADE;
    return true;
  case 4:
    action = TransactionHistoryAction::REINSTALL;
    return true;
  case 5:
    action = TransactionHistoryAction::REMOVE;
    return true;
  case 6:
    action = TransactionHistoryAction::REPLACED;
    return true;
  case 7:
    action = TransactionHistoryAction::REASON_CHANGE;
    return true;
  case 8:
    action = TransactionHistoryAction::OTHER;
    return true;
  case 0:
  default:
    action = TransactionHistoryAction::OTHER;
    return false;
  }
}

// -----------------------------------------------------------------------------
// Convert the selected result row to the backend history filter value.
// -----------------------------------------------------------------------------
TransactionHistoryResultFilter
history_result_filter_from_index(guint result_index)
{
  switch (result_index) {
  case 1:
    return TransactionHistoryResultFilter::OK;
  case 2:
    return TransactionHistoryResultFilter::FAILED;
  case 0:
  default:
    return TransactionHistoryResultFilter::ALL;
  }
}

// -----------------------------------------------------------------------------
// Return true when the current history filter narrows the page load.
// -----------------------------------------------------------------------------
bool
history_filter_is_active(const TransactionHistoryFilter &filter)
{
  return !filter.package_text.empty() || !filter.detail_text.empty() || filter.from != 0 ||
      filter.to != std::numeric_limits<int64_t>::max() || filter.action_enabled ||
      filter.result != TransactionHistoryResultFilter::ALL;
}

// -----------------------------------------------------------------------------
// Return the first matching row offset for one history page.
// -----------------------------------------------------------------------------
TransactionHistoryCursor
history_cursor_for_page(size_t page)
{
  return TransactionHistoryCursor::for_page(page, kHistoryPackageRowsPerPage);
}

// -----------------------------------------------------------------------------
// Return the one-based page number for a history cursor.
// -----------------------------------------------------------------------------
size_t
history_page_for_cursor(TransactionHistoryCursor cursor)
{
  return cursor.page(kHistoryPackageRowsPerPage);
}

// -----------------------------------------------------------------------------
// Remove simple ASCII whitespace from both ends of a filter field.
// -----------------------------------------------------------------------------
std::string
history_trim_filter_text(const char *text)
{
  std::string trimmed = text ? text : "";
  size_t start = 0;
  while (start < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[start]))) {
    ++start;
  }

  size_t end = trimmed.size();
  while (end > start && std::isspace(static_cast<unsigned char>(trimmed[end - 1]))) {
    --end;
  }

  return trimmed.substr(start, end - start);
}

// -----------------------------------------------------------------------------
// Parse a local date filter in YYYY-MM-DD format.
// -----------------------------------------------------------------------------
bool
history_parse_date_filter(const char *text, bool end_of_day, int64_t &timestamp, std::string &error)
{
  std::string value = history_trim_filter_text(text);
  if (value.empty()) {
    timestamp = end_of_day ? std::numeric_limits<int64_t>::max() : 0;
    return true;
  }

  if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
    error = _("Dates must use YYYY-MM-DD.");
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  char extra = 0;
  if (std::sscanf(value.c_str(), "%d-%d-%d%c", &year, &month, &day, &extra) != 3) {
    error = _("Dates must use YYYY-MM-DD.");
    return false;
  }

  std::tm local {};
  local.tm_year = year - 1900;
  local.tm_mon = month - 1;
  local.tm_mday = day;
  local.tm_hour = end_of_day ? 23 : 0;
  local.tm_min = end_of_day ? 59 : 0;
  local.tm_sec = end_of_day ? 59 : 0;
  local.tm_isdst = -1;

  std::time_t parsed = std::mktime(&local);
  if (parsed == static_cast<std::time_t>(-1) || local.tm_year != year - 1900 || local.tm_mon != month - 1 ||
      local.tm_mday != day) {
    error = _("Dates must use YYYY-MM-DD.");
    return false;
  }

  timestamp = static_cast<int64_t>(parsed);
  return true;
}

// -----------------------------------------------------------------------------
// Read the active history filters from the window controls.
// -----------------------------------------------------------------------------
HistoryFilters
history_current_filters(const std::shared_ptr<TransactionHistoryWindowState> &state)
{
  HistoryFilters filters;
  if (!state) {
    return filters;
  }

  filters.backend.package_text =
      history_trim_filter_text(state->package_entry ? gtk_editable_get_text(GTK_EDITABLE(state->package_entry)) : "");
  filters.backend.detail_text =
      history_trim_filter_text(state->text_entry ? gtk_editable_get_text(GTK_EDITABLE(state->text_entry)) : "");
  filters.backend.action_enabled = history_filter_action_from_index(
      state->action_dropdown ? gtk_drop_down_get_selected(state->action_dropdown) : 0, filters.backend.action);
  filters.backend.result =
      history_result_filter_from_index(state->result_dropdown ? gtk_drop_down_get_selected(state->result_dropdown) : 0);

  if (!history_parse_date_filter(state->from_entry ? gtk_editable_get_text(GTK_EDITABLE(state->from_entry)) : "",
                                 false,
                                 filters.backend.from,
                                 filters.error)) {
    return filters;
  }

  if (!history_parse_date_filter(state->to_entry ? gtk_editable_get_text(GTK_EDITABLE(state->to_entry)) : "",
                                 true,
                                 filters.backend.to,
                                 filters.error)) {
    return filters;
  }

  if (filters.backend.from > filters.backend.to) {
    filters.error = _("The start date must be before the end date.");
  }

  return filters;
}

// -----------------------------------------------------------------------------
// Format one transaction timestamp for display.
// -----------------------------------------------------------------------------
std::string
history_time_text(int64_t timestamp)
{
  if (timestamp <= 0) {
    return _("Unknown time");
  }

  std::time_t time = static_cast<std::time_t>(timestamp);
  std::tm local {};
  localtime_r(&time, &local);

  char buffer[64] = {};
  if (std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M", &local) == 0) {
    return _("Unknown time");
  }

  return buffer;
}

// -----------------------------------------------------------------------------
// Return the CSS class used for one history action.
// -----------------------------------------------------------------------------
const char *
history_action_css_class(TransactionHistoryAction action)
{
  switch (action) {
  case TransactionHistoryAction::INSTALL:
    return "transaction-history-install";
  case TransactionHistoryAction::UPGRADE:
    return "transaction-history-upgrade";
  case TransactionHistoryAction::DOWNGRADE:
    return "transaction-history-downgrade";
  case TransactionHistoryAction::REINSTALL:
    return "transaction-history-reinstall";
  case TransactionHistoryAction::REMOVE:
    return "transaction-history-remove";
  case TransactionHistoryAction::REPLACED:
    return "transaction-history-replaced";
  case TransactionHistoryAction::REASON_CHANGE:
    return "transaction-history-reason";
  case TransactionHistoryAction::OTHER:
  default:
    return "transaction-history-other";
  }
}

// -----------------------------------------------------------------------------
// Remove all rows from one GTK list box.
// -----------------------------------------------------------------------------
void
history_list_clear(GtkListBox *list_box)
{
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list_box));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(list_box, child);
    child = next;
  }
}

// -----------------------------------------------------------------------------
// Update controls that depend on whether a history load is currently running.
// -----------------------------------------------------------------------------
void
history_set_loading(const std::shared_ptr<TransactionHistoryWindowState> &state, bool loading)
{
  if (!state || state->destroyed) {
    return;
  }

  gtk_widget_set_sensitive(GTK_WIDGET(state->search_button), !loading);
  gtk_widget_set_sensitive(GTK_WIDGET(state->newer_button), !loading && state->current_page > 1);
  gtk_widget_set_sensitive(GTK_WIDGET(state->older_button), !loading && state->has_older_history);
  gtk_widget_set_sensitive(GTK_WIDGET(state->page_spin_button), !loading);
  gtk_widget_set_sensitive(GTK_WIDGET(state->goto_button), !loading);

  if (loading) {
    gtk_spinner_start(state->spinner);
    gtk_widget_set_visible(GTK_WIDGET(state->spinner), TRUE);
  } else {
    gtk_spinner_stop(state->spinner);
    gtk_widget_set_visible(GTK_WIDGET(state->spinner), FALSE);
  }
}

// -----------------------------------------------------------------------------
// Add one package history row to the list.
// -----------------------------------------------------------------------------
void
history_list_append_row(GtkListBox *list_box, const TransactionHistoryPackageRow &row)
{
  GtkWidget *list_row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_add_css_class(box, "transaction-history-row");
  gtk_widget_add_css_class(box, history_action_css_class(row.action));
  if (!row.succeeded) {
    gtk_widget_add_css_class(box, "transaction-history-failed");
  }

  std::string title = history_time_text(row.started_at);
  title += "   ";
  title += dnf_backend_transaction_history_action_to_string(row.action);
  title += "   ";
  title += row.package_id.empty() ? row.name : row.package_id;

  GtkWidget *title_label = gtk_label_new(title.c_str());
  gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(title_label), TRUE);
  gtk_box_append(GTK_BOX(box), title_label);

  std::ostringstream details;
  details << _("Transaction") << " " << row.transaction_id;
  if (!row.repo.empty()) {
    details << "   " << _("Repo") << ": " << row.repo;
  }
  details << "   " << _("Result") << ": " << (row.succeeded ? _("OK") : _("Failed"));

  GtkWidget *details_label = gtk_label_new(details.str().c_str());
  gtk_label_set_xalign(GTK_LABEL(details_label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(details_label), TRUE);
  gtk_widget_add_css_class(details_label, "package-meta");
  gtk_box_append(GTK_BOX(box), details_label);

  if (!row.description.empty()) {
    std::string description = _("Command");
    description += ": ";
    description += row.description;

    GtkWidget *description_label = gtk_label_new(description.c_str());
    gtk_label_set_xalign(GTK_LABEL(description_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(description_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(description_label), TRUE);
    gtk_widget_add_css_class(description_label, "package-meta");
    gtk_box_append(GTK_BOX(box), description_label);
  }

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), box);
  gtk_list_box_append(list_box, list_row);
}

// -----------------------------------------------------------------------------
// Render the current page returned by the backend history query.
// -----------------------------------------------------------------------------
void
history_render_rows(const std::shared_ptr<TransactionHistoryWindowState> &state)
{
  if (!state || state->destroyed || !state->list_box) {
    return;
  }

  history_list_clear(state->list_box);

  if (state->rows.empty()) {
    if (state->current_cursor.row_offset > 0) {
      gtk_label_set_text(GTK_LABEL(state->status_label), _("No transaction history rows were found on this page."));
    } else {
      gtk_label_set_text(GTK_LABEL(state->status_label),
                         history_filter_is_active(state->current_filter)
                             ? _("No transaction history rows match the filter.")
                             : _("No transaction history was found."));
    }
    return;
  }

  for (const auto &row : state->rows) {
    history_list_append_row(state->list_box, row);
  }

  std::string status =
      dnfui_i18n_format_count(state->rows.size(), "Showing %zu package change.", "Showing %zu package changes.");

  status += " ";
  status += dnfui_i18n_format(_("Page %zu."), state->current_page);

  if (state->has_older_history) {
    status += " ";
    status += _("Use Older to show more history.");
  }
  gtk_label_set_text(GTK_LABEL(state->status_label), status.c_str());
}

// -----------------------------------------------------------------------------
// Cancel the active history load and release the window reference to its cancellable.
// -----------------------------------------------------------------------------
void
history_cancel_active_load(const std::shared_ptr<TransactionHistoryWindowState> &state)
{
  if (!state || state->destroyed || !state->cancellable) {
    return;
  }

  g_cancellable_cancel(state->cancellable);
  g_object_unref(state->cancellable);
  state->cancellable = nullptr;
}

// -----------------------------------------------------------------------------
// Show one filter error and prevent an older worker from replacing the message.
// -----------------------------------------------------------------------------
void
history_show_filter_error(const std::shared_ptr<TransactionHistoryWindowState> &state, const std::string &error)
{
  if (!state || state->destroyed) {
    return;
  }

  history_cancel_active_load(state);
  ++state->load_id;
  state->rows.clear();
  state->current_cursor = TransactionHistoryCursor {};
  state->next_cursor = TransactionHistoryCursor {};
  state->current_page = 1;
  state->has_older_history = false;
  if (state->page_spin_button) {
    gtk_spin_button_set_value(state->page_spin_button, 1);
  }
  history_list_clear(state->list_box);
  history_set_loading(state, false);
  gtk_label_set_text(GTK_LABEL(state->status_label), error.c_str());
}

// -----------------------------------------------------------------------------
// Start a fresh backend load from the current filter controls.
// -----------------------------------------------------------------------------
void
history_reload_from_controls(const std::shared_ptr<TransactionHistoryWindowState> &state)
{
  HistoryFilters filters = history_current_filters(state);
  if (!filters.error.empty()) {
    history_show_filter_error(state, filters.error);
    return;
  }

  history_start_load(state, TransactionHistoryCursor {}, filters.backend);
}

// -----------------------------------------------------------------------------
// Load one requested page using the filters currently shown in the window.
// -----------------------------------------------------------------------------
void
history_load_page_from_controls(const std::shared_ptr<TransactionHistoryWindowState> &state, size_t page)
{
  HistoryFilters filters = history_current_filters(state);
  if (!filters.error.empty()) {
    history_show_filter_error(state, filters.error);
    return;
  }

  history_start_load(state, history_cursor_for_page(page), filters.backend);
}

// -----------------------------------------------------------------------------
// Search history with the current filters.
// -----------------------------------------------------------------------------
void
on_history_search_requested(gpointer user_data)
{
  auto *state_holder = static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(user_data);
  if (state_holder) {
    history_reload_from_controls(*state_holder);
  }
}

// -----------------------------------------------------------------------------
// Load history rows on a worker thread.
// -----------------------------------------------------------------------------
void
on_history_load_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("History load was cancelled."));
    return;
  }

  try {
    const auto *task_data = static_cast<const HistoryTaskUserData *>(g_task_get_task_data(task));
    const TransactionHistoryCursor cursor = task_data ? task_data->cursor : TransactionHistoryCursor {};
    const TransactionHistoryFilter filter = task_data ? task_data->filter : TransactionHistoryFilter {};
    auto *page = new TransactionHistoryPage(
        dnf_backend_list_transaction_history_page(cursor, filter, kHistoryPackageRowsPerPage, cancellable));
    g_task_return_pointer(task, page, [](gpointer p) { delete static_cast<TransactionHistoryPage *>(p); });
  } catch (const std::exception &e) {
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("History load was cancelled."));
      return;
    }
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish loading history rows on the GTK thread.
// -----------------------------------------------------------------------------
void
on_history_load_finished(GObject *, GAsyncResult *result, gpointer user_data)
{
  auto *task_user_data = static_cast<HistoryTaskUserData *>(user_data);
  std::shared_ptr<TransactionHistoryWindowState> state = task_user_data ? task_user_data->state : nullptr;
  uint64_t load_id = task_user_data ? task_user_data->load_id : 0;
  TransactionHistoryCursor requested_cursor = task_user_data ? task_user_data->cursor : TransactionHistoryCursor {};
  TransactionHistoryFilter requested_filter = task_user_data ? task_user_data->filter : TransactionHistoryFilter {};
  delete task_user_data;

  if (!state || state->destroyed || load_id != state->load_id) {
    return;
  }

  history_set_loading(state, false);

  GTask *task = G_TASK(result);
  GError *error = nullptr;
  auto *page = static_cast<TransactionHistoryPage *>(g_task_propagate_pointer(task, &error));

  if (state->cancellable) {
    g_object_unref(state->cancellable);
    state->cancellable = nullptr;
  }

  if (!page) {
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      std::string message = _("Failed to load transaction history.");
      message += " ";
      message += error->message;
      gtk_label_set_text(state->status_label, message.c_str());
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  state->current_cursor = requested_cursor;
  state->current_page = history_page_for_cursor(requested_cursor);
  state->rows = std::move(page->rows);
  state->next_cursor = page->next_cursor;
  state->current_filter = requested_filter;
  state->has_older_history = page->has_more;
  if (state->page_spin_button) {
    gtk_spin_button_set_value(state->page_spin_button, static_cast<double>(state->current_page));
  }

  delete page;
  history_set_loading(state, false);
  history_render_rows(state);
}

// -----------------------------------------------------------------------------
// Start or restart the background history load for one page of package changes.
// -----------------------------------------------------------------------------
void
history_start_load(const std::shared_ptr<TransactionHistoryWindowState> &state,
                   TransactionHistoryCursor cursor,
                   const TransactionHistoryFilter &filter)
{
  if (!state || state->destroyed) {
    return;
  }

  if (state->cancellable) {
    g_cancellable_cancel(state->cancellable);
    g_object_unref(state->cancellable);
  }

  state->cancellable = g_cancellable_new();
  ++state->load_id;

  state->rows.clear();
  history_list_clear(state->list_box);

  history_set_loading(state, true);
  gtk_label_set_text(state->status_label, _("Loading transaction history..."));

  auto *task_user_data = new HistoryTaskUserData { state, state->load_id, cursor, filter };
  GTask *task = g_task_new(state->window, state->cancellable, on_history_load_finished, task_user_data);
  g_task_set_task_data(task, task_user_data, nullptr);
  g_task_run_in_thread(task, on_history_load_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Mark the history window as destroyed and cancel any current load.
// -----------------------------------------------------------------------------
void
on_history_window_destroy(GtkWidget *, gpointer user_data)
{
  auto *state_holder = static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(user_data);
  if (!state_holder || !*state_holder) {
    return;
  }

  auto state = *state_holder;
  state->destroyed = true;
  if (state->cancellable) {
    g_cancellable_cancel(state->cancellable);
    g_object_unref(state->cancellable);
    state->cancellable = nullptr;
  }
}

}

// -----------------------------------------------------------------------------
// Open the read-only transaction history window.
// -----------------------------------------------------------------------------
void
transaction_history_show_window(GtkWindow *parent)
{
  auto state = std::make_shared<TransactionHistoryWindowState>();

  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  state->window = window;
  gtk_window_set_title(window, _("Transaction History"));
  gtk_window_set_default_size(window, 820, 560);
  gtk_window_set_transient_for(window, parent);

  auto *state_holder = new std::shared_ptr<TransactionHistoryWindowState>(state);
  g_object_set_data_full(G_OBJECT(window), "dnfui-transaction-history-state", state_holder, [](gpointer p) {
    delete static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(p);
  });
  g_signal_connect(window, "destroy", G_CALLBACK(on_history_window_destroy), state_holder);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(root, 12);
  gtk_widget_set_margin_end(root, 12);
  gtk_widget_set_margin_top(root, 12);
  gtk_widget_set_margin_bottom(root, 12);
  gtk_window_set_child(window, root);

  GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(root), top_row);

  GtkWidget *package_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(package_entry), _("Package name..."));
  gtk_widget_set_hexpand(package_entry, TRUE);
  gtk_box_append(GTK_BOX(top_row), package_entry);
  state->package_entry = GTK_ENTRY(package_entry);

  GtkWidget *text_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(text_entry), _("Command, repository, or architecture..."));
  gtk_widget_set_hexpand(text_entry, TRUE);
  gtk_box_append(GTK_BOX(top_row), text_entry);
  state->text_entry = GTK_ENTRY(text_entry);

  const char *action_labels[] = {
    _("All actions"), _("Install"),  _("Upgrade"),        _("Downgrade"), _("Reinstall"),
    _("Remove"),      _("Replaced"), _("Reason changed"), _("Other"),     nullptr,
  };
  GtkWidget *action_dropdown = gtk_drop_down_new_from_strings(action_labels);
  gtk_box_append(GTK_BOX(top_row), action_dropdown);
  state->action_dropdown = GTK_DROP_DOWN(action_dropdown);

  const char *result_labels[] = {
    _("All results"),
    _("OK"),
    _("Failed"),
    nullptr,
  };
  GtkWidget *result_dropdown = gtk_drop_down_new_from_strings(result_labels);
  gtk_box_append(GTK_BOX(top_row), result_dropdown);
  state->result_dropdown = GTK_DROP_DOWN(result_dropdown);

  GtkWidget *date_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(root), date_row);

  GtkWidget *from_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(from_entry), _("From date YYYY-MM-DD"));
  gtk_widget_set_hexpand(from_entry, TRUE);
  gtk_box_append(GTK_BOX(date_row), from_entry);
  state->from_entry = GTK_ENTRY(from_entry);

  GtkWidget *to_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(to_entry), _("To date YYYY-MM-DD"));
  gtk_widget_set_hexpand(to_entry, TRUE);
  gtk_box_append(GTK_BOX(date_row), to_entry);
  state->to_entry = GTK_ENTRY(to_entry);

  GtkWidget *search_button = ui_helpers_create_icon_button("system-search-symbolic", _("Search"));
  gtk_box_append(GTK_BOX(date_row), search_button);
  state->search_button = GTK_BUTTON(search_button);

  GtkWidget *status_label = gtk_label_new(_("Loading transaction history..."));
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
  gtk_box_append(GTK_BOX(root), status_label);
  state->status_label = GTK_LABEL(status_label);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_box_append(GTK_BOX(root), scrolled);

  GtkWidget *list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list_box);
  state->list_box = GTK_LIST_BOX(list_box);

  GtkWidget *navigation_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand(navigation_row, TRUE);
  gtk_box_append(GTK_BOX(root), navigation_row);

  GtkWidget *page_label = gtk_label_new(_("Page"));
  gtk_box_append(GTK_BOX(navigation_row), page_label);

  GtkAdjustment *page_adjustment = gtk_adjustment_new(1, 1, G_MAXINT, 1, 10, 0);
  GtkWidget *page_spin_button = gtk_spin_button_new(page_adjustment, 1, 0);
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(page_spin_button), TRUE);
  gtk_widget_set_size_request(page_spin_button, 80, -1);
  gtk_box_append(GTK_BOX(navigation_row), page_spin_button);
  state->page_spin_button = GTK_SPIN_BUTTON(page_spin_button);

  GtkWidget *goto_button = ui_helpers_create_icon_button("go-jump-symbolic", _("Go"));
  gtk_box_append(GTK_BOX(navigation_row), goto_button);
  state->goto_button = GTK_BUTTON(goto_button);

  GtkWidget *navigation_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(navigation_spacer, TRUE);
  gtk_box_append(GTK_BOX(navigation_row), navigation_spacer);

  GtkWidget *newer_button = ui_helpers_create_icon_button("go-previous-symbolic", _("Newer"));
  gtk_widget_set_sensitive(newer_button, FALSE);
  gtk_box_append(GTK_BOX(navigation_row), newer_button);
  state->newer_button = GTK_BUTTON(newer_button);

  GtkWidget *older_button = ui_helpers_create_icon_button("go-next-symbolic", _("Older"));
  gtk_widget_set_sensitive(older_button, FALSE);
  gtk_box_append(GTK_BOX(navigation_row), older_button);
  state->older_button = GTK_BUTTON(older_button);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(navigation_row), spinner);
  state->spinner = GTK_SPINNER(spinner);

  g_signal_connect(package_entry,
                   "activate",
                   G_CALLBACK(+[](GtkEntry *, gpointer user_data) { on_history_search_requested(user_data); }),
                   state_holder);
  g_signal_connect(text_entry,
                   "activate",
                   G_CALLBACK(+[](GtkEntry *, gpointer user_data) { on_history_search_requested(user_data); }),
                   state_holder);
  g_signal_connect(from_entry,
                   "activate",
                   G_CALLBACK(+[](GtkEntry *, gpointer user_data) { on_history_search_requested(user_data); }),
                   state_holder);
  g_signal_connect(to_entry,
                   "activate",
                   G_CALLBACK(+[](GtkEntry *, gpointer user_data) { on_history_search_requested(user_data); }),
                   state_holder);

  g_signal_connect(search_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(user_data);
                     if (state_holder) {
                       history_reload_from_controls(*state_holder);
                     }
                   }),
                   state_holder);

  g_signal_connect(goto_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(user_data);
                     if (!state_holder || !*state_holder || !(*state_holder)->page_spin_button) {
                       return;
                     }

                     int page = gtk_spin_button_get_value_as_int((*state_holder)->page_spin_button);
                     if (page < 1) {
                       page = 1;
                     }
                     history_load_page_from_controls(*state_holder, static_cast<size_t>(page));
                   }),
                   state_holder);

  g_signal_connect(newer_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(user_data);
                     if (state_holder && *state_holder && (*state_holder)->current_page > 1) {
                       history_start_load(*state_holder,
                                          history_cursor_for_page((*state_holder)->current_page - 1),
                                          (*state_holder)->current_filter);
                     }
                   }),
                   state_holder);

  g_signal_connect(older_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(user_data);
                     if (state_holder && *state_holder && (*state_holder)->has_older_history) {
                       history_start_load(*state_holder, (*state_holder)->next_cursor, (*state_holder)->current_filter);
                     }
                   }),
                   state_holder);

  gtk_window_present(window);
  history_start_load(state, TransactionHistoryCursor {}, TransactionHistoryFilter {});
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
