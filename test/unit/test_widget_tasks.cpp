// -----------------------------------------------------------------------------
// test_widget_tasks.cpp
// Shared UI task lifetime and cancellation tests
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "test_gtk_utils.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"

#include <memory>

namespace {

struct WidgetTaskCompletion {
  std::weak_ptr<MainWindowUiState> widgets;
  bool called = false;
  bool state_retained = false;
  bool skipped = false;
  bool cancelled = false;
  GThread *starting_thread = nullptr;
  bool completed_on_starting_thread = false;
};

// -----------------------------------------------------------------------------
// Record task completion, cancellation, and window state ownership.
// -----------------------------------------------------------------------------
static void
on_widget_task_finished(GObject *, GAsyncResult *result, gpointer user_data)
{
  GTask *task = G_TASK(result);
  auto completion = *static_cast<std::shared_ptr<WidgetTaskCompletion> *>(g_task_get_task_data(task));
  completion->called = true;
  completion->completed_on_starting_thread = g_thread_self() == completion->starting_thread;

  auto widgets = completion->widgets.lock();
  completion->state_retained = widgets && widgets.get() == user_data;
  if (completion->state_retained) {
    completion->skipped = widgets_task_should_skip_completion(task, widgets.get());
  }

  GError *error = nullptr;
  g_task_propagate_boolean(task, &error);
  completion->cancelled = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&error);
}

// -----------------------------------------------------------------------------
// Complete a task on the worker thread.
// -----------------------------------------------------------------------------
static void
on_widget_task_worker(GTask *task, gpointer, gpointer, GCancellable *)
{
  g_task_return_boolean(task, TRUE);
}

// -----------------------------------------------------------------------------
// Keep completion data alive if a task finishes after a test timeout.
// -----------------------------------------------------------------------------
template <typename T>
static void
set_completion_data(GTask *task, const std::shared_ptr<T> &completion)
{
  g_task_set_task_data(
      task, new std::shared_ptr<T>(completion), [](gpointer p) { delete static_cast<std::shared_ptr<T> *>(p); });
}

// -----------------------------------------------------------------------------
// Dispatch pending callbacks until the condition is met or a timeout expires.
// -----------------------------------------------------------------------------
template <typename Predicate>
static bool
dispatch_until(Predicate done)
{
  const gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

  while (!done() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, FALSE);
    if (!done()) {
      g_usleep(1000);
    }
  }

  return done();
}

} // namespace

// -----------------------------------------------------------------------------
// An asynchronous task keeps the main window state alive after its owner releases it.
// -----------------------------------------------------------------------------
TEST_CASE("Window task retains state until the task is released")
{
  auto widgets = std::make_shared<MainWindowUiState>();
  std::weak_ptr<MainWindowUiState> weak_widgets = widgets;
  GTask *task = widgets_task_new_for_main_window_ui_state(widgets.get(), nullptr, nullptr);
  widgets.reset();

  REQUIRE_FALSE(weak_widgets.expired());
  {
    auto state = weak_widgets.lock();
    REQUIRE_FALSE(widgets_task_should_skip_completion(task, state.get()));

    state->window_state.destroyed = true;
    REQUIRE(widgets_task_should_skip_completion(task, state.get()));
  }

  g_object_unref(task);
  REQUIRE(weak_widgets.expired());
}

// -----------------------------------------------------------------------------
// Worker results must return to the starting thread while task state is retained.
// -----------------------------------------------------------------------------
TEST_CASE("Background task completion keeps window state alive")
{
  auto widgets = std::make_shared<MainWindowUiState>();
  std::weak_ptr<MainWindowUiState> weak_widgets = widgets;
  auto completion = std::make_shared<WidgetTaskCompletion>();
  completion->widgets = widgets;
  completion->starting_thread = g_thread_self();

  GTask *task = widgets_task_new_for_main_window_ui_state(widgets.get(), nullptr, on_widget_task_finished);
  set_completion_data(task, completion);
  g_task_run_in_thread(task, on_widget_task_worker);
  widgets.reset();
  g_object_unref(task);

  REQUIRE(dispatch_until([&]() { return completion->called; }));
  REQUIRE(completion->completed_on_starting_thread);
  REQUIRE(completion->state_retained);
  REQUIRE_FALSE(completion->skipped);
  REQUIRE_FALSE(completion->cancelled);
  REQUIRE(dispatch_until([&]() { return weak_widgets.expired(); }));
}

// -----------------------------------------------------------------------------
// A completed task must skip cancelled UI updates after its widget is destroyed.
// -----------------------------------------------------------------------------
TEST_CASE("Window destruction cancels task completion", "[gtk]")
{
  if (!test_gtk_display_available()) {
    SKIP("A GTK display is required for widget task tests.");
  }

  GtkWidget *window = gtk_window_new();
  GCancellable *cancellable = widgets_make_task_cancellable_for(window);
  auto widgets = std::make_shared<MainWindowUiState>();
  std::weak_ptr<MainWindowUiState> weak_widgets = widgets;
  auto completion = std::make_shared<WidgetTaskCompletion>();
  completion->widgets = widgets;
  completion->starting_thread = g_thread_self();

  GTask *task = widgets_task_new_for_main_window_ui_state(widgets.get(), cancellable, on_widget_task_finished);
  set_completion_data(task, completion);
  const bool initially_cancelled = g_cancellable_is_cancelled(cancellable);

  widgets.reset();
  gtk_window_destroy(GTK_WINDOW(window));
  const bool cancelled_after_destroy = g_cancellable_is_cancelled(cancellable);

  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
  const bool completed = dispatch_until([&]() { return completion->called; });
  g_object_unref(cancellable);

  REQUIRE_FALSE(initially_cancelled);
  REQUIRE(cancelled_after_destroy);
  REQUIRE(completed);
  REQUIRE(completion->completed_on_starting_thread);
  REQUIRE(completion->state_retained);
  REQUIRE(completion->skipped);
  REQUIRE(completion->cancelled);
  REQUIRE(dispatch_until([&]() { return weak_widgets.expired(); }));
}
