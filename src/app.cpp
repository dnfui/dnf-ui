// -----------------------------------------------------------------------------
// src/app.cpp
// GTK application setup
// Creates the GTK application, window, periodic refresh task, and backend warm
// up task used during startup.
// -----------------------------------------------------------------------------
#include "app.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "ui/main_window.hpp"
#include "ui/ui_helpers.hpp"
#include "ui/widgets.hpp"
#include "ui/widgets_internal.hpp"

#include <atomic>
#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Function forward declarations
// -----------------------------------------------------------------------------
static void activate(GtkApplication *app, gpointer user_data);
static void setup_periodic_tasks(void);
static gboolean on_periodic_installed_refresh_tick(gpointer user_data);
static void start_installed_refresh_task(void);
static void
on_installed_refresh_task(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void on_installed_refresh_task_finished(GObject *source_object, GAsyncResult *result, gpointer user_data);
static void startup_warmup_data_free(gpointer data);
static gboolean start_backend_warmup_idle(gpointer user_data);
static void start_backend_warmup_task(SearchWidgets *widgets);
static void on_backend_warmup_task(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void on_backend_warmup_task_finished(GObject *source_object, GAsyncResult *result, gpointer user_data);
#ifdef DNFUI_DEBUG_TRACE
static const char *base_repo_state_trace_name(BaseRepoState state);
#endif

static std::atomic<bool> g_installed_refresh_running { false };

struct StartupWarmupData {
  SearchWidgets *widgets = nullptr;
  GCancellable *startup_cancellable = nullptr;
};

struct AppBackendBaseDropGuard {
  ~AppBackendBaseDropGuard()
  {
    BaseManager::instance().drop_cached_base();
  }
};

// -----------------------------------------------------------------------------
// Return the text used in trace logs for one repository state.
// -----------------------------------------------------------------------------
#ifdef DNFUI_DEBUG_TRACE
static const char *
base_repo_state_trace_name(BaseRepoState state)
{
  switch (state) {
  case BaseRepoState::LIVE_METADATA:
    return "live-metadata";
  case BaseRepoState::CACHED_METADATA:
    return "cached-metadata";
  case BaseRepoState::INSTALLED_ONLY:
    return "installed-only";
  default:
    return "unknown";
  }
}
#endif

// -----------------------------------------------------------------------------
// Run GTK application and return process exit status
// -----------------------------------------------------------------------------
int
app_run_dnfui(int argc, char **argv)
{
  dnfui_i18n_init();

  GtkApplication *app = gtk_application_new("com.fedora.dnfui", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  return status;
}

// -----------------------------------------------------------------------------
// Setup periodic background tasks
// -----------------------------------------------------------------------------
static void
setup_periodic_tasks(void)
{
  // --- Periodic refresh of installed package names every 5 minutes ---
  g_timeout_add_seconds(300, on_periodic_installed_refresh_tick, nullptr);
}

// -----------------------------------------------------------------------------
// Start one periodic installed-package refresh tick.
// -----------------------------------------------------------------------------
static gboolean
on_periodic_installed_refresh_tick(gpointer)
{
  start_installed_refresh_task();
  return TRUE;
}

// -----------------------------------------------------------------------------
// Refresh the installed-package snapshot in the background so the periodic
// refresh does not block the GTK main thread.
// -----------------------------------------------------------------------------
static void
start_installed_refresh_task(void)
{
  if (g_installed_refresh_running.exchange(true)) {
    DNFUI_TRACE("Installed package refresh skipped because the previous refresh is still running");
    return;
  }

  DNFUI_TRACE("Installed package refresh task start");

  GTask *task = g_task_new(nullptr, nullptr, on_installed_refresh_task_finished, nullptr);
  g_task_run_in_thread(task, on_installed_refresh_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Refresh installed-package state on a worker thread.
// -----------------------------------------------------------------------------
static void
on_installed_refresh_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  AppBackendBaseDropGuard base_drop_guard;

  try {
    dnf_backend_refresh_installed_nevras();
    g_task_return_boolean(task, TRUE);
  } catch (const std::exception &e) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", e.what());
  } catch (...) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", _("Installed package refresh failed."));
  }
}

// -----------------------------------------------------------------------------
// Finish one installed-package refresh tick on the GTK thread.
// -----------------------------------------------------------------------------
static void
on_installed_refresh_task_finished(GObject *, GAsyncResult *result, gpointer)
{
  GError *error = nullptr;
  g_task_propagate_boolean(G_TASK(result), &error);

  if (error) {
    DNFUI_TRACE("Installed package refresh task failed: %s", error->message);
  } else {
    DNFUI_TRACE("Installed package refresh task done");
  }

  g_clear_error(&error);
  g_installed_refresh_running = false;
}

// -----------------------------------------------------------------------------
// Start backend warm up after the first window show is out of the way.
// -----------------------------------------------------------------------------
static void
startup_warmup_data_free(gpointer data)
{
  StartupWarmupData *warmup = static_cast<StartupWarmupData *>(data);
  if (!warmup) {
    return;
  }
  if (warmup->startup_cancellable) {
    g_object_unref(warmup->startup_cancellable);
  }
  delete warmup;
}

// -----------------------------------------------------------------------------
// Start backend warm up from the GTK idle queue.
// -----------------------------------------------------------------------------
static gboolean
start_backend_warmup_idle(gpointer user_data)
{
  StartupWarmupData *warmup = static_cast<StartupWarmupData *>(user_data);
  if (!warmup || !warmup->widgets || !warmup->startup_cancellable ||
      g_cancellable_is_cancelled(warmup->startup_cancellable)) {
    return G_SOURCE_REMOVE;
  }
  start_backend_warmup_task(warmup->widgets);

  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Start a quiet background task that warms up the shared DNF base.
// -----------------------------------------------------------------------------
static void
start_backend_warmup_task(SearchWidgets *widgets)
{
  if (!widgets || !widgets->window_state.backend_warmup_label) {
    return;
  }

  DNFUI_TRACE("Backend warm up task start");
  gtk_widget_set_visible(GTK_WIDGET(widgets->window_state.backend_warmup_label), TRUE);

  widgets->window_state.backend_warmup_cancellable = g_cancellable_new();

  GTask *task = widgets_task_new_for_search_widgets(
      widgets, widgets->window_state.backend_warmup_cancellable, on_backend_warmup_task_finished);
  g_task_run_in_thread(task, on_backend_warmup_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Warm up BaseManager in the background so the first package query is faster.
// -----------------------------------------------------------------------------
static void
on_backend_warmup_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("Backend warm up was cancelled."));
    return;
  }

  try {
    BaseManager::instance().acquire_read();
    if (g_cancellable_is_cancelled(cancellable)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("Backend warm up was cancelled."));
      return;
    }
    BaseRepoState repo_state = BaseManager::instance().current_repo_state();
    g_task_return_pointer(
        task, new BaseRepoState(repo_state), [](gpointer p) { delete static_cast<BaseRepoState *>(p); });
  } catch (const std::exception &e) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", e.what());
  } catch (...) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", _("Backend warm up failed."));
  }
}

