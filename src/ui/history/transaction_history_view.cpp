// -----------------------------------------------------------------------------
// src/ui/history/transaction_history_view.cpp
// Read-only transaction history window
// -----------------------------------------------------------------------------
#include "ui/history/transaction_history_view.hpp"

#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <memory>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kHistoryTransactionLimit = 200;
constexpr size_t kHistoryPackageRowLimit = 2000;

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
  GCancellable *cancellable = nullptr;
  std::vector<TransactionHistoryPackageRow> rows;
  uint64_t load_id = 0;
  bool destroyed = false;
};

struct HistoryFilters {
  std::string package;
  std::string text;
  int64_t from = 0;
  int64_t to = std::numeric_limits<int64_t>::max();
  guint action_index = 0;
  guint result_index = 0;
  std::string error;
};

struct HistoryTaskUserData {
  std::shared_ptr<TransactionHistoryWindowState> state;
  uint64_t load_id = 0;
};

// -----------------------------------------------------------------------------
// Return ASCII-lowercase text for simple package history filtering.
// -----------------------------------------------------------------------------
std::string
history_filter_text(std::string text)
{
  std::transform(
      text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
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

  filters.package = history_filter_text(
      history_trim_filter_text(state->package_entry ? gtk_editable_get_text(GTK_EDITABLE(state->package_entry)) : ""));
  filters.text = history_filter_text(
      history_trim_filter_text(state->text_entry ? gtk_editable_get_text(GTK_EDITABLE(state->text_entry)) : ""));
  filters.action_index = state->action_dropdown ? gtk_drop_down_get_selected(state->action_dropdown) : 0;
  filters.result_index = state->result_dropdown ? gtk_drop_down_get_selected(state->result_dropdown) : 0;

  if (!history_parse_date_filter(state->from_entry ? gtk_editable_get_text(GTK_EDITABLE(state->from_entry)) : "",
                                 false,
                                 filters.from,
                                 filters.error)) {
    return filters;
  }

  if (!history_parse_date_filter(state->to_entry ? gtk_editable_get_text(GTK_EDITABLE(state->to_entry)) : "",
                                 true,
                                 filters.to,
                                 filters.error)) {
    return filters;
  }

  if (filters.from > filters.to) {
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
// Return true when a history row matches the selected action filter.
// -----------------------------------------------------------------------------
bool
history_row_matches_action(const TransactionHistoryPackageRow &row, guint action_index)
{
  switch (action_index) {
  case 1:
    return row.action == TransactionHistoryAction::INSTALL;
  case 2:
    return row.action == TransactionHistoryAction::UPGRADE;
  case 3:
    return row.action == TransactionHistoryAction::DOWNGRADE;
  case 4:
    return row.action == TransactionHistoryAction::REINSTALL;
  case 5:
    return row.action == TransactionHistoryAction::REMOVE;
  case 6:
    return row.action == TransactionHistoryAction::REPLACED;
  case 7:
    return row.action == TransactionHistoryAction::REASON_CHANGE;
  case 8:
    return row.action == TransactionHistoryAction::OTHER;
  case 0:
  default:
    return true;
  }
}

// -----------------------------------------------------------------------------
// Return true when a history row matches the selected result filter.
// -----------------------------------------------------------------------------
bool
history_row_matches_result(const TransactionHistoryPackageRow &row, guint result_index)
{
  switch (result_index) {
  case 1:
    return row.succeeded;
  case 2:
    return !row.succeeded;
  case 0:
  default:
    return true;
  }
}

// -----------------------------------------------------------------------------
// Return true when a history row matches the active filters.
// -----------------------------------------------------------------------------
bool
history_row_matches_filters(const TransactionHistoryPackageRow &row, const HistoryFilters &filters)
{
  if (!history_row_matches_action(row, filters.action_index) ||
      !history_row_matches_result(row, filters.result_index)) {
    return false;
  }

  if (row.started_at < filters.from || row.started_at > filters.to) {
    return false;
  }

  if (!filters.package.empty()) {
    std::string package_text = row.name;
    package_text += "\n";
    package_text += row.package_id;
    if (history_filter_text(package_text).find(filters.package) == std::string::npos) {
      return false;
    }
  }

  if (!filters.text.empty()) {
    std::string text = row.repo;
    text += "\n";
    text += row.description;
    text += "\n";
    text += row.arch;
    if (history_filter_text(text).find(filters.text) == std::string::npos) {
      return false;
    }
  }

  return true;
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
// Render loaded history rows using the current filters.
// -----------------------------------------------------------------------------
void
history_render_rows(const std::shared_ptr<TransactionHistoryWindowState> &state)
{
  if (!state || state->destroyed || !state->list_box) {
    return;
  }

  HistoryFilters filters = history_current_filters(state);

  history_list_clear(state->list_box);
  if (!filters.error.empty()) {
    gtk_label_set_text(GTK_LABEL(state->status_label), filters.error.c_str());
    return;
  }

  if (state->rows.empty()) {
    gtk_label_set_text(GTK_LABEL(state->status_label), _("No transaction history was found."));
    return;
  }

  size_t shown = 0;
  for (const auto &row : state->rows) {
    if (!history_row_matches_filters(row, filters)) {
      continue;
    }
    history_list_append_row(state->list_box, row);
    ++shown;
  }

  if (shown == 0) {
    gtk_label_set_text(GTK_LABEL(state->status_label), _("No transaction history rows match the filter."));
  } else {
    std::string status = dnfui_i18n_format_count(shown, "Showing %zu package change.", "Showing %zu package changes.");
    gtk_label_set_text(GTK_LABEL(state->status_label), status.c_str());
  }
}

// -----------------------------------------------------------------------------
// Reapply filters when one filter control changes.
// -----------------------------------------------------------------------------
void
on_history_filter_changed(gpointer user_data)
{
  auto *state_holder = static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(user_data);
  if (state_holder) {
    history_render_rows(*state_holder);
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
    auto *rows = new std::vector<TransactionHistoryPackageRow>(
        dnf_backend_list_transaction_history_rows(kHistoryTransactionLimit, kHistoryPackageRowLimit, cancellable));
    g_task_return_pointer(
        task, rows, [](gpointer p) { delete static_cast<std::vector<TransactionHistoryPackageRow> *>(p); });
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
  delete task_user_data;

  if (!state || state->destroyed || load_id != state->load_id) {
    return;
  }

  gtk_spinner_stop(state->spinner);
  gtk_widget_set_visible(GTK_WIDGET(state->spinner), FALSE);

  GTask *task = G_TASK(result);
  GError *error = nullptr;
  auto *rows = static_cast<std::vector<TransactionHistoryPackageRow> *>(g_task_propagate_pointer(task, &error));

  if (state->cancellable) {
    g_object_unref(state->cancellable);
    state->cancellable = nullptr;
  }

  if (!rows) {
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

  state->rows = std::move(*rows);
  delete rows;
  history_render_rows(state);
}

// -----------------------------------------------------------------------------
// Start or restart the background history load.
// -----------------------------------------------------------------------------
void
history_start_load(const std::shared_ptr<TransactionHistoryWindowState> &state)
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

  gtk_spinner_start(state->spinner);
  gtk_widget_set_visible(GTK_WIDGET(state->spinner), TRUE);
  gtk_label_set_text(state->status_label, _("Loading transaction history..."));
  history_list_clear(state->list_box);

  auto *task_user_data = new HistoryTaskUserData { state, state->load_id };
  GTask *task = g_task_new(state->window, state->cancellable, on_history_load_finished, task_user_data);
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

  GtkWidget *refresh_button = gtk_button_new_with_label(_("Refresh"));
  gtk_box_append(GTK_BOX(date_row), refresh_button);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(date_row), spinner);
  state->spinner = GTK_SPINNER(spinner);

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

  g_signal_connect(package_entry,
                   "changed",
                   G_CALLBACK(+[](GtkEditable *, gpointer user_data) { on_history_filter_changed(user_data); }),
                   state_holder);
  g_signal_connect(text_entry,
                   "changed",
                   G_CALLBACK(+[](GtkEditable *, gpointer user_data) { on_history_filter_changed(user_data); }),
                   state_holder);
  g_signal_connect(from_entry,
                   "changed",
                   G_CALLBACK(+[](GtkEditable *, gpointer user_data) { on_history_filter_changed(user_data); }),
                   state_holder);
  g_signal_connect(to_entry,
                   "changed",
                   G_CALLBACK(+[](GtkEditable *, gpointer user_data) { on_history_filter_changed(user_data); }),
                   state_holder);
  g_signal_connect(
      action_dropdown,
      "notify::selected",
      G_CALLBACK(+[](GtkDropDown *, GParamSpec *, gpointer user_data) { on_history_filter_changed(user_data); }),
      state_holder);
  g_signal_connect(
      result_dropdown,
      "notify::selected",
      G_CALLBACK(+[](GtkDropDown *, GParamSpec *, gpointer user_data) { on_history_filter_changed(user_data); }),
      state_holder);

  g_signal_connect(refresh_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(user_data);
                     if (state_holder) {
                       history_start_load(*state_holder);
                     }
                   }),
                   state_holder);

  gtk_window_present(window);
  history_start_load(state);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
