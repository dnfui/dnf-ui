// -----------------------------------------------------------------------------
// transaction_key_dialog.cpp
// Repository signing-key confirmation and cancellation
// -----------------------------------------------------------------------------
#include "ui/transaction/transaction_key_dialog.hpp"

#include "dnf5daemon_client/transaction_service_client.hpp"
#include "i18n.hpp"
#include "ui/common/widgets.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

struct KeyImportDialogState {
  MainWindowUiState *widgets = nullptr;
  TransactionKeyImportRequest request;
  std::mutex mutex;
  std::condition_variable condition;
  // Set on the GTK thread after creation. Cancellation uses it to close the dialog from the main thread.
  GtkWindow *dialog = nullptr;
  bool done = false;
  bool accepted = false;
};

using KeyImportDialogStatePtr = std::shared_ptr<KeyImportDialogState>;

constexpr const char *kKeyImportDialogStateKey = "dnfui-key-import-dialog-state";

// -----------------------------------------------------------------------------
// Return repository key identities as text for the key import dialog.
// -----------------------------------------------------------------------------
static std::string
key_import_user_ids_text(const std::vector<std::string> &user_ids)
{
  std::ostringstream out;
  bool first = true;
  for (const auto &user_id : user_ids) {
    if (user_id.empty()) {
      continue;
    }
    if (!first) {
      out << "\n";
    }
    out << user_id;
    first = false;
  }

  return out.str();
}

// -----------------------------------------------------------------------------
// Return the key details shown before the user accepts or rejects a repository key.
// -----------------------------------------------------------------------------
std::string
transaction_key_dialog_details_text(const TransactionKeyImportRequest &request)
{
  std::ostringstream out;
  out << _("The repository cannot be used until its signing key is trusted.") << "\n\n";

  if (!request.key_id.empty()) {
    out << _("Key ID") << ": " << request.key_id << "\n";
  }
  if (!request.fingerprint.empty()) {
    out << _("Fingerprint") << ": " << request.fingerprint << "\n";
  }

  std::string user_ids = key_import_user_ids_text(request.user_ids);
  if (!user_ids.empty()) {
    out << _("Repository") << ": " << user_ids << "\n";
  }
  if (!request.key_url.empty()) {
    out << _("Key URL") << ": " << request.key_url << "\n";
  }

  return out.str();
}

// -----------------------------------------------------------------------------
// Store the final key answer and wake the waiting worker.
// -----------------------------------------------------------------------------
static bool
complete_key_import_state(const KeyImportDialogStatePtr &state, bool accepted)
{
  if (!state) {
    return false;
  }

  std::unique_lock<std::mutex> lock(state->mutex);
  if (state->done) {
    return false;
  }

  state->accepted = accepted;
  state->done = true;
  lock.unlock();
  state->condition.notify_one();
  return true;
}

// -----------------------------------------------------------------------------
// Finish the visible key import dialog after a user choice.
// -----------------------------------------------------------------------------
static void
finish_key_import_dialog(GtkWindow *dialog, bool accepted)
{
  KeyImportDialogStatePtr state;
  if (dialog) {
    auto *stored =
        static_cast<KeyImportDialogStatePtr *>(g_object_get_data(G_OBJECT(dialog), kKeyImportDialogStateKey));
    if (stored) {
      state = *stored;
    }
  }

  complete_key_import_state(state, accepted);

  if (dialog) {
    gtk_window_destroy(dialog);
  }
}

