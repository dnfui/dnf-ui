// -----------------------------------------------------------------------------
// src/ui/window/main_window.cpp
// Main application window
// Builds the primary GTK window, creates shared widget state, and wires callbacks.
// -----------------------------------------------------------------------------
#include "ui/window/main_window.hpp"

#include "config.hpp"
#include "i18n.hpp"
#include "ui/history/transaction_history_view.hpp"
#include "ui/window/main_menu.hpp"
#include "ui/window/main_window_layout.hpp"
#include "ui/details/package_details_controller.hpp"
#include "ui/package_query/package_query_controller.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/transaction/pending_transaction_controller.hpp"
#include "ui/refresh/repository_refresh_controller.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"

#include <memory>

struct MainWindowCleanupData {
  std::shared_ptr<MainWindowUiState> widgets;
  GCancellable *startup_cancellable = nullptr;
};

// -----------------------------------------------------------------------------
// Function forward declarations
// -----------------------------------------------------------------------------
static GtkWidget *create_window(GtkApplication *app);
static gboolean toggle_stateful_window_action(GtkWidget *widget, const char *action_name);
static void setup_shortcuts(GtkWidget *window, GtkWidget *entry);
static std::shared_ptr<MainWindowUiState> create_main_window_ui_state(const AppWidgets *ui);
static void setup_css(MainWindowUiState *widgets);
static void initialize_ui_state(MainWindowUiState *widgets);
static void connect_signals(const AppWidgets *ui, MainWindowUiState *widgets);
static void
connect_cleanup(GtkWidget *window, std::shared_ptr<MainWindowUiState> widgets, GCancellable *startup_cancellable);
static void show_pending_quit_dialog(MainWindowUiState *widgets);
static gboolean on_main_window_close_request(GtkWindow *window, gpointer user_data);

// -----------------------------------------------------------------------------
// Create main application window
// -----------------------------------------------------------------------------
static GtkWidget *
create_window(GtkApplication *app)
{
  GtkWidget *window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), _("DNF UI"));
  config_load_window_geometry(GTK_WINDOW(window));

  return window;
}

// -----------------------------------------------------------------------------
// Toggle one stateful window action by name.
// -----------------------------------------------------------------------------
static gboolean
toggle_stateful_window_action(GtkWidget *widget, const char *action_name)
{
  if (!widget || !action_name) {
    return FALSE;
  }

  GActionGroup *actions = G_ACTION_GROUP(g_object_get_data(G_OBJECT(widget), "dnfui-menu-action-group"));
  if (!actions) {
    return FALSE;
  }

  GVariant *state = g_action_group_get_action_state(actions, action_name);
  if (!state) {
    return FALSE;
  }

  gboolean enabled = g_variant_get_boolean(state);
  g_variant_unref(state);
  g_action_group_change_action_state(actions, action_name, g_variant_new_boolean(!enabled));

  return TRUE;
}

