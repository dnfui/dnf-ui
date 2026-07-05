// -----------------------------------------------------------------------------
// src/ui/history/transaction_history_view.cpp
// Read-only transaction history window
// -----------------------------------------------------------------------------
#include "ui/history/transaction_history_view.hpp"

#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kHistoryTransactionLimit = 200;

struct TransactionHistoryWindowState {
  GtkWindow *window = nullptr;
  GtkEntry *search_entry = nullptr;
  GtkListBox *list_box = nullptr;
  GtkLabel *status_label = nullptr;
  GtkSpinner *spinner = nullptr;
  GCancellable *cancellable = nullptr;
  std::vector<TransactionHistoryPackageRow> rows;
  uint64_t load_id = 0;
  bool destroyed = false;
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
// Return true when a history row matches the current search text.
// -----------------------------------------------------------------------------
bool
history_row_matches_filter(const TransactionHistoryPackageRow &row, const std::string &filter)
{
  if (filter.empty()) {
    return true;
  }

  std::string text = row.package_id;
  text += "\n";
  text += row.name;
  text += "\n";
  text += row.arch;
  text += "\n";
  text += row.repo;
  text += "\n";
  text += row.description;
  text += "\n";
  text += dnf_backend_transaction_history_action_to_string(row.action);

  return history_filter_text(text).find(filter) != std::string::npos;
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
// Render loaded history rows using the current search filter.
// -----------------------------------------------------------------------------
void
history_render_rows(const std::shared_ptr<TransactionHistoryWindowState> &state)
{
  if (!state || state->destroyed || !state->list_box) {
    return;
  }

  const char *entry_text = state->search_entry ? gtk_editable_get_text(GTK_EDITABLE(state->search_entry)) : "";
  std::string filter = history_filter_text(entry_text ? entry_text : "");

  history_list_clear(state->list_box);

  size_t shown = 0;
  for (const auto &row : state->rows) {
    if (!history_row_matches_filter(row, filter)) {
      continue;
    }
    history_list_append_row(state->list_box, row);
    ++shown;
  }

  if (shown == 0) {
    gtk_label_set_text(GTK_LABEL(state->status_label), _("No transaction history rows match the filter."));
  } else {
    std::string status =
        dnfui_i18n_format_count(shown, "Showing %zu transaction history row.", "Showing %zu transaction history rows.");
    gtk_label_set_text(GTK_LABEL(state->status_label), status.c_str());
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
        dnf_backend_list_transaction_history_rows(kHistoryTransactionLimit));
    g_task_return_pointer(
        task, rows, [](gpointer p) { delete static_cast<std::vector<TransactionHistoryPackageRow> *>(p); });
  } catch (const std::exception &e) {
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

  GtkWidget *search_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), _("Filter transaction history..."));
  gtk_widget_set_hexpand(search_entry, TRUE);
  gtk_box_append(GTK_BOX(top_row), search_entry);
  state->search_entry = GTK_ENTRY(search_entry);

  GtkWidget *refresh_button = gtk_button_new_with_label(_("Refresh"));
  gtk_box_append(GTK_BOX(top_row), refresh_button);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(top_row), spinner);
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

  g_signal_connect(search_entry,
                   "changed",
                   G_CALLBACK(+[](GtkEditable *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<TransactionHistoryWindowState> *>(user_data);
                     if (state_holder) {
                       history_render_rows(*state_holder);
                     }
                   }),
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
