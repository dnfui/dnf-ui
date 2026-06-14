// -----------------------------------------------------------------------------
// pending_transaction_apply.cpp
// Pending transaction preview and apply helpers
//
// Keeps service preview, service apply, and post-transaction refresh code separate from package action button handlers.
// -----------------------------------------------------------------------------
#include "pending_transaction_apply.hpp"

#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "package_query_controller.hpp"
#include "pending_transaction_controller.hpp"
#include "pending_transaction_request.hpp"
#include "pending_transaction_view.hpp"
#include "transaction_progress.hpp"
#include "transaction_review_dialog.hpp"
#include "transaction_service_client.hpp"
#include "ui_helpers.hpp"
#include "widgets.hpp"
#include "widgets_internal.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

// Data owned by the apply task.
// The worker uses transaction_path to call the service, and progress_window receives text lines while the service
// applies.
struct ApplyTaskData {
  SearchWidgets *widgets = nullptr;
  std::string transaction_path;
  TransactionProgressWindow *progress_window;
};

// Data passed to the transaction preview worker.
struct PreviewTaskData {
  TransactionRequest request;
  TransactionPreview preview;
  std::string transaction_path;
  bool transaction_path_transferred = false;
};

struct KeyImportPromptState {
  SearchWidgets *widgets = nullptr;
  TransactionKeyImportRequest request;
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
  bool accepted = false;
};

using KeyImportPromptStatePtr = std::shared_ptr<KeyImportPromptState>;

constexpr const char *kKeyImportPromptStateKey = "dnfui-key-import-prompt-state";

// -----------------------------------------------------------------------------
// Free data owned by one apply task.
// -----------------------------------------------------------------------------
static void
apply_task_data_free(gpointer p)
{
  ApplyTaskData *d = static_cast<ApplyTaskData *>(p);
  if (!d) {
    return;
  }

  if (!d->transaction_path.empty()) {
    transaction_service_client_release_request_async(d->transaction_path);
  }

  // Drop the task reference. Queued progress callbacks retain the window state
  // separately until their GTK update has run.
  transaction_progress_release(d->progress_window);
  delete d;
}

// -----------------------------------------------------------------------------
// Free data owned by one preview task.
// -----------------------------------------------------------------------------
static void
preview_task_data_free(gpointer p)
{
  PreviewTaskData *d = static_cast<PreviewTaskData *>(p);
  if (d && !d->transaction_path.empty() && !d->transaction_path_transferred) {
    transaction_service_client_release_request_async(d->transaction_path);
  }
  delete d;
}

// -----------------------------------------------------------------------------
// Return repository key identities as text for the key import prompt.
// -----------------------------------------------------------------------------
static std::string
key_import_user_ids_text(const std::vector<std::string> &user_ids)
{
  std::ostringstream out;
  for (const auto &user_id : user_ids) {
    if (user_id.empty()) {
      continue;
    }
    if (out.tellp() > 0) {
      out << "\n";
    }
    out << user_id;
  }

  return out.str();
}

// -----------------------------------------------------------------------------
// Finish the key import prompt and wake the apply worker.
// -----------------------------------------------------------------------------
static void
finish_key_import_prompt(GtkWindow *dialog, bool accepted)
{
  KeyImportPromptStatePtr state;
  if (dialog) {
    auto *stored =
        static_cast<KeyImportPromptStatePtr *>(g_object_get_data(G_OBJECT(dialog), kKeyImportPromptStateKey));
    if (stored) {
      state = *stored;
    }
  }

  if (state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->done) {
      state->accepted = accepted;
      state->done = true;
      state->condition.notify_one();
    }
  }

  if (dialog) {
    gtk_window_destroy(dialog);
  }
}

