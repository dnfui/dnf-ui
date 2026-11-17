// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_resolved_preview.cpp
// Update display-only package markings from a daemon transaction preview.
// -----------------------------------------------------------------------------
#include "ui/transaction/pending_transaction_resolved_preview.hpp"

#include "debug_trace.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"
#include "i18n.hpp"
#include "transaction/transaction_preview.hpp"
#include "transaction/transaction_request.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/transaction/pending_transaction_request.hpp"
#include "ui/transaction/pending_transaction_resolved_actions.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ResolvedPreviewTaskData {
  uint64_t revision = 0;
  std::vector<PendingAction> actions;
  TransactionPreview preview;
  std::string transaction_path;
};

// -----------------------------------------------------------------------------
// Release the read-only daemon session even when the task was cancelled.
// -----------------------------------------------------------------------------
void
resolved_preview_task_data_free(gpointer data)
{
  auto *task_data = static_cast<ResolvedPreviewTaskData *>(data);
  if (!task_data->transaction_path.empty()) {
    transaction_service_client_release_request_async(task_data->transaction_path);
  }
  delete task_data;
}

} // namespace

// -----------------------------------------------------------------------------
// Discard a stale result before it can be shown in the package table.
// -----------------------------------------------------------------------------
void
pending_transaction_clear_resolved_preview(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  auto &state = widgets->transaction_state;
  ++state.resolved_actions_revision;
  state.resolved_actions_pending = false;
  state.resolved_actions_by_nevra.clear();
  state.resolved_actions_by_name_arch.clear();
  if (state.resolved_actions_cancellable) {
    g_cancellable_cancel(state.resolved_actions_cancellable.get());
    state.resolved_actions_cancellable.reset();
  }
}

// -----------------------------------------------------------------------------
// Start at most one daemon resolve at a time, using the latest pending marks.
// -----------------------------------------------------------------------------
void
pending_transaction_update_resolved_preview(MainWindowUiState *widgets)
{
  if (!widgets || widgets->window_state.destroyed) {
    return;
  }

  auto &state = widgets->transaction_state;
  state.resolved_actions_pending = !state.actions.empty();
  if (!state.resolved_actions_pending || state.resolved_actions_in_progress) {
    return;
  }

  state.resolved_actions_pending = false;

  auto *task_data = new ResolvedPreviewTaskData;
  task_data->revision = state.resolved_actions_revision;
  task_data->actions = state.actions;
  state.resolved_actions_in_progress = true;

  GCancellable *cancellable = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  state.resolved_actions_cancellable =
      std::shared_ptr<GCancellable>(cancellable, [](GCancellable *value) { g_object_unref(value); });

  GTask *task = widgets_task_new_for_main_window_ui_state(
      widgets, cancellable, +[](GObject *, GAsyncResult *result, gpointer user_data) {
        GTask *task = G_TASK(result);
        auto *widgets = static_cast<MainWindowUiState *>(user_data);
        auto *task_data = static_cast<ResolvedPreviewTaskData *>(g_task_get_task_data(task));
        if (!widgets || widgets->window_state.destroyed) {
          return;
        }

        auto &state = widgets->transaction_state;
        state.resolved_actions_in_progress = false;
        if (task_data->revision != state.resolved_actions_revision) {
          if (state.resolved_actions_pending) {
            pending_transaction_update_resolved_preview(widgets);
          }
          return;
        }

        state.resolved_actions_cancellable.reset();
        if (widgets_task_should_skip_completion(task, widgets)) {
          return;
        }

        GError *error = nullptr;
        if (!g_task_propagate_boolean(task, &error)) {
          DNFUI_TRACE("Resolved package markings failed: %s", error ? error->message : "Unknown error");
          ui_helpers_set_status(widgets->query.status_label,
                                _("Could not resolve additional package actions. Apply will show the final preview."),
                                "yellow");
          g_clear_error(&error);
          return;
        }

        PendingTransactionResolvedActions resolved =
            pending_transaction_resolved_actions_from_preview(task_data->preview);
        state.resolved_actions_by_nevra = std::move(resolved.by_nevra);
        state.resolved_actions_by_name_arch = std::move(resolved.by_name_arch);
        if (!state.resolved_actions_by_nevra.empty() || !state.resolved_actions_by_name_arch.empty()) {
          package_table_refresh_additional_statuses(widgets);
        }
      });
  g_task_set_task_data(task, task_data, resolved_preview_task_data_free);
  g_task_run_in_thread(
      task, +[](GTask *task, gpointer, gpointer data, GCancellable *cancellable) {
        auto *task_data = static_cast<ResolvedPreviewTaskData *>(data);
        std::string error;
        TransactionRequest request;
        bool ok = pending_transaction_build_request(task_data->actions, request, error);
        if (ok) {
          // Apply performs the installed-package safety checks on its own preview.
          ok = transaction_service_client_preview_request(request,
                                                          task_data->preview,
                                                          task_data->transaction_path,
                                                          error,
                                                          {},
                                                          cancellable,
                                                          true);
        }
        if (ok) {
          g_task_return_boolean(task, TRUE);
        } else {
          g_task_return_new_error(task,
                                  G_IO_ERROR,
                                  G_IO_ERROR_FAILED,
                                  "%s",
                                  error.empty() ? "Could not resolve package actions." : error.c_str());
        }
      });
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
