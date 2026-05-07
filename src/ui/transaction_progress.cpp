// -----------------------------------------------------------------------------
// src/ui/transaction_progress.cpp
// Transaction progress window helpers
// Keeps the live apply progress window separate from transaction review dialogs.
// -----------------------------------------------------------------------------
#include "transaction_progress.hpp"

#include "i18n.hpp"
#include "widgets.hpp"

#include <atomic>
#include <sstream>

// -----------------------------------------------------------------------------
// Transaction progress popup state
// -----------------------------------------------------------------------------
// The GTK widgets inside this state still belong to the main thread, but the
// state object itself can outlive the window for a short time.
//
// Ownership rules:
//   - The newly created progress window starts with one reference for the live window.
//   - The apply task keeps one reference while background work may still report progress.
//   - Each queued main-loop progress callback keeps one temporary reference until it runs.
//
// Any new queued callback or long-lived background handoff of this pointer must
// retain it before the handoff and release it after that work is done so delayed
// callbacks never read freed state.
// -----------------------------------------------------------------------------
struct TransactionProgressWindow {
  std::atomic<unsigned> ref_count { 1 };
  GtkWindow *window = nullptr;
  GtkLabel *title_label = nullptr;
  GtkLabel *stage_label = nullptr;
  GtkTextBuffer *buffer = nullptr;
  GtkTextView *view = nullptr;
  GtkSpinner *spinner = nullptr;
  GtkButton *close_button = nullptr;
  bool finished = false;
};

struct ProgressAppendData {
  TransactionProgressWindow *progress = nullptr;
  char *message;
};

// -----------------------------------------------------------------------------
// Free data owned by one queued progress message.
// -----------------------------------------------------------------------------
static void
progress_append_data_free(ProgressAppendData *data)
{
  if (!data) {
    return;
  }

  transaction_progress_release(data->progress);
  g_free(data->message);
  delete data;
}

// -----------------------------------------------------------------------------
// Retain one reference to the progress window state so queued main loop work
// can safely keep using it after the caller returns.
// -----------------------------------------------------------------------------
TransactionProgressWindow *
transaction_progress_retain(TransactionProgressWindow *progress)
{
  if (!progress) {
    return nullptr;
  }

  progress->ref_count.fetch_add(1, std::memory_order_relaxed);
  return progress;
}

// -----------------------------------------------------------------------------
// Release one reference to the progress window state and delete it after the
// last owner is done with it.
// -----------------------------------------------------------------------------
void
transaction_progress_release(TransactionProgressWindow *progress)
{
  if (!progress) {
    return;
  }

  if (progress->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete progress;
  }
}

// -----------------------------------------------------------------------------
// Build the transaction popup used for streaming package install output
// -----------------------------------------------------------------------------
TransactionProgressWindow *
transaction_progress_create_window(SearchWidgets *widgets, size_t pending_count)
{
  auto *progress = new TransactionProgressWindow();
  progress->finished = false;

  progress->window = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(progress->window, _("Transaction Progress"));
  gtk_window_set_default_size(progress->window, 760, 420);
  gtk_window_set_modal(progress->window, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
  if (root && GTK_IS_WINDOW(root)) {
    GtkWindow *parent = GTK_WINDOW(root);
    if (GtkApplication *app = gtk_window_get_application(parent)) {
      gtk_window_set_application(progress->window, app);
    }
    gtk_window_set_transient_for(progress->window, parent);
  }

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(progress->window, outer);

  progress->title_label = GTK_LABEL(gtk_label_new(nullptr));
  std::string title_text;
  if (pending_count == 0) {
    char *markup = g_markup_printf_escaped("<b>%s</b>", _("Applying package transaction"));
    title_text = markup ? markup : "";
    g_free(markup);
  } else {
    std::string count_text = dnfui_i18n_format_count(
        pending_count, "Applying %zu pending package change", "Applying %zu pending package changes");
    char *markup = g_markup_printf_escaped("<b>%s</b>", count_text.c_str());
    title_text = markup ? markup : "";
    g_free(markup);
  }
  gtk_label_set_markup(progress->title_label, title_text.c_str());
  gtk_label_set_xalign(progress->title_label, 0.0f);
  gtk_box_append(GTK_BOX(outer), GTK_WIDGET(progress->title_label));

  GtkWidget *stage_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(outer), stage_box);

  progress->spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_spinner_start(progress->spinner);
  gtk_box_append(GTK_BOX(stage_box), GTK_WIDGET(progress->spinner));

  progress->stage_label = GTK_LABEL(gtk_label_new(_("Resolving dependency changes...")));
  gtk_label_set_xalign(progress->stage_label, 0.0f);
  gtk_widget_set_hexpand(GTK_WIDGET(progress->stage_label), TRUE);
  gtk_box_append(GTK_BOX(stage_box), GTK_WIDGET(progress->stage_label));

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_append(GTK_BOX(outer), scroller);

  progress->view = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_editable(progress->view, FALSE);
  gtk_text_view_set_cursor_visible(progress->view, FALSE);
  gtk_text_view_set_monospace(progress->view, TRUE);
  gtk_text_view_set_wrap_mode(progress->view, GTK_WRAP_WORD_CHAR);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(progress->view));
  progress->buffer = gtk_text_view_get_buffer(progress->view);

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  progress->close_button = GTK_BUTTON(gtk_button_new_with_label(_("Close")));
  gtk_widget_set_sensitive(GTK_WIDGET(progress->close_button), FALSE);
  gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(progress->close_button));

  g_signal_connect(progress->close_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *progress = static_cast<TransactionProgressWindow *>(user_data);
                     gtk_window_destroy(progress->window);
                   }),
                   progress);

  g_signal_connect(progress->window,
                   "close-request",
                   G_CALLBACK(+[](GtkWindow *, gpointer user_data) -> gboolean {
                     auto *progress = static_cast<TransactionProgressWindow *>(user_data);
                     return progress->finished ? FALSE : TRUE;
                   }),
                   progress);

  g_signal_connect(progress->window,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer user_data) {
                     auto *progress = static_cast<TransactionProgressWindow *>(user_data);
                     if (!progress) {
                       return;
                     }

                     progress->window = nullptr;
                     progress->title_label = nullptr;
                     progress->stage_label = nullptr;
                     progress->buffer = nullptr;
                     progress->view = nullptr;
                     progress->spinner = nullptr;
                     progress->close_button = nullptr;
                     transaction_progress_release(progress);
                   }),
                   progress);

  gtk_window_present(progress->window);

  return progress;
}