// -----------------------------------------------------------------------------
// Setup window keyboard shortcuts
// -----------------------------------------------------------------------------
static void
setup_shortcuts(GtkWidget *window, GtkWidget *entry)
{
  // Keyboard shortcuts for closing the window and focusing search.
  GtkEventController *shortcuts = GTK_EVENT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_widget_add_controller(window, shortcuts);

  auto shortcut_callback = +[](GtkWidget *widget, GVariant *, gpointer) -> gboolean {
    gtk_window_close(GTK_WINDOW(widget));
    return TRUE;
  };

  // Close with Ctrl+Q.
  GtkShortcut *close_shortcut_q = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_q, GDK_CONTROL_MASK),
                                                   gtk_callback_action_new(shortcut_callback, NULL, NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), close_shortcut_q);

  // Close with Ctrl+W.
  GtkShortcut *close_shortcut_w = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_w, GDK_CONTROL_MASK),
                                                   gtk_callback_action_new(shortcut_callback, NULL, NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), close_shortcut_w);

  // Focus search with Ctrl+F.
  GtkShortcut *focus_search = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_f, GDK_CONTROL_MASK),
                                               gtk_callback_action_new(
                                                   +[](GtkWidget *, GVariant *, gpointer user_data) -> gboolean {
                                                     GtkEntry *entry = static_cast<GtkEntry *>(user_data);
                                                     gtk_widget_grab_focus(GTK_WIDGET(entry));
                                                     return TRUE;
                                                   },
                                                   entry,
                                                   NULL));

  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), focus_search);

  GtkShortcut *clear_list = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_l, GDK_CONTROL_MASK),
                                             gtk_callback_action_new(
                                                 +[](GtkWidget *widget, GVariant *, gpointer) -> gboolean {
                                                   return gtk_widget_activate_action(widget, "win.clear-list", NULL);
                                                 },
                                                 NULL,
                                                 NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), clear_list);

  // Export Package List with Ctrl+E.
  GtkShortcut *export_package_list =
      gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_e, GDK_CONTROL_MASK),
                       gtk_callback_action_new(
                           +[](GtkWidget *widget, GVariant *, gpointer) -> gboolean {
                             return gtk_widget_activate_action(widget, "win.export-package-list", NULL);
                           },
                           NULL,
                           NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), export_package_list);

  // Toggle Package Info Panel with Ctrl+I.
  GtkShortcut *toggle_info = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_i, GDK_CONTROL_MASK),
                                              gtk_callback_action_new(
                                                  +[](GtkWidget *widget, GVariant *, gpointer) -> gboolean {
                                                    return toggle_stateful_window_action(widget, "show-info");
                                                  },
                                                  NULL,
                                                  NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), toggle_info);

  // Toggle History Panel with Ctrl+H.
  GtkShortcut *toggle_history = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_h, GDK_CONTROL_MASK),
                                                 gtk_callback_action_new(
                                                     +[](GtkWidget *widget, GVariant *, gpointer) -> gboolean {
                                                       return toggle_stateful_window_action(widget, "show-history");
                                                     },
                                                     NULL,
                                                     NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), toggle_history);

  // Open Transaction History with Ctrl+Shift+H.
  GtkShortcut *transaction_history = gtk_shortcut_new(
      gtk_keyval_trigger_new(GDK_KEY_H, static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_SHIFT_MASK)),
      gtk_callback_action_new(
          +[](GtkWidget *widget, GVariant *, gpointer) -> gboolean {
            return gtk_widget_activate_action(widget, "win.transaction-history", NULL);
          },
          NULL,
          NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), transaction_history);
}

// -----------------------------------------------------------------------------
// Create shared MainWindowUiState state from UI handles
// Kept alive by the main window cleanup data and running UI tasks
// -----------------------------------------------------------------------------
static std::shared_ptr<MainWindowUiState>
create_main_window_ui_state(const AppWidgets *ui)
{
  auto widgets = std::make_shared<MainWindowUiState>();
  widgets->query.entry = GTK_ENTRY(ui->entry);
  widgets->query.history_list = GTK_LIST_BOX(ui->history_list);
  widgets->query.spinner = GTK_SPINNER(ui->spinner);
  widgets->query.search_button = GTK_BUTTON(ui->search_button);
  widgets->query.list_button = GTK_BUTTON(ui->list_button);
  widgets->query.list_available_button = GTK_BUTTON(ui->list_available_button);
  widgets->query.list_upgradeable_button = GTK_BUTTON(ui->list_upgradeable_button);
  widgets->query.refresh_button = GTK_BUTTON(ui->refresh_button);
  widgets->query.status_label = GTK_LABEL(ui->status_label);
  widgets->query.desc_checkbox = GTK_CHECK_BUTTON(ui->desc_checkbox);
  widgets->query.exact_checkbox = GTK_CHECK_BUTTON(ui->exact_checkbox);
  widgets->query.latest_checkbox = GTK_CHECK_BUTTON(ui->latest_checkbox);

  widgets->results.listbox = GTK_LIST_BOX(ui->listbox);
  widgets->results.list_scroller = GTK_SCROLLED_WINDOW(ui->scrolled_list);
  widgets->results.inner_paned = GTK_PANED(ui->inner_paned);
  widgets->results.details_stack = GTK_STACK(ui->details_stack);
  widgets->results.details_buffer = ui->details_buffer;
  widgets->results.files_buffer = ui->files_buffer;
  widgets->results.deps_buffer = ui->deps_buffer;
  widgets->results.changelog_buffer = ui->changelog_buffer;
  widgets->results.count_label = GTK_LABEL(ui->count_label);

  widgets->transaction.install_button = GTK_BUTTON(ui->install_button);
  widgets->transaction.remove_button = GTK_BUTTON(ui->remove_button);
  widgets->transaction.reinstall_button = GTK_BUTTON(ui->reinstall_button);
  widgets->transaction.upgrade_all_button = GTK_BUTTON(ui->upgrade_all_button);
  widgets->transaction.mark_listed_upgrades_button = GTK_BUTTON(ui->mark_listed_upgrades_button);
  widgets->transaction.apply_button = GTK_BUTTON(ui->apply_button);
  widgets->transaction.clear_pending_button = GTK_BUTTON(ui->clear_pending_button);
  widgets->transaction.pending_list = GTK_LIST_BOX(ui->pending_list);

  widgets->query_state.package_list_cancellable = nullptr;
  widgets->query_state.next_package_list_request_id = 1;
  widgets->query_state.current_package_list_request_id = 0;
  widgets->query_state.current_package_list_request_kind = PackageListRequestKind::NONE;

  widgets->window_state.allow_close_with_pending = false;
  widgets->window_state.pending_quit_dialog_open = false;
  widgets->window_state.backend_warmup_label = GTK_LABEL(ui->warmup_label);
  widgets->window_state.query_duration_label = GTK_LABEL(ui->query_duration_label);
  widgets->window_state.backend_warmup_cancellable = nullptr;

  return widgets;
}

