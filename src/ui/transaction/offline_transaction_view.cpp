// -----------------------------------------------------------------------------
// Offline transaction view
// Keeps status checks and privileged cleanup off the GTK thread.
// -----------------------------------------------------------------------------
#include "ui/transaction/offline_transaction_view.hpp"

#include "dnf5daemon_client/transaction_service_client.hpp"
#include "i18n.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"
#include "ui/transaction/pending_transaction_apply.hpp"

struct OfflineViewState {
  GtkLabel *message = nullptr;
  GtkButton *discard = nullptr;
  GtkButton *refresh = nullptr;
  GtkButton *close = nullptr;
  bool destroyed = false;
  bool discarding = false;
};

struct OfflineViewTask {
  bool discard = false;
  OfflineTransactionStatus status;
};

// -----------------------------------------------------------------------------
// Describe only what the daemon confirms. Prepared changes are not installed yet.
// -----------------------------------------------------------------------------
static const char *
offline_status_message(const OfflineTransactionStatus &status)
{
  if (status.scheduled && status.state == "ready") {
    return _("Updates are prepared for your next reboot. No packages have been changed by this preparation. "
             "Restart the computer when you are ready to install them. Discarding removes the prepared changes "
             "and their downloaded packages.");
  }
  if (status.state == "transaction-incomplete") {
    return _("The offline transaction did not complete. Review Transaction History before preparing another "
             "transaction. Discarding removes the stored transaction data; it does not undo installed changes.");
  }
  if (!status.state.empty()) {
    return _("DNF has stored transaction data, but has not confirmed valid updates scheduled for reboot. "
             "Discard this data before preparing a new preview. Discarding also removes its downloaded packages.");
  }
  if (status.boot_trigger_present) {
    return _("An offline update boot is scheduled, but DNF has no prepared transaction to show. "
             "Use the tool that scheduled the update to manage it.");
  }
  return _("No updates are prepared for reboot. Use Transaction History to review completed package changes.");
}

// -----------------------------------------------------------------------------
// Fetch current state after opening the window or discarding prepared changes.
// -----------------------------------------------------------------------------
static void
start_offline_view_task(GtkWindow *window, bool discard)
{
  auto *state = static_cast<OfflineViewState *>(g_object_get_data(G_OBJECT(window), "offline-view-state"));
  if (!state || state->destroyed) {
    return;
  }
  state->discarding = discard;
  gtk_widget_set_sensitive(GTK_WIDGET(state->close), !discard);
  gtk_widget_set_sensitive(GTK_WIDGET(state->discard), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(state->refresh), FALSE);
  gtk_label_set_text(state->message, discard ? _("Discarding prepared changes...") : _("Checking prepared updates..."));

  GCancellable *cancellable = widgets_make_task_cancellable_for(GTK_WIDGET(window));
  GTask *task = g_task_new(
      window,
      cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer) {
        auto *state = static_cast<OfflineViewState *>(g_object_get_data(source, "offline-view-state"));
        auto *task = G_TASK(result);
        auto *data = static_cast<OfflineViewTask *>(g_task_get_task_data(task));
        GError *error = nullptr;
        bool success = g_task_propagate_boolean(task, &error);
        if (state && !state->destroyed) {
          state->discarding = false;
          gtk_widget_set_sensitive(GTK_WIDGET(state->close), TRUE);
          gtk_widget_set_sensitive(GTK_WIDGET(state->refresh), TRUE);
          if (success) {
            gtk_label_set_text(state->message, offline_status_message(data->status));
            gtk_widget_set_sensitive(GTK_WIDGET(state->discard), !data->status.state.empty());
          } else {
            gtk_label_set_text(state->message, error ? error->message : _("Could not check prepared updates."));
          }
        }
        g_clear_error(&error);
      },
      nullptr);
  auto *data = new OfflineViewTask;
  data->discard = discard;
  g_task_set_task_data(task, data, +[](gpointer p) { delete static_cast<OfflineViewTask *>(p); });
  g_task_run_in_thread(
      task, +[](GTask *task, gpointer, gpointer task_data, GCancellable *cancellable) {
        auto *data = static_cast<OfflineViewTask *>(task_data);
        std::string error;
        bool ok = true;
        if (data->discard) {
          ok = transaction_service_client_clear_offline_transaction(error, cancellable);
        }
        if (ok) {
          ok = transaction_service_client_get_offline_status(data->status, error, cancellable);
        }
        if (ok) {
          g_task_return_boolean(task, TRUE);
        } else {
          g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.c_str());
        }
      });
  g_object_unref(task);
  g_object_unref(cancellable);
}

