#include <catch2/catch_test_macros.hpp>

#include "transaction/transaction_preview.hpp"
#include "ui/common/widgets.hpp"
#include "ui/transaction/transaction_dialogs.hpp"

#include <memory>

static bool
transaction_dialog_display_available()
{
  static const bool available = gtk_init_check();
  return available;
}

static GtkWindow *
find_summary_dialog(GtkWindow *parent)
{
  GListModel *windows = gtk_window_get_toplevels();
  for (guint index = 0; index < g_list_model_get_n_items(windows); ++index) {
    GtkWindow *window = GTK_WINDOW(g_list_model_get_item(windows, index));
    if (gtk_window_get_transient_for(window) == parent) {
      return window;
    }
    g_object_unref(window);
  }
  return nullptr;
}

static GtkWidget *
find_apply_button(GtkWidget *widget)
{
  if (GTK_IS_BUTTON(widget) && gtk_widget_has_css_class(widget, "suggested-action")) {
    return widget;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    if (GtkWidget *button = find_apply_button(child)) {
      return button;
    }
  }
  return nullptr;
}

// -----------------------------------------------------------------------------
// Verify that delayed summary destruction cannot unlock the main controls during Apply.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction summary destruction preserves the Apply lock", "[gtk]")
{
  if (!transaction_dialog_display_available()) {
    SKIP("A GTK display is required for transaction dialog tests.");
  }

  auto widgets = std::make_shared<MainWindowUiState>();
  GtkWindow *parent = GTK_WINDOW(gtk_window_new());
  widgets->query.entry = GTK_ENTRY(gtk_entry_new());
  gtk_window_set_child(parent, GTK_WIDGET(widgets->query.entry));

  transaction_dialogs_show_summary_dialog(
      widgets.get(),
      TransactionPreview {},
      +[](MainWindowUiState *widgets) {
        widgets->transaction_state.apply_in_progress = true;
        GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
        gtk_widget_set_sensitive(GTK_WIDGET(root), FALSE);
      },
      nullptr);

  // Keep a reference so the summary is disposed only after the Apply callback has run.
  GtkWindow *dialog = find_summary_dialog(parent);
  REQUIRE(dialog != nullptr);
  GtkWidget *apply_button = find_apply_button(GTK_WIDGET(dialog));
  REQUIRE(apply_button != nullptr);
  REQUIRE_FALSE(gtk_widget_is_sensitive(GTK_WIDGET(widgets->query.entry)));

  g_signal_emit_by_name(apply_button, "clicked");
  REQUIRE(widgets->transaction_state.apply_in_progress);

  g_object_run_dispose(G_OBJECT(dialog));
  REQUIRE_FALSE(gtk_widget_is_sensitive(GTK_WIDGET(parent)));
  REQUIRE_FALSE(gtk_widget_is_sensitive(GTK_WIDGET(widgets->query.entry)));

  g_object_unref(dialog);
  gtk_window_destroy(parent);
}

// -----------------------------------------------------------------------------
// Verify that cancelling the summary still restores the main controls.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction summary cancellation restores the main controls", "[gtk]")
{
  if (!transaction_dialog_display_available()) {
    SKIP("A GTK display is required for transaction dialog tests.");
  }

  auto widgets = std::make_shared<MainWindowUiState>();
  GtkWindow *parent = GTK_WINDOW(gtk_window_new());
  widgets->query.entry = GTK_ENTRY(gtk_entry_new());
  gtk_window_set_child(parent, GTK_WIDGET(widgets->query.entry));
  widgets->transaction_state.preview_transaction_path = "prepared-request";

  transaction_dialogs_show_summary_dialog(
      widgets.get(), TransactionPreview {}, nullptr, +[](MainWindowUiState *widgets) {
        widgets->transaction_state.preview_transaction_path.clear();
      });

  GtkWindow *dialog = find_summary_dialog(parent);
  REQUIRE(dialog != nullptr);
  REQUIRE_FALSE(gtk_widget_is_sensitive(GTK_WIDGET(widgets->query.entry)));

  gtk_window_destroy(dialog);
  g_object_run_dispose(G_OBJECT(dialog));
  REQUIRE(gtk_widget_is_sensitive(GTK_WIDGET(widgets->query.entry)));
  REQUIRE(widgets->transaction_state.preview_transaction_path.empty());

  g_object_unref(dialog);
  gtk_window_destroy(parent);
}