// -----------------------------------------------------------------------------
// Setup GTK CSS styling
// -----------------------------------------------------------------------------
static void
setup_css(MainWindowUiState *widgets)
{
  GtkCssProvider *css = gtk_css_provider_new();
  // The few custom style rules are kept next to the widgets they style.
  gtk_css_provider_load_from_string(css,
                                    "label.status-bar { padding: 6px 8px; border-radius: 6px; } "
                                    ".top-controls { "
                                    "  padding: 10px 12px 8px 12px; "
                                    "  border-bottom: 1px solid @borders; "
                                    "  background-color: alpha(@theme_bg_color, 0.82); "
                                    "} "
                                    ".control-row { "
                                    "  border-spacing: 7px; "
                                    "} "
                                    ".history-sidebar { "
                                    "  padding: 10px; "
                                    "  border-right: 1px solid @borders; "
                                    "  background-color: alpha(@theme_bg_color, 0.55); "
                                    "} "
                                    ".history-heading { "
                                    "  margin: 2px 4px 6px 4px; "
                                    "  font-weight: 700; "
                                    "} "
                                    ".history-list { "
                                    "  background: transparent; "
                                    "} "
                                    ".transaction-history-row { "
                                    "  padding-left: 8px; "
                                    "  border-left: 3px solid transparent; "
                                    "} "
                                    ".transaction-history-install { "
                                    "  border-left-color: #2a7b43; "
                                    "} "
                                    ".transaction-history-upgrade { "
                                    "  border-left-color: #1c71d8; "
                                    "} "
                                    ".transaction-history-downgrade { "
                                    "  border-left-color: #986a00; "
                                    "} "
                                    ".transaction-history-reinstall { "
                                    "  border-left-color: #8f6500; "
                                    "} "
                                    ".transaction-history-remove { "
                                    "  border-left-color: #c01c28; "
                                    "} "
                                    ".transaction-history-replaced { "
                                    "  border-left-color: #6f8396; "
                                    "} "
                                    ".transaction-history-reason { "
                                    "  border-left-color: #7a5fa5; "
                                    "} "
                                    ".transaction-history-other { "
                                    "  border-left-color: #7d7d7d; "
                                    "} "
                                    ".transaction-history-failed { "
                                    "  background-color: rgba(192, 28, 40, 0.06); "
                                    "} "
                                    ".bottom-bar { "
                                    "  padding: 6px; "
                                    "  border-top: 1px solid @borders; "
                                    "  background-color: alpha(@theme_bg_color, 0.55); "
                                    "} "
                                    ".package-status { "
                                    "  padding: 3px 9px; "
                                    "  border-radius: 999px; "
                                    "  border: 1px solid transparent; "
                                    "  font-weight: 600; "
                                    "} "
                                    ".package-status-icon { "
                                    "  opacity: 0.86; "
                                    "} "
                                    ".package-status-available { "
                                    "  background-color: #dbe5f2; "
                                    "  border-color: #b8c7d9; "
                                    "  color: #20364f; "
                                    "} "
                                    ".package-status-installed { "
                                    "  background-color: #dcebd5; "
                                    "  border-color: #b8d2aa; "
                                    "  color: #25451e; "
                                    "} "
                                    ".package-status-local-only { "
                                    "  background-color: #d8ecea; "
                                    "  border-color: #a7cbc7; "
                                    "  color: #1d4542; "
                                    "} "
                                    ".package-status-upgradeable { "
                                    "  background-color: #f3e5bf; "
                                    "  border-color: #d6b862; "
                                    "  color: #4a3505; "
                                    "} "
                                    ".package-status-installed-newer { "
                                    "  background-color: #f0ddd6; "
                                    "  border-color: #d7aa9a; "
                                    "  color: #5a2a1b; "
                                    "} "
                                    ".package-status-pending-install { "
                                    "  background-color: #1c71d8; "
                                    "  border-color: #15539e; "
                                    "  color: #ffffff; "
                                    "} "
                                    ".package-status-pending-reinstall { "
                                    "  background-color: #e5a50a; "
                                    "  border-color: #b58300; "
                                    "  color: #2e2100; "
                                    "} "
                                    ".package-status-pending-remove { "
                                    "  background-color: #c01c28; "
                                    "  border-color: #8f141d; "
                                    "  color: #ffffff; "
                                    "} "
                                    ".package-table-view row:selected { "
                                    "  background-color: #62a0ea; "
                                    "} "
                                    ".package-row-pending-install { "
                                    "  background-color: #1c71d8; "
                                    "  box-shadow: -1px 0 0 #1c71d8, 1px 0 0 #1c71d8; "
                                    "  color: #ffffff; "
                                    "} "
                                    ".package-table-view row:selected .package-row-pending-install { "
                                    "  background-color: #15539e; "
                                    "  box-shadow: -1px 0 0 #15539e, 1px 0 0 #15539e; "
                                    "} "
                                    ".package-row-pending-reinstall { "
                                    "  background-color: #e5a50a; "
                                    "  box-shadow: -1px 0 0 #e5a50a, 1px 0 0 #e5a50a; "
                                    "  color: #2e2100; "
                                    "} "
                                    ".package-table-view row:selected .package-row-pending-reinstall { "
                                    "  background-color: #b58300; "
                                    "  box-shadow: -1px 0 0 #b58300, 1px 0 0 #b58300; "
                                    "} "
                                    ".package-row-pending-remove { "
                                    "  background-color: #c01c28; "
                                    "  box-shadow: -1px 0 0 #c01c28, 1px 0 0 #c01c28; "
                                    "  color: #ffffff; "
                                    "} "
                                    ".package-table-view row:selected .package-row-pending-remove { "
                                    "  background-color: #8f141d; "
                                    "  box-shadow: -1px 0 0 #8f141d, 1px 0 0 #8f141d; "
                                    "} "
                                    ".package-meta { "
                                    "  opacity: 0.78; "
                                    "} "
                                    ".package-summary { "
                                    "  opacity: 0.92; "
                                    "} "
                                    ".package-empty-state { "
                                    "  min-width: 340px; "
                                    "  padding: 26px 30px; "
                                    "  border-radius: 14px; "
                                    "  border: 1px solid alpha(@theme_fg_color, 0.12); "
                                    "  background-color: alpha(@theme_base_color, 0.92); "
                                    "} "
                                    ".package-empty-icon { "
                                    "  opacity: 0.62; "
                                    "} "
                                    ".package-empty-title { "
                                    "  font-size: 1.25em; "
                                    "} "
                                    ".package-empty-message { "
                                    "  opacity: 0.82; "
                                    "} "
                                    ".package-empty-shortcuts { "
                                    "  margin-top: 8px; "
                                    "  opacity: 0.72; "
                                    "} "
                                    ".details-switcher { "
                                    "  margin: 6px; "
                                    "} ");
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
  gtk_widget_add_css_class(GTK_WIDGET(widgets->query.status_label), "status-bar");
  g_object_unref(css);
}