// -----------------------------------------------------------------------------
// Show the repository key import prompt on the GTK thread.
// -----------------------------------------------------------------------------
static gboolean
show_key_import_prompt_on_main(gpointer user_data)
{
  std::unique_ptr<KeyImportPromptStatePtr> holder(static_cast<KeyImportPromptStatePtr *>(user_data));
  KeyImportPromptStatePtr state = holder ? *holder : nullptr;
  if (!state || !state->widgets || state->widgets->window_state.destroyed) {
    if (state) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->done = true;
      state->accepted = false;
      state->condition.notify_one();
    }
    return G_SOURCE_REMOVE;
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
  }

  auto *dialog_state = new KeyImportPromptStatePtr(state);
  g_object_set_data_full(G_OBJECT(dialog), kKeyImportPromptStateKey, dialog_state, [](gpointer p) {
    delete static_cast<KeyImportPromptStatePtr *>(p);
  });

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *title = gtk_label_new(nullptr);
  gchar *title_markup = g_markup_printf_escaped("<b>%s</b>", _("Trust this repository signing key?"));
  gtk_label_set_markup(GTK_LABEL(title), title_markup);
  g_free(title_markup);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_box_append(GTK_BOX(outer), title);

  GtkWidget *intro = gtk_label_new(_("The repository cannot be used until its signing key is trusted."));
  gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
  gtk_box_append(GTK_BOX(outer), intro);

  GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append(GTK_BOX(outer), details);

  auto append_detail = [&](const char *label, const std::string &value) {
    if (value.empty()) {
      return;
    }

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(details), row);

    GtkWidget *heading = gtk_label_new(nullptr);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", label);
    gtk_label_set_markup(GTK_LABEL(heading), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_box_append(GTK_BOX(row), heading);

    GtkWidget *text = gtk_label_new(value.c_str());
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_widget_set_focusable(text, FALSE);
    gtk_box_append(GTK_BOX(row), text);
  };

  append_detail(_("Key ID"), state->request.key_id);
  append_detail(_("Fingerprint"), state->request.fingerprint);
  append_detail(_("Repository"), key_import_user_ids_text(state->request.user_ids));
  append_detail(_("Key URL"), state->request.key_url);

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
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
                       finish_key_import_prompt(GTK_WINDOW(root), false);
                     }
                   }),
                   nullptr);

  g_signal_connect(trust_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       finish_key_import_prompt(GTK_WINDOW(root), true);
                     }
                   }),
                   nullptr);

  g_signal_connect(dialog,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *widget, gpointer) {
                     auto *stored = static_cast<KeyImportPromptStatePtr *>(
                         g_object_get_data(G_OBJECT(widget), kKeyImportPromptStateKey));
                     KeyImportPromptStatePtr state = stored ? *stored : nullptr;
                     if (!state) {
                       return;
                     }

                     std::lock_guard<std::mutex> lock(state->mutex);
                     if (!state->done) {
                       state->accepted = false;
                       state->done = true;
                       state->condition.notify_one();
                     }
                   }),
                   nullptr);

  gtk_window_present(dialog);
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Ask the user whether the daemon may import a repository signing key.
// -----------------------------------------------------------------------------
static bool
confirm_repository_key_import(SearchWidgets *widgets, const TransactionKeyImportRequest &request)
{
  auto state = std::make_shared<KeyImportPromptState>();
  state->widgets = widgets;
  state->request = request;

  g_main_context_invoke(nullptr, show_key_import_prompt_on_main, new KeyImportPromptStatePtr(state));

  std::unique_lock<std::mutex> lock(state->mutex);
  state->condition.wait(lock, [&]() { return state->done; });
  return state->accepted;
}

// -----------------------------------------------------------------------------
// Release any prepared service preview because the pending actions changed.
// -----------------------------------------------------------------------------
void
pending_transaction_invalidate_service_preview(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  if (!widgets->transaction.preview_transaction_path.empty()) {
    // Drop the prepared service request without waiting on D-Bus from GTK.
    transaction_service_client_release_request_async(widgets->transaction.preview_transaction_path);
  }

  widgets->transaction.preview_transaction_path.clear();
  widgets->transaction.preview_upgrade_all = false;
}

// -----------------------------------------------------------------------------
// Return the status text shown while a preview request is running.
// -----------------------------------------------------------------------------
const char *
pending_transaction_preview_busy_message()
{
  return _("Wait for the current transaction preview to finish.");
}

// -----------------------------------------------------------------------------
// Return true when a preview request is running.
// -----------------------------------------------------------------------------
bool
pending_transaction_preview_is_busy(SearchWidgets *widgets)
{
  return widgets && widgets->transaction.preview_request_in_progress;
}