// -----------------------------------------------------------------------------
// Queue one progress line onto the GTK main loop. The caller may be a worker
// thread, but GTK widgets must only be touched by the main thread.
// -----------------------------------------------------------------------------
static void
append_transaction_progress_line(TransactionProgressWindow *progress, const std::string &message)
{
  if (!progress || message.empty()) {
    return;
  }

  auto *data = new ProgressAppendData();
  data->progress = transaction_progress_retain(progress);
  data->message = g_strdup(message.c_str());

  g_main_context_invoke(
      nullptr,
      +[](gpointer user_data) -> gboolean {
        auto *data = static_cast<ProgressAppendData *>(user_data);
        TransactionProgressWindow *progress = data ? data->progress : nullptr;

        if (!progress || !progress->stage_label || !progress->buffer || !progress->view) {
          progress_append_data_free(data);
          return G_SOURCE_REMOVE;
        }

        gtk_label_set_text(progress->stage_label, data->message);

        GtkTextIter end;
        gtk_text_buffer_get_end_iter(progress->buffer, &end);
        gtk_text_buffer_insert(progress->buffer, &end, data->message, -1);
        gtk_text_buffer_insert(progress->buffer, &end, "\n", 1);

        gtk_text_buffer_get_end_iter(progress->buffer, &end);
        GtkTextMark *mark = gtk_text_buffer_create_mark(progress->buffer, nullptr, &end, FALSE);
        gtk_text_view_scroll_mark_onscreen(progress->view, mark);
        gtk_text_buffer_delete_mark(progress->buffer, mark);

        progress_append_data_free(data);
        return G_SOURCE_REMOVE;
      },
      data);
}

// -----------------------------------------------------------------------------
// Split a message into lines and queue each line for the GTK main thread.
// -----------------------------------------------------------------------------
void
transaction_progress_append(TransactionProgressWindow *progress, const std::string &message)
{
  if (!progress || message.empty()) {
    return;
  }

  std::istringstream stream(message);
  std::string line;

  while (std::getline(stream, line)) {
    if (!line.empty()) {
      append_transaction_progress_line(progress, line);
    }
  }
}

// -----------------------------------------------------------------------------
// Mark the popup finished. After this the user may close it.
// -----------------------------------------------------------------------------
void
transaction_progress_finish(TransactionProgressWindow *progress, bool success, const std::string &summary)
{
  if (!progress) {
    return;
  }

  if (!summary.empty()) {
    transaction_progress_append(progress, summary);
  }

  progress->finished = true;
  if (progress->spinner) {
    gtk_spinner_stop(progress->spinner);
    gtk_widget_set_visible(GTK_WIDGET(progress->spinner), FALSE);
  }
  if (progress->close_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(progress->close_button), TRUE);
  }
  if (progress->stage_label) {
    gtk_label_set_text(progress->stage_label,
                       success ? _("Transaction finished successfully.") : _("Transaction finished with errors."));
  }
  if (progress->window) {
    gtk_window_set_title(progress->window, success ? _("Transaction Complete") : _("Transaction Failed"));
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
