// -----------------------------------------------------------------------------
// package_query_upgrade_indicator.cpp
// Quiet dnf5daemon upgrade count refresh for the List Upgradable button
//
// Owns the GTask worker and GTK-thread completion handler for the count check.
// Visible List Upgradable package queries stay in package_query_tasks.cpp.
// -----------------------------------------------------------------------------
#include "ui/package_query/package_query_controller_internal.hpp"

#include "debug_trace.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"
#include "i18n.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"
#include "ui/refresh/repository_refresh_controller.hpp"
#include "ui/transaction/pending_transaction_apply.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct UpgradeIndicatorRefreshResult {
  DaemonUpgradeRefreshOwner refresh_owner;
  std::vector<DaemonUpgradeTarget> targets;
  bool package_state_changed = false;
};

struct UpgradeIndicatorRefreshTaskData {
  DaemonUpgradeRefreshOwner refresh_owner;
};

static void
on_upgrade_indicator_refresh_task(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void on_upgrade_indicator_refresh_finished(GObject *source_object, GAsyncResult *result, gpointer user_data);

// -----------------------------------------------------------------------------
// Start one quiet refresh of the List Upgradable button count.
// -----------------------------------------------------------------------------
void
package_query_start_upgrade_indicator_refresh(MainWindowUiState *widgets)
{
  if (!widgets || widgets->window_state.destroyed) {
    return;
  }

  widgets->query_state.upgrade_indicator_refresh_pending = true;
  package_query_maybe_start_upgrade_indicator_refresh(widgets);
}

// -----------------------------------------------------------------------------
// Start a pending quiet upgrade count refresh when no conflicting work is active.
// -----------------------------------------------------------------------------
void
package_query_maybe_start_upgrade_indicator_refresh(MainWindowUiState *widgets)
{
  if (!widgets || widgets->window_state.destroyed || !widgets->query_state.upgrade_indicator_refresh_pending) {
    return;
  }

  if (package_query_has_active_package_list_request(widgets) || repository_refresh_is_running() ||
      pending_transaction_preview_is_busy(widgets) || pending_transaction_apply_is_busy(widgets)) {
    DNFUI_TRACE("Upgrade indicator refresh delayed because user work is active");
    package_query_refresh_upgrade_indicator(widgets);
    return;
  }

  DaemonUpgradeSnapshot snapshot = DaemonUpgradeState::instance().snapshot();
  if (snapshot.status == DaemonUpgradeSnapshotStatus::READY) {
    widgets->query_state.upgrade_indicator_refresh_pending = false;
    package_query_refresh_upgrade_indicator(widgets);
    return;
  }
  if (snapshot.status == DaemonUpgradeSnapshotStatus::REFRESHING) {
    package_query_refresh_upgrade_indicator(widgets);
    return;
  }

  std::optional<DaemonUpgradeRefreshId> refresh_id = DaemonUpgradeState::instance().begin_refresh();
  if (!refresh_id.has_value()) {
    package_query_refresh_upgrade_indicator(widgets);
    return;
  }
  widgets->query_state.upgrade_indicator_refresh_pending = false;

  DNFUI_TRACE("Upgrade indicator refresh task start");
  package_query_refresh_upgrade_indicator(widgets);
  auto *task_data = new UpgradeIndicatorRefreshTaskData {
    DaemonUpgradeRefreshOwner(refresh_id.value()),
  };
  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets, c, on_upgrade_indicator_refresh_finished);
  g_task_set_task_data(task, task_data, [](gpointer p) { delete static_cast<UpgradeIndicatorRefreshTaskData *>(p); });
  g_task_run_in_thread(task, on_upgrade_indicator_refresh_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Load daemon upgrade targets for the button count without loading package metadata.
// -----------------------------------------------------------------------------
static void
on_upgrade_indicator_refresh_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  auto *td = static_cast<UpgradeIndicatorRefreshTaskData *>(task_data);
  if (!td) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", _("Unable to start upgrade count check."));
    return;
  }
  DaemonUpgradeRefreshOwner &refresh_owner = td->refresh_owner;

  try {
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      DaemonUpgradeState::instance().abandon_refresh(refresh_owner.id());
      refresh_owner.close();
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("Upgrade count check was cancelled."));
      return;
    }

    if (dnf_backend_refresh_installed_snapshot_preserving_cached_base()) {
      DaemonUpgradeState::instance().abandon_refresh(refresh_owner.id());
      refresh_owner.close();
      auto *result = new UpgradeIndicatorRefreshResult;
      result->package_state_changed = true;
      g_task_return_pointer(task, result, [](gpointer p) { delete static_cast<UpgradeIndicatorRefreshResult *>(p); });
      return;
    }

    std::vector<DaemonUpgradeTarget> targets;
    std::string error;
    if (!transaction_service_client_list_upgrade_targets(targets, error, cancellable)) {
      if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        DaemonUpgradeState::instance().abandon_refresh(refresh_owner.id());
        refresh_owner.close();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("Upgrade count check was cancelled."));
        return;
      }

      DaemonUpgradeState::instance().publish_failure(refresh_owner.id());
      refresh_owner.close();
      throw std::runtime_error(error.empty() ? _("Unable to check for package upgrades from dnf5daemon.") : error);
    }

    if (dnf_backend_refresh_installed_snapshot_preserving_cached_base()) {
      DaemonUpgradeState::instance().abandon_refresh(refresh_owner.id());
      refresh_owner.close();
      auto *result = new UpgradeIndicatorRefreshResult;
      result->package_state_changed = true;
      g_task_return_pointer(task, result, [](gpointer p) { delete static_cast<UpgradeIndicatorRefreshResult *>(p); });
      return;
    }

    auto *result = new UpgradeIndicatorRefreshResult;
    result->refresh_owner = std::move(refresh_owner);
    result->targets = std::move(targets);
    g_task_return_pointer(task, result, [](gpointer p) { delete static_cast<UpgradeIndicatorRefreshResult *>(p); });
  } catch (const std::exception &e) {
    if (refresh_owner.id() != 0 && cancellable && g_cancellable_is_cancelled(cancellable)) {
      DaemonUpgradeState::instance().abandon_refresh(refresh_owner.id());
      refresh_owner.close();
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("Upgrade count check was cancelled."));
      return;
    }
    DaemonUpgradeState::instance().abandon_refresh(refresh_owner.id());
    refresh_owner.close();
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish the quiet upgrade count refresh on the GTK thread.
// -----------------------------------------------------------------------------
static void
on_upgrade_indicator_refresh_finished(GObject *, GAsyncResult *result, gpointer user_data)
{
  GTask *task = G_TASK(result);
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  GError *error = nullptr;
  std::unique_ptr<UpgradeIndicatorRefreshResult> check_result(
      static_cast<UpgradeIndicatorRefreshResult *>(g_task_propagate_pointer(task, &error)));

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      DNFUI_TRACE("Upgrade indicator refresh task failed: %s", error->message);
    }
    g_clear_error(&error);
    package_query_refresh_upgrade_indicator(widgets);
    return;
  }

  if (!check_result) {
    package_query_refresh_upgrade_indicator(widgets);
    return;
  }

  if (check_result->package_state_changed) {
    widgets->query_state.upgrade_indicator_refresh_pending = true;
    package_query_refresh_upgrade_indicator(widgets);
    package_query_maybe_start_upgrade_indicator_refresh(widgets);
    return;
  }

  std::string publish_error;
  DaemonUpgradePublishResult publish_result = DaemonUpgradeState::instance().publish_success(
      check_result->refresh_owner.id(), check_result->targets, publish_error);
  check_result->refresh_owner.close();

  if (publish_result == DaemonUpgradePublishResult::PUBLISHED) {
    widgets->query_state.upgrade_indicator_refresh_pending = false;
  } else if (!publish_error.empty()) {
    DNFUI_TRACE("Upgrade indicator refresh publish failed: %s", publish_error.c_str());
  }

  package_query_refresh_upgrade_indicator(widgets);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