// -----------------------------------------------------------------------------
// Return the status text shown while an apply request is running.
// -----------------------------------------------------------------------------
const char *
pending_transaction_apply_busy_message()
{
  return _("Wait for the current transaction to finish.");
}

// -----------------------------------------------------------------------------
// Return true when an apply request is running.
// -----------------------------------------------------------------------------
bool
pending_transaction_apply_is_busy(SearchWidgets *widgets)
{
  return widgets && widgets->transaction.apply_in_progress;
}

// -----------------------------------------------------------------------------
// Enable or disable the controls that can start a new transaction preview.
// -----------------------------------------------------------------------------
void
pending_transaction_set_preview_controls_sensitive(SearchWidgets *widgets, bool sensitive)
{
  if (!widgets) {
    return;
  }

  if (widgets->transaction.pending_list) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.pending_list), sensitive);
  }
  if (widgets->transaction.upgrade_all_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.upgrade_all_button), sensitive);
  }

  if (!sensitive) {
    if (widgets->transaction.apply_button) {
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.apply_button), FALSE);
    }
    if (widgets->transaction.clear_pending_button) {
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.clear_pending_button), FALSE);
    }
    return;
  }

  pending_transaction_refresh_pending_tab(widgets);
}

// -----------------------------------------------------------------------------
// Enable or disable controls while a preview request is running.
// -----------------------------------------------------------------------------
static void
set_preview_request_busy_state(SearchWidgets *widgets, bool busy)
{
  if (!widgets) {
    return;
  }

  widgets->transaction.preview_request_in_progress = busy;
  pending_transaction_set_preview_controls_sensitive(widgets, !busy);

  if (busy) {
    ui_helpers_set_icon_button(widgets->transaction.apply_button, "object-select-symbolic", _("Preparing Preview..."));
    return;
  }
}

// -----------------------------------------------------------------------------
// Finish the post-transaction repository rebuild.
// -----------------------------------------------------------------------------
static void
rebuild_after_tx_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  GError *error = nullptr;
  gboolean ok = g_task_propagate_boolean(task, &error);

  if (!ok && error) {
    ui_helpers_set_status(widgets->query.status_label, error->message, "red");
    g_error_free(error);
    return;
  }

  // Transaction follow-up rebuilds produce a new Base generation.
  // Cached search result rows must be discarded before the next search.
  package_query_clear_search_cache();

  // Repopulate the currently visible package view so rows removed by the transaction disappear without a manual reload.
  package_query_reload_current_view(widgets);
}