// -----------------------------------------------------------------------------
// Use one modal window so cleanup cannot overlap a new transaction from this UI.
// -----------------------------------------------------------------------------
void
offline_transaction_view_show(MainWindowUiState *widgets)
{
  if (!widgets || widgets->window_state.destroyed || pending_transaction_apply_is_busy(widgets) ||
      pending_transaction_preview_is_busy(widgets) || !widgets->transaction_state.preview_transaction_path.empty()) {
    return;
  }
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
  if (!root || !GTK_IS_WINDOW(root)) {
    return;
  }
  auto *existing = static_cast<GtkWindow *>(g_object_get_data(G_OBJECT(root), "offline-view-window"));
  if (existing) {
    gtk_window_present(existing);
    return;
  }

  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(window, _("Updates Prepared for Reboot"));
  gtk_window_set_default_size(window, 580, 220);
  gtk_window_set_transient_for(window, GTK_WINDOW(root));
  gtk_window_set_destroy_with_parent(window, TRUE);
  gtk_window_set_modal(window, TRUE);
  gtk_window_set_application(window, gtk_window_get_application(GTK_WINDOW(root)));
  g_object_set_data(G_OBJECT(root), "offline-view-window", window);

  auto *state = new OfflineViewState;
  g_object_set_data_full(
      G_OBJECT(window), "offline-view-state", state, +[](gpointer p) { delete static_cast<OfflineViewState *>(p); });
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_window_set_child(window, box);

  state->message = GTK_LABEL(gtk_label_new(nullptr));
  gtk_label_set_wrap(state->message, TRUE);
  gtk_label_set_xalign(state->message, 0.0f);
  gtk_label_set_selectable(state->message, TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(state->message), TRUE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->message));

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(box), buttons);
  state->refresh = GTK_BUTTON(gtk_button_new_with_label(_("Refresh")));
  state->discard = GTK_BUTTON(gtk_button_new_with_label(_("Discard Prepared Changes")));
  state->close = GTK_BUTTON(gtk_button_new_with_label(_("Close")));
  gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(state->refresh));
  gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(state->discard));
  gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(state->close));

  g_signal_connect(state->refresh,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer data) { start_offline_view_task(GTK_WINDOW(data), false); }),
                   window);
  g_signal_connect(state->discard,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer data) { start_offline_view_task(GTK_WINDOW(data), true); }),
                   window);
  g_signal_connect(state->close,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer data) { gtk_window_close(GTK_WINDOW(data)); }),
                   window);
  g_signal_connect(window,
                   "close-request",
                   G_CALLBACK(+[](GtkWindow *, gpointer data) -> gboolean {
                     return static_cast<OfflineViewState *>(data)->discarding;
                   }),
                   state);
  g_signal_connect_object(window,
                          "destroy",
                          G_CALLBACK(+[](GtkWidget *, gpointer parent) {
                            g_object_set_data(G_OBJECT(parent), "offline-view-window", nullptr);
                          }),
                          root,
                          G_CONNECT_DEFAULT);
  g_signal_connect(
      window,
      "destroy",
      G_CALLBACK(+[](GtkWidget *, gpointer data) { static_cast<OfflineViewState *>(data)->destroyed = true; }),
      state);

  start_offline_view_task(window, false);
  gtk_window_present(window);
}