// -----------------------------------------------------------------------------
// Close a key import dialog from the GTK thread after its task was cancelled.
// -----------------------------------------------------------------------------
static gboolean
destroy_key_import_dialog_on_main(gpointer user_data)
{
  std::unique_ptr<KeyImportDialogStatePtr> holder(static_cast<KeyImportDialogStatePtr *>(user_data));
  KeyImportDialogStatePtr state = holder ? *holder : nullptr;
  GtkWindow *dialog = nullptr;

  if (state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    dialog = state->dialog;
  }

  if (dialog) {
    gtk_window_destroy(dialog);
  }

  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Reject a pending key request when the preview or apply task is cancelled.
// The cancellation callback may run on a worker thread, so GTK cleanup is queued.
// -----------------------------------------------------------------------------
static void
key_import_dialog_cancelled(GCancellable *, gpointer user_data)
{
  auto *state_ptr = static_cast<KeyImportDialogStatePtr *>(user_data);
  KeyImportDialogStatePtr state = state_ptr ? *state_ptr : nullptr;
  complete_key_import_state(state, false);
  g_main_context_invoke(nullptr, destroy_key_import_dialog_on_main, new KeyImportDialogStatePtr(state));
}

// -----------------------------------------------------------------------------
// Show the repository key import dialog on the GTK thread.
// -----------------------------------------------------------------------------
static gboolean
show_key_import_dialog_on_main(gpointer user_data)
{
  std::unique_ptr<KeyImportDialogStatePtr> holder(static_cast<KeyImportDialogStatePtr *>(user_data));
  KeyImportDialogStatePtr state = holder ? *holder : nullptr;
  if (!state || !state->widgets || state->widgets->window_state.destroyed) {
    complete_key_import_state(state, false);
    return G_SOURCE_REMOVE;
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->done) {
      return G_SOURCE_REMOVE;
    }
  }

  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dialog, _("Repository Signing Key"));
  gtk_window_set_default_size(dialog, 620, 360);
  gtk_window_set_modal(dialog, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(state->widgets->query.entry));
  if (root && GTK_IS_WINDOW(root)) {
    GtkWindow *parent = GTK_WINDOW(root);
    if (GtkApplication *app = gtk_window_get_application(parent)) {
      gtk_window_set_application(dialog, app);
    }
    gtk_window_set_transient_for(dialog, parent);
    gtk_window_set_destroy_with_parent(dialog, TRUE);
  }

  auto *dialog_state = new KeyImportDialogStatePtr(state);
  g_object_set_data_full(G_OBJECT(dialog), kKeyImportDialogStateKey, dialog_state, [](gpointer p) {
    delete static_cast<KeyImportDialogStatePtr *>(p);
  });

  bool destroy_dialog = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->done) {
      destroy_dialog = true;
    } else {
      state->dialog = dialog;
    }
  }

  if (destroy_dialog) {
    gtk_window_destroy(dialog);
    return G_SOURCE_REMOVE;
  }

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *heading = gtk_label_new(nullptr);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", _("Trust this repository signing key?"));
  gtk_label_set_markup(GTK_LABEL(heading), markup);
  g_free(markup);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_box_append(GTK_BOX(outer), heading);

  GtkWidget *message = gtk_label_new(transaction_key_dialog_details_text(state->request).c_str());
  gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_label_set_selectable(GTK_LABEL(message), TRUE);
  gtk_label_select_region(GTK_LABEL(message), 0, 0);
  gtk_widget_set_vexpand(message, TRUE);
  gtk_widget_set_valign(message, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(outer), message);

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_widget_set_valign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  GtkWidget *reject_button = gtk_button_new_with_label(_("Reject"));
  gtk_box_append(GTK_BOX(button_box), reject_button);

  GtkWidget *trust_button = gtk_button_new_with_label(_("Trust Key"));
  gtk_widget_add_css_class(trust_button, "suggested-action");
  gtk_box_append(GTK_BOX(button_box), trust_button);

  g_signal_connect(reject_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       finish_key_import_dialog(GTK_WINDOW(root), false);
                     }
                   }),
                   nullptr);

  g_signal_connect(trust_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       finish_key_import_dialog(GTK_WINDOW(root), true);
                     }
                   }),
                   nullptr);

  g_signal_connect(dialog,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *widget, gpointer) {
                     auto *stored = static_cast<KeyImportDialogStatePtr *>(
                         g_object_get_data(G_OBJECT(widget), kKeyImportDialogStateKey));
                     KeyImportDialogStatePtr state = stored ? *stored : nullptr;
                     if (!state) {
                       return;
                     }

                     {
                       std::lock_guard<std::mutex> lock(state->mutex);
                       if (state->dialog == GTK_WINDOW(widget)) {
                         state->dialog = nullptr;
                       }
                     }
                     complete_key_import_state(state, false);
                   }),
                   nullptr);

  gtk_widget_grab_focus(reject_button);
  gtk_window_present(dialog);
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Ask for repository key approval when dnf5daemon requests it.
// The daemon signal is handled on the preview or apply worker thread.
// Show the GTK dialog on the main thread and wait for the user answer.
// -----------------------------------------------------------------------------
bool
transaction_dialogs_confirm_key_import(MainWindowUiState *widgets,
                                       const TransactionKeyImportRequest &request,
                                       GCancellable *cancellable)
{
  if (!widgets || (cancellable && g_cancellable_is_cancelled(cancellable))) {
    return false;
  }

  auto state = std::make_shared<KeyImportDialogState>();
  state->widgets = widgets;
  state->request = request;

  auto *cancel_data = new KeyImportDialogStatePtr(state);
  gulong cancel_handler_id = 0;
  if (cancellable) {
    // GLib frees cancel_data when this handler is disconnected, or immediately if the task was already cancelled.
    cancel_handler_id =
        g_cancellable_connect(cancellable, G_CALLBACK(key_import_dialog_cancelled), cancel_data, [](gpointer p) {
          delete static_cast<KeyImportDialogStatePtr *>(p);
        });
  } else {
    delete cancel_data;
  }

  g_main_context_invoke(nullptr, show_key_import_dialog_on_main, new KeyImportDialogStatePtr(state));

  std::unique_lock<std::mutex> lock(state->mutex);
  state->condition.wait(lock, [&]() { return state->done; });
  bool accepted = state->accepted;
  lock.unlock();

  if (cancellable && cancel_handler_id > 0) {
    g_cancellable_disconnect(cancellable, cancel_handler_id);
  }

  return accepted;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
