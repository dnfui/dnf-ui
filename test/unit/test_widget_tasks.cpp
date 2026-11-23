// -----------------------------------------------------------------------------
// test_widget_tasks.cpp
// Shared UI task lifetime and cancellation tests
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

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
};

// -----------------------------------------------------------------------------
// Record whether a cancelled task completion can still access the window state.
// -----------------------------------------------------------------------------
static void
on_widget_task_finished(GObject *, GAsyncResult *result, gpointer user_data)
{
  GTask *task = G_TASK(result);
  auto *completion = static_cast<WidgetTaskCompletion *>(g_task_get_task_data(task));
  completion->called = true;

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
// A completed task must skip cancelled UI updates after its widget is destroyed.
// -----------------------------------------------------------------------------
TEST_CASE("Window destruction cancels task completion", "[gtk]")
{
  if (!gtk_init_check()) {
    REQUIRE(g_getenv("DNFUI_TEST_REQUIRE_DISPLAY") == nullptr);
    SKIP("A GTK display is required for widget task tests.");
  }

  GtkWidget *window = gtk_window_new();
  GCancellable *cancellable = widgets_make_task_cancellable_for(window);
  auto widgets = std::make_shared<MainWindowUiState>();
  std::weak_ptr<MainWindowUiState> weak_widgets = widgets;
  WidgetTaskCompletion completion;
  completion.widgets = widgets;

  GTask *task = widgets_task_new_for_main_window_ui_state(widgets.get(), cancellable, on_widget_task_finished);
  g_task_set_task_data(task, &completion, nullptr);
  const bool initially_cancelled = g_cancellable_is_cancelled(cancellable);

  widgets.reset();
  gtk_window_destroy(GTK_WINDOW(window));
  const bool cancelled_after_destroy = g_cancellable_is_cancelled(cancellable);

  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
  while (!completion.called && g_main_context_pending(nullptr)) {
    g_main_context_iteration(nullptr, FALSE);
  }
  g_object_unref(cancellable);

  REQUIRE_FALSE(initially_cancelled);
  REQUIRE(cancelled_after_destroy);
  REQUIRE(completion.called);
  REQUIRE(completion.state_retained);
  REQUIRE(completion.skipped);
  REQUIRE(completion.cancelled);
  REQUIRE(weak_widgets.expired());
}