// -----------------------------------------------------------------------------
// Initialize widget state after construction
// -----------------------------------------------------------------------------
static void
initialize_ui_state(MainWindowUiState *widgets)
{
  // Apply and Clear Transactions are meaningful only when there are pending actions
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.apply_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.clear_pending_button), FALSE);

  ui_helpers_set_status(widgets->query.status_label, _("Ready."), "gray");
  package_table_fill_package_view(widgets, {});
}

// -----------------------------------------------------------------------------
// Connect all GTK signals and callbacks
// -----------------------------------------------------------------------------
static void
connect_signals(const AppWidgets *ui, MainWindowUiState *widgets)
{
  MainMenuWidgets menu_widgets;
  menu_widgets.window = ui->window;
  menu_widgets.history_panel = ui->vbox_history;
  menu_widgets.info_panel = ui->details_panel;
  main_menu_connect_actions(menu_widgets, widgets);

  g_signal_connect(ui->list_button, "clicked", G_CALLBACK(package_query_on_list_button_clicked), widgets);

  g_signal_connect(
      ui->list_available_button, "clicked", G_CALLBACK(package_query_on_list_available_button_clicked), widgets);

  g_signal_connect(
      ui->list_upgradeable_button, "clicked", G_CALLBACK(package_query_on_list_upgradeable_button_clicked), widgets);

  g_signal_connect(ui->install_button, "clicked", G_CALLBACK(pending_transaction_on_install_button_clicked), widgets);

  g_signal_connect(
      ui->reinstall_button, "clicked", G_CALLBACK(pending_transaction_on_reinstall_button_clicked), widgets);

  g_signal_connect(ui->remove_button, "clicked", G_CALLBACK(pending_transaction_on_remove_button_clicked), widgets);

  g_signal_connect(
      ui->upgrade_all_button, "clicked", G_CALLBACK(pending_transaction_on_upgrade_all_button_clicked), widgets);

  g_signal_connect(ui->mark_listed_upgrades_button,
                   "clicked",
                   G_CALLBACK(pending_transaction_on_mark_listed_upgrades_button_clicked),
                   widgets);

  g_signal_connect(ui->search_button, "clicked", G_CALLBACK(package_query_on_search_button_clicked), widgets);

  g_signal_connect(ui->entry, "activate", G_CALLBACK(package_query_on_search_button_clicked), widgets);
  g_signal_connect(ui->history_list, "row-selected", G_CALLBACK(package_query_on_history_row_selected), widgets);

  g_signal_connect(ui->apply_button, "clicked", G_CALLBACK(pending_transaction_on_apply_button_clicked), widgets);
  g_signal_connect(
      ui->clear_pending_button, "clicked", G_CALLBACK(pending_transaction_on_clear_pending_button_clicked), widgets);

  g_signal_connect(ui->refresh_button, "clicked", G_CALLBACK(repository_refresh_on_button_clicked), widgets);

  g_signal_connect(
      ui->details_stack, "notify::visible-child-name", G_CALLBACK(package_details_on_details_page_changed), widgets);

  // Intercept window close when pending or running transaction work needs attention.
  g_signal_connect(ui->window, "close-request", G_CALLBACK(on_main_window_close_request), widgets);

  // Live-update: save pane position whenever the user moves the divider
  g_signal_connect(ui->inner_paned,
                   "notify::position",
                   G_CALLBACK(+[](GtkPaned *paned, GParamSpec *, gpointer) { config_save_paned_position(paned); }),
                   NULL);
}