// -----------------------------------------------------------------------------
// Ignore warm up errors so startup stays quiet and normal queries handle them.
// -----------------------------------------------------------------------------
static void
on_backend_warmup_task_finished(GObject *, GAsyncResult *result, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GTask *task = G_TASK(result);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  GError *error = nullptr;
  BaseRepoState *repo_state = static_cast<BaseRepoState *>(g_task_propagate_pointer(task, &error));

  if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    DNFUI_TRACE("Backend warm up task cancelled");
    g_clear_error(&error);
    return;
  }

  if (error) {
    DNFUI_TRACE("Backend warm up task failed: %s", error->message);
  } else {
    DNFUI_TRACE("Backend warm up task done: %s", base_repo_state_trace_name(*repo_state));
    if (*repo_state == BaseRepoState::LIVE_METADATA) {
      ui_helpers_set_status(widgets->query.status_label, _("Ready. Live repository metadata loaded."), "gray");
    } else if (*repo_state == BaseRepoState::CACHED_METADATA) {
      ui_helpers_set_status(widgets->query.status_label, _("Ready. Using cached repository metadata."), "blue");
    } else {
      ui_helpers_set_status(widgets->query.status_label, _("Ready. Showing installed packages only."), "blue");
    }
  }

  g_clear_error(&error);
  delete repo_state;

  if (widgets && widgets->window_state.backend_warmup_label) {
    gtk_widget_set_visible(GTK_WIDGET(widgets->window_state.backend_warmup_label), FALSE);
  }
}

// -----------------------------------------------------------------------------
// GTK app setup (start here)
// -----------------------------------------------------------------------------
static void
activate(GtkApplication *app, gpointer)
{
  MainWindow main_window = main_window_create(app);

  setup_periodic_tasks();

  // Show the fully initialized window
  gtk_window_present(GTK_WINDOW(main_window.window));

  // Warm up the shared backend after the window is on screen
  StartupWarmupData *warmup = new StartupWarmupData();
  warmup->widgets = main_window.widgets;
  warmup->startup_cancellable = G_CANCELLABLE(g_object_ref(main_window.startup_cancellable));
  g_idle_add_full(G_PRIORITY_LOW, start_backend_warmup_idle, warmup, startup_warmup_data_free);
  g_object_unref(main_window.startup_cancellable);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