// -----------------------------------------------------------------------------
// Rebuild repository data after a transaction completes.
// -----------------------------------------------------------------------------
static void
rebuild_after_tx_async(SearchWidgets *widgets)
{
  // Once the post-transaction rebuild begins, stop serving cached search results from the pre-transaction Base
  // generation.
  package_query_clear_search_cache();

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, rebuild_after_tx_finished);
  g_task_run_in_thread(task, widgets_on_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start applying the transaction after the user confirms the summary.
// The service streams progress back through the callback passed to the client.
// -----------------------------------------------------------------------------
static void
start_apply_transaction(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  if (widgets->transaction.preview_transaction_path.empty()) {
    ui_helpers_set_status(widgets->query.status_label, _("No prepared transaction request is available."), "red");
    return;
  }

  widgets->transaction.apply_in_progress = true;
  pending_transaction_set_preview_controls_sensitive(widgets, false);

  ApplyTaskData *td = new ApplyTaskData;
  td->widgets = widgets;
  // Apply now owns this dnf5daemon session path.
  // Pending action changes must not release it while the daemon is applying the transaction.
  td->transaction_path = std::move(widgets->transaction.preview_transaction_path);
  widgets->transaction.preview_transaction_path.clear();
  size_t pending_count = widgets->transaction.preview_upgrade_all ? 0 : widgets->transaction.actions.size();
  td->progress_window = transaction_progress_create_window(widgets, pending_count);
  // Keep the progress state alive while the apply worker may still receive service progress.
  // Closing the window only removes the GTK widgets.
  transaction_progress_retain(td->progress_window);

  transaction_progress_append(td->progress_window, _("Queued transaction request."));
  const char *status_message = widgets->transaction.preview_upgrade_all
      ? _("Applying package upgrades. See transaction window for details.")
      : _("Applying pending changes. See transaction window for details.");
  ui_helpers_set_status(widgets->query.status_label, status_message, "blue");
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(
      widgets, c, +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        ApplyTaskData *td = static_cast<ApplyTaskData *>(g_task_get_task_data(task));
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        if (widgets_task_should_skip_completion(task, widgets)) {
          return;
        }

        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);

        // Release this task's spinner slot.
        widgets_spinner_release(widgets->query.spinner);
        widgets->transaction.apply_in_progress = false;

        transaction_progress_finish(td ? td->progress_window : nullptr, success, "");

        if (success) {
          pending_transaction_invalidate_service_preview(widgets);
          // Clear pending actions and restore the transaction controls.
          widgets->transaction.actions.clear();
          pending_transaction_set_preview_controls_sensitive(widgets, true);

          ui_helpers_set_status(widgets->query.status_label, _("Transaction successful."), "green");

          // Rebuild repository data and refresh package state in the background.
          rebuild_after_tx_async(widgets);
        } else {
          pending_transaction_invalidate_service_preview(widgets);
          pending_transaction_set_preview_controls_sensitive(widgets, true);
          std::string details = error ? error->message : _("Transaction failed.");
          ui_helpers_set_status(widgets->query.status_label, details.c_str(), "red");
          // Show the full backend error in a copyable dialog instead of only in the status bar.
          transaction_review_show_error_dialog(widgets,
                                               _("Transaction Failed"),
                                               _("The transaction could not be completed. Review the details below."),
                                               details);
          if (error) {
            g_error_free(error);
          }
        }
      });

  g_task_set_task_data(task, td, apply_task_data_free);

  g_task_run_in_thread(
      task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *) {
        ApplyTaskData *td = static_cast<ApplyTaskData *>(task_data);
        std::string err;
        // This callback runs on the apply worker thread when the client receives a service Progress signal.
        // transaction_progress_append queues the actual GTK update onto the main thread.
        bool ok = transaction_service_client_apply_started_request(
            td->transaction_path,
            [td](const std::string &message) { transaction_progress_append(td->progress_window, message); },
            [td](const TransactionKeyImportRequest &request) {
              return confirm_repository_key_import(td->widgets, request);
            },
            err);
        if (ok) {
          g_task_return_boolean(t, TRUE);
        } else {
          g_task_return_error(t, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, err.c_str()));
        }
      });

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Prepare a service-backed transaction preview and show the confirmation dialog.
// -----------------------------------------------------------------------------
static void
start_preview_request(SearchWidgets *widgets, TransactionRequest request)
{
  pending_transaction_invalidate_service_preview(widgets);
  widgets->transaction.preview_upgrade_all = request.upgrade_all;
  DNFUI_TRACE("Transaction preview request start upgrade_all=%d install=%zu remove=%zu reinstall=%zu",
              request.upgrade_all ? 1 : 0,
              request.install.size(),
              request.remove.size(),
              request.reinstall.size());
  ui_helpers_set_status(widgets->query.status_label, _("Preparing transaction preview..."), "blue");
  widgets_spinner_acquire(widgets->query.spinner);
  set_preview_request_busy_state(widgets, true);

  PreviewTaskData *td = new PreviewTaskData();
  td->request = std::move(request);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(
      widgets, c, +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        PreviewTaskData *td = static_cast<PreviewTaskData *>(g_task_get_task_data(task));

        if (widgets_task_should_skip_completion(task, widgets)) {
          return;
        }

        widgets_spinner_release(widgets->query.spinner);
        set_preview_request_busy_state(widgets, false);

        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);
        if (!success || !td) {
          const char *status_message =
              error && error->message ? error->message : _("Unable to prepare transaction preview.");
          DNFUI_TRACE("Transaction preview request failed error=%s", status_message);
          widgets->transaction.preview_upgrade_all = false;
          ui_helpers_set_status(widgets->query.status_label, status_message, "red");
          transaction_review_show_error_dialog(widgets,
                                               _("Transaction Preview Failed"),
                                               _("The transaction could not be prepared. Review the details below."),
                                               status_message);
          if (error) {
            g_error_free(error);
          }
          return;
        }

        if (td->preview.empty()) {
          DNFUI_TRACE("Transaction preview request empty upgrade_all=%d install=%zu remove=%zu reinstall=%zu path=%s",
                      td->request.upgrade_all ? 1 : 0,
                      td->request.install.size(),
                      td->request.remove.size(),
                      td->request.reinstall.size(),
                      td->transaction_path.c_str());
          if (!td->transaction_path.empty()) {
            transaction_service_client_release_request_async(td->transaction_path);
            td->transaction_path.clear();
          }
          widgets->transaction.preview_transaction_path.clear();
          widgets->transaction.preview_upgrade_all = false;
          ui_helpers_set_status(widgets->query.status_label, _("All packages are already up to date."), "green");
          return;
        }

        widgets->transaction.preview_transaction_path = td->transaction_path;
        widgets->transaction.preview_upgrade_all = td->request.upgrade_all;
        td->transaction_path_transferred = true;
        DNFUI_TRACE("Transaction preview request ready upgrade_all=%d path=%s",
                    td->request.upgrade_all ? 1 : 0,
                    widgets->transaction.preview_transaction_path.c_str());
        transaction_review_show_summary_dialog(
            widgets, td->preview, start_apply_transaction, pending_transaction_invalidate_service_preview);
      });

  g_task_set_task_data(task, td, preview_task_data_free);
  g_task_run_in_thread(
      task, +[](GTask *task, gpointer, gpointer task_data, GCancellable *) {
        PreviewTaskData *td = static_cast<PreviewTaskData *>(task_data);
        std::string error;
        bool ok = false;
        if (td && td->request.upgrade_all) {
          DNFUI_TRACE("Transaction preview worker start upgrade_all=1");
          ok = transaction_service_client_preview_upgrade_all_request(td->preview, td->transaction_path, error);
        } else if (td) {
          DNFUI_TRACE("Transaction preview worker start upgrade_all=0 install=%zu remove=%zu reinstall=%zu",
                      td->request.install.size(),
                      td->request.remove.size(),
                      td->request.reinstall.size());
          ok = transaction_service_client_preview_request(td->request, td->preview, td->transaction_path, error);
        }

        if (!ok) {
          DNFUI_TRACE("Transaction preview worker failed error=%s", error.c_str());
          g_task_return_new_error(task,
                                  G_IO_ERROR,
                                  G_IO_ERROR_FAILED,
                                  "%s",
                                  error.empty() ? _("Unable to prepare transaction preview.") : error.c_str());
          return;
        }

        DNFUI_TRACE("Transaction preview worker done path=%s empty=%d",
                    td ? td->transaction_path.c_str() : "",
                    td && td->preview.empty() ? 1 : 0);
        g_task_return_boolean(task, TRUE);
      });

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Prepare a transaction preview for all package upgrades.
// -----------------------------------------------------------------------------
void
pending_transaction_on_upgrade_all_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  if (pending_transaction_apply_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_apply_busy_message(), "blue");
    return;
  }

  if (!widgets->transaction.actions.empty()) {
    ui_helpers_set_status(
        widgets->query.status_label, _("Clear pending package actions before upgrading all packages."), "blue");
    return;
  }

  TransactionRequest request;
  request.upgrade_all = true;
  std::string error;
  if (!request.validate(error)) {
    ui_helpers_set_status(widgets->query.status_label, error.c_str(), "red");
    return;
  }

  start_preview_request(widgets, std::move(request));
}

// -----------------------------------------------------------------------------
// Prepare a transaction preview and ask the user to confirm it.
// -----------------------------------------------------------------------------
void
pending_transaction_on_apply_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  if (pending_transaction_apply_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_apply_busy_message(), "blue");
    return;
  }

  if (widgets->transaction.actions.empty()) {
    ui_helpers_set_status(widgets->query.status_label, _("No pending changes."), "gray");
    return;
  }

  TransactionRequest request;
  std::string error;
  if (!pending_transaction_build_request(widgets->transaction.actions, request, error)) {
    ui_helpers_set_status(widgets->query.status_label, error.c_str(), "red");
    return;
  }

  // Refuse self-protected transactions before asking the service to preview them.
  if (!pending_transaction_validate_request(request, error)) {
    ui_helpers_set_status(widgets->query.status_label, error.c_str(), "red");
    return;
  }

  start_preview_request(widgets, std::move(request));
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