// -----------------------------------------------------------------------------
// Show a confirmation dialog before closing the app with unapplied changes.
// -----------------------------------------------------------------------------
static void
show_pending_quit_dialog(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  widgets->window_state.pending_quit_dialog_open = true;

  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dialog, _("Quit and discard marked changes?"));
  gtk_window_set_default_size(dialog, 520, 180);
  gtk_window_set_modal(dialog, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
  if (root && GTK_IS_WINDOW(root)) {
    GtkWindow *parent = GTK_WINDOW(root);
    if (GtkApplication *app = gtk_window_get_application(parent)) {
      gtk_window_set_application(dialog, app);
    }
    gtk_window_set_transient_for(dialog, parent);
  }

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *title = gtk_label_new(nullptr);
  gchar *title_markup = g_markup_printf_escaped("<b>%s</b>", _("Quit and discard marked changes?"));
  gtk_label_set_markup(GTK_LABEL(title), title_markup);
  g_free(title_markup);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_box_append(GTK_BOX(outer), title);

  GtkWidget *message = gtk_label_new(
      _("There are still marked changes that have not yet been applied. They will get lost if you choose to quit."));
  gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_box_append(GTK_BOX(outer), message);

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  GtkWidget *cancel_button = gtk_button_new_with_label(_("Cancel"));
  gtk_box_append(GTK_BOX(button_box), cancel_button);

  GtkWidget *quit_button = gtk_button_new_with_label(_("Quit"));
  gtk_widget_add_css_class(quit_button, "destructive-action");
  gtk_box_append(GTK_BOX(button_box), quit_button);

  g_signal_connect(cancel_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                   }),
                   nullptr);

  g_signal_connect(quit_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                     MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
                     widgets->window_state.allow_close_with_pending = true;

                     GtkRoot *dialog_root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (dialog_root && GTK_IS_WINDOW(dialog_root)) {
                       gtk_window_destroy(GTK_WINDOW(dialog_root));
                     }

                     GtkRoot *parent_root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
                     if (parent_root && GTK_IS_WINDOW(parent_root)) {
                       gtk_window_close(GTK_WINDOW(parent_root));
                     }
                   }),
                   widgets);

  g_signal_connect(dialog,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer user_data) {
                     MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
                     widgets->window_state.pending_quit_dialog_open = false;
                   }),
                   widgets);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// Handle close requests that need confirmation or must wait for apply to finish.
// -----------------------------------------------------------------------------
static gboolean
on_main_window_close_request(GtkWindow *window, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);

  if (!widgets) {
    config_save_window_geometry(window);
    return FALSE;
  }

  if (widgets->transaction.apply_in_progress) {
    ui_helpers_set_status(widgets->query.status_label, _("A transaction is still applying."), "blue");
    return TRUE;
  }

  if (widgets->window_state.allow_close_with_pending) {
    config_save_window_geometry(window);
    if (widgets->results.inner_paned) {
      config_save_paned_position(widgets->results.inner_paned);
    }
    return FALSE;
  }

  if (widgets->transaction.actions.empty()) {
    config_save_window_geometry(window);
    if (widgets->results.inner_paned) {
      config_save_paned_position(widgets->results.inner_paned);
    }
    return FALSE;
  }

  if (!widgets->window_state.pending_quit_dialog_open) {
    show_pending_quit_dialog(widgets);
  }

  return TRUE;
}

// -----------------------------------------------------------------------------
// Connect window cleanup.
// -----------------------------------------------------------------------------
static void
connect_cleanup(GtkWidget *window, std::shared_ptr<MainWindowUiState> widgets, GCancellable *startup_cancellable)
{
  MainWindowCleanupData *cleanup_data = new MainWindowCleanupData();
  cleanup_data->widgets = std::move(widgets);
  cleanup_data->startup_cancellable = G_CANCELLABLE(g_object_ref(startup_cancellable));

  g_signal_connect(window,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer data) {
                     MainWindowCleanupData *cleanup_data = static_cast<MainWindowCleanupData *>(data);
                     MainWindowUiState *widgets = cleanup_data ? cleanup_data->widgets.get() : nullptr;
                     if (cleanup_data && cleanup_data->startup_cancellable) {
                       g_cancellable_cancel(cleanup_data->startup_cancellable);
                       g_object_unref(cleanup_data->startup_cancellable);
                       cleanup_data->startup_cancellable = nullptr;
                     }
                     if (!widgets) {
                       delete cleanup_data;
                       return;
                     }
                     widgets->window_state.destroyed = true;
                     transaction_history_close_window();
                     if (widgets->window_state.backend_warmup_cancellable) {
                       g_cancellable_cancel(widgets->window_state.backend_warmup_cancellable);
                       g_object_unref(widgets->window_state.backend_warmup_cancellable);
                       widgets->window_state.backend_warmup_cancellable = nullptr;
                     }
                     if (widgets->query_state.package_list_cancellable) {
                       g_cancellable_cancel(widgets->query_state.package_list_cancellable);
                       g_object_unref(widgets->query_state.package_list_cancellable);
                       widgets->query_state.package_list_cancellable = nullptr;
                     }
                     repository_refresh_cancel_active();
                     package_details_cancel_active_load(widgets);
                     if (!widgets->transaction.preview_transaction_path.empty()) {
                       transaction_service_client_release_request_async(widgets->transaction.preview_transaction_path);
                       widgets->transaction.preview_transaction_path.clear();
                     }
                     delete cleanup_data;
                   }),
                   cleanup_data);
}

// -----------------------------------------------------------------------------
// Create and initialize the main application window
// -----------------------------------------------------------------------------
MainWindow
main_window_create(GtkApplication *app)
{
  AppWidgets ui = {};
  ui.window = create_window(app);

  main_window_build_layout(&ui);

  std::shared_ptr<MainWindowUiState> widgets = create_main_window_ui_state(&ui);
  GCancellable *startup_cancellable = g_cancellable_new();

  setup_shortcuts(ui.window, ui.entry);
  setup_css(widgets.get());
  initialize_ui_state(widgets.get());
  connect_signals(&ui, widgets.get());
  connect_cleanup(ui.window, widgets, startup_cancellable);

  MainWindow main_window;
  main_window.window = ui.window;
  main_window.widgets = widgets.get();
  main_window.startup_cancellable = startup_cancellable;

  return main_window;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
