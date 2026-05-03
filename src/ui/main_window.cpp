// -----------------------------------------------------------------------------
// src/ui/main_window.cpp
// Main application window
// Builds the primary GTK window, creates shared widget state, wires callbacks,
// and owns window-level UI lifecycle behavior.
// -----------------------------------------------------------------------------
#include "main_window.hpp"

#include "config.hpp"
#include "i18n.hpp"
#include "main_menu.hpp"
#include "package_query_controller.hpp"
#include "package_table_view.hpp"
#include "pending_transaction_controller.hpp"
#include "transaction_service_client.hpp"
#include "ui_helpers.hpp"
#include "widgets.hpp"

#include <memory>

// -----------------------------------------------------------------------------
// Internal UI handles used only during application setup
// Keeps widget construction readable without extending SearchWidgets
// -----------------------------------------------------------------------------
struct AppWidgets {
  GtkWidget *window = NULL;

  GtkWidget *vbox_root = NULL;
  GtkWidget *vbox_main = NULL;
  GtkWidget *vbox_history = NULL;

  GtkWidget *history_list = NULL;

  GtkWidget *entry = NULL;
  GtkWidget *search_button = NULL;
  GtkWidget *desc_checkbox = NULL;
  GtkWidget *exact_checkbox = NULL;
  GtkWidget *spinner = NULL;

  GtkWidget *list_button = NULL;
  GtkWidget *list_available_button = NULL;
  GtkWidget *list_upgradeable_button = NULL;
  GtkWidget *refresh_button = NULL;

  GtkWidget *install_button = NULL;
  GtkWidget *reinstall_button = NULL;
  GtkWidget *remove_button = NULL;
  GtkWidget *upgrade_all_button = NULL;
  GtkWidget *apply_button = NULL;
  GtkWidget *clear_pending_button = NULL;

  GtkWidget *status_label = NULL;
  GtkWidget *inner_paned = NULL;

  GtkWidget *scrolled_list = NULL;
  GtkWidget *listbox = NULL;
  GtkWidget *notebook = NULL;

  GtkTextBuffer *details_buffer = NULL;
  GtkTextBuffer *files_buffer = NULL;
  GtkTextBuffer *deps_buffer = NULL;
  GtkTextBuffer *changelog_buffer = NULL;
  GtkWidget *pending_list = NULL;

  GtkWidget *count_label = NULL;
  GtkWidget *warmup_label = NULL;
};

struct MainWindowCleanupData {
  std::shared_ptr<SearchWidgets> widgets;
  GCancellable *startup_cancellable = nullptr;
};

// -----------------------------------------------------------------------------
// Function forward declarations
// -----------------------------------------------------------------------------
static GtkWidget *create_window(GtkApplication *app);
static GtkWidget *create_thin_separator(void);
static GtkWidget *create_scrolled_text_view(const char *text, GtkWrapMode wrap_mode, GtkTextBuffer **out_buffer);
static void setup_shortcuts(GtkWidget *window, GtkWidget *entry);
static void build_main_ui(AppWidgets *ui);
static std::shared_ptr<SearchWidgets> create_search_widgets(const AppWidgets *ui);
static void setup_css(SearchWidgets *widgets);
static void initialize_ui_state(SearchWidgets *widgets);
static void connect_signals(const AppWidgets *ui, SearchWidgets *widgets);
static void
connect_cleanup(GtkWidget *window, std::shared_ptr<SearchWidgets> widgets, GCancellable *startup_cancellable);
static void show_pending_quit_dialog(SearchWidgets *widgets);
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
// Create a reusable flat line separator
// -----------------------------------------------------------------------------
static GtkWidget *
create_thin_separator(void)
{
  GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(line, -1, 1);
  gtk_widget_add_css_class(line, "thin-line");

  return line;
}

// -----------------------------------------------------------------------------
// Create selectable read-only text view inside a scrolled window
// -----------------------------------------------------------------------------
static GtkWidget *
create_scrolled_text_view(const char *text, GtkWrapMode wrap_mode, GtkTextBuffer **out_buffer)
{
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), wrap_mode);
  gtk_widget_set_focusable(view, TRUE);
  gtk_widget_set_margin_start(view, 10);
  gtk_widget_set_margin_end(view, 10);
  gtk_widget_set_margin_top(view, 10);
  gtk_widget_set_margin_bottom(view, 10);

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  gtk_text_buffer_set_text(buffer, text, -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);

  if (out_buffer) {
    *out_buffer = buffer;
  }

  return scrolled;
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
}

// -----------------------------------------------------------------------------
// Build all GTK widgets and main application layout
// -----------------------------------------------------------------------------
static void
build_main_ui(AppWidgets *ui)
{
  GtkWidget *window = ui->window;

  GtkWidget *vbox_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_window_set_child(GTK_WINDOW(window), vbox_root);
  ui->vbox_root = vbox_root;

  GtkWidget *menu_bar = main_menu_create();
  gtk_box_append(GTK_BOX(vbox_root), menu_bar);

  GtkWidget *outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(outer_paned), 200);

  GtkWidget *vbox_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_paned_set_end_child(GTK_PANED(outer_paned), vbox_main);
  ui->vbox_main = vbox_main;

  GtkWidget *vbox_history = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_vexpand(vbox_history, TRUE);
  gtk_widget_set_hexpand(vbox_history, TRUE);
  gtk_paned_set_start_child(GTK_PANED(outer_paned), vbox_history);
  ui->vbox_history = vbox_history;

  GtkWidget *history_label = gtk_label_new(_("Search History"));
  gtk_label_set_xalign(GTK_LABEL(history_label), 0.0);
  gtk_box_append(GTK_BOX(vbox_history), history_label);

  // --- Flat line separator below Search History label ---
  gtk_box_append(GTK_BOX(vbox_history), create_thin_separator());

  GtkWidget *scrolled_history = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled_history, TRUE);
  gtk_widget_set_hexpand(scrolled_history, TRUE);
  gtk_box_append(GTK_BOX(vbox_history), scrolled_history);

  GtkWidget *history_list = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_history), history_list);
  ui->history_list = history_list;

  // --- Search bar row ---
  GtkWidget *hbox_search = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_root), hbox_search);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("Search packages..."));
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(hbox_search), entry);
  ui->entry = entry;

  GtkWidget *search_button = ui_helpers_create_icon_button("system-search-symbolic", _("Search"));
  gtk_box_append(GTK_BOX(hbox_search), search_button);
  ui->search_button = search_button;

  GtkWidget *desc_checkbox = gtk_check_button_new_with_label(_("Search in description"));
  gtk_box_append(GTK_BOX(hbox_search), desc_checkbox);
  ui->desc_checkbox = desc_checkbox;

  GtkWidget *exact_checkbox = gtk_check_button_new_with_label(_("Exact match"));
  gtk_box_append(GTK_BOX(hbox_search), exact_checkbox);
  ui->exact_checkbox = exact_checkbox;

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(hbox_search), spinner);
  ui->spinner = spinner;

  // --- Flat line separator below Search bar ---
  gtk_box_append(GTK_BOX(vbox_root), create_thin_separator());

  // --- Buttons row ---
  GtkWidget *hbox_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_root), hbox_buttons);

  GtkWidget *list_button = ui_helpers_create_icon_button("view-list-symbolic", _("List Installed"));
  gtk_box_append(GTK_BOX(hbox_buttons), list_button);
  ui->list_button = list_button;

  GtkWidget *list_available_button = ui_helpers_create_icon_button("view-list-symbolic", _("List Packages"));
  gtk_box_append(GTK_BOX(hbox_buttons), list_available_button);
  ui->list_available_button = list_available_button;

  GtkWidget *list_upgradeable_button = ui_helpers_create_icon_button("view-list-symbolic", _("List Upgradable"));
  gtk_box_append(GTK_BOX(hbox_buttons), list_upgradeable_button);
  ui->list_upgradeable_button = list_upgradeable_button;

  // --- Refresh Repositories button ---
  // Triggers an asynchronous repository rebuild using BaseManager::rebuild()
  // Runs in a background thread to keep the GTK UI responsive
  GtkWidget *refresh_button = ui_helpers_create_icon_button("view-refresh-symbolic", _("Refresh Repositories"));
  gtk_box_append(GTK_BOX(hbox_buttons), refresh_button);
  ui->refresh_button = refresh_button;

  // Transaction buttons row for marking and applying package actions.
  GtkWidget *hbox_tx_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_root), hbox_tx_buttons);

  GtkWidget *install_button = ui_helpers_create_icon_button("list-add-symbolic", _("Mark for Install"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), install_button);
  ui->install_button = install_button;

  GtkWidget *reinstall_button = ui_helpers_create_icon_button("view-refresh-symbolic", _("Mark for Reinstall"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), reinstall_button);
  ui->reinstall_button = reinstall_button;

  GtkWidget *remove_button = ui_helpers_create_icon_button("list-remove-symbolic", _("Mark for Removal"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), remove_button);
  ui->remove_button = remove_button;

  GtkWidget *upgrade_all_button = ui_helpers_create_icon_button("view-refresh-symbolic", _("Upgrade All"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), upgrade_all_button);
  ui->upgrade_all_button = upgrade_all_button;

  GtkWidget *apply_button = ui_helpers_create_icon_button("emblem-ok-symbolic", _("Apply Transactions"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), apply_button);
  ui->apply_button = apply_button;

  GtkWidget *clear_pending_button = ui_helpers_create_icon_button("edit-clear-symbolic", _("Clear Transactions"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), clear_pending_button);
  ui->clear_pending_button = clear_pending_button;

  // --- Flat line separator ---
  gtk_box_append(GTK_BOX(vbox_root), create_thin_separator());

  GtkWidget *status_label = gtk_label_new(_("Ready."));
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
  gtk_label_set_selectable(GTK_LABEL(status_label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
  gtk_box_append(GTK_BOX(vbox_root), status_label);
  ui->status_label = status_label;

  gtk_box_append(GTK_BOX(vbox_root), create_thin_separator());

  // History panel and package list fill the remaining space below the toolbar
  gtk_widget_set_vexpand(outer_paned, TRUE);
  gtk_box_append(GTK_BOX(vbox_root), outer_paned);

  // Inner paned area with packages above details.
  GtkWidget *inner_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_box_append(GTK_BOX(vbox_main), inner_paned);
  gtk_widget_set_vexpand(inner_paned, TRUE);
  gtk_widget_set_hexpand(inner_paned, TRUE);
  int pos = config_load_paned_position();
  if (pos < 100) {
    pos = 400;
  }
  gtk_paned_set_position(GTK_PANED(inner_paned), pos);
  ui->inner_paned = inner_paned;

  // --- Top: package list ---
  GtkWidget *scrolled_list = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_list, TRUE);
  gtk_widget_set_vexpand(scrolled_list, TRUE);
  gtk_paned_set_start_child(GTK_PANED(inner_paned), scrolled_list);
  ui->scrolled_list = scrolled_list;

  GtkWidget *listbox = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_list), listbox);
  ui->listbox = listbox;

  // --- Bottom: notebook with tabs ---
  GtkWidget *notebook = gtk_notebook_new();
  gtk_widget_set_hexpand(notebook, TRUE);
  gtk_widget_set_vexpand(notebook, TRUE);
  gtk_paned_set_end_child(GTK_PANED(inner_paned), notebook);
  ui->notebook = notebook;

  // --- Tab 1: Package Info ---
  GtkTextBuffer *details_buffer = NULL;
  GtkWidget *scrolled_details =
      create_scrolled_text_view(_("Select a package for details."), GTK_WRAP_WORD, &details_buffer);
  ui->details_buffer = details_buffer;

  GtkWidget *tab_label_info = gtk_label_new(_("Info"));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_details, tab_label_info);

  // --- Tab 2: File List ---
  GtkTextBuffer *files_buffer = NULL;
  GtkWidget *scrolled_files =
      create_scrolled_text_view(_("Select an installed package to view its file list."), GTK_WRAP_NONE, &files_buffer);
  ui->files_buffer = files_buffer;

  GtkWidget *tab_label_files = gtk_label_new(_("Files"));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_files, tab_label_files);

  // --- Tab 3: Dependencies ---
  GtkTextBuffer *deps_buffer = NULL;
  GtkWidget *scrolled_deps =
      create_scrolled_text_view(_("Select a package to view dependencies."), GTK_WRAP_WORD, &deps_buffer);
  ui->deps_buffer = deps_buffer;

  GtkWidget *tab_label_deps = gtk_label_new(_("Dependencies"));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_deps, tab_label_deps);

  // --- Tab 4: Changelog ---
  GtkTextBuffer *changelog_buffer = NULL;
  GtkWidget *scrolled_changelog =
      create_scrolled_text_view(_("Select a package to view its changelog."), GTK_WRAP_WORD, &changelog_buffer);
  ui->changelog_buffer = changelog_buffer;

  GtkWidget *tab_label_changelog = gtk_label_new(_("Changelog"));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_changelog, tab_label_changelog);

  // --- Tab 5: Pending actions ---
  GtkWidget *pending_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(pending_scrolled, TRUE);
  gtk_widget_set_vexpand(pending_scrolled, TRUE);

  GtkWidget *pending_list = gtk_list_box_new();
  // Pending rows act as direct action buttons and should not keep listbox selection state.
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(pending_list), GTK_SELECTION_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pending_scrolled), pending_list);
  ui->pending_list = pending_list;

  GtkWidget *tab_label_pending = gtk_label_new(_("Pending"));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), pending_scrolled, tab_label_pending);

  // --- Bottom bar with item count ---
  GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_hexpand(bottom_bar, TRUE);
  gtk_widget_add_css_class(bottom_bar, "bottom-bar");
  gtk_box_append(GTK_BOX(vbox_root), bottom_bar);

  GtkWidget *count_label = gtk_label_new(_("Items: 0"));
  gtk_label_set_xalign(GTK_LABEL(count_label), 0.0);
  gtk_widget_set_hexpand(count_label, TRUE);
  gtk_box_append(GTK_BOX(bottom_bar), count_label);
  ui->count_label = count_label;

  // Passive startup note shown only while the backend is warming up.
  GtkWidget *warmup_label = gtk_label_new(_("Loading package data..."));
  gtk_label_set_xalign(GTK_LABEL(warmup_label), 1.0);
  gtk_widget_set_visible(warmup_label, FALSE);
  gtk_box_append(GTK_BOX(bottom_bar), warmup_label);
  ui->warmup_label = warmup_label;
}

// -----------------------------------------------------------------------------
// Create shared SearchWidgets state from UI handles
// Kept alive by the main window cleanup data and running UI tasks
// -----------------------------------------------------------------------------
static std::shared_ptr<SearchWidgets>
create_search_widgets(const AppWidgets *ui)
{
  auto widgets = std::make_shared<SearchWidgets>();
  widgets->query.entry = GTK_ENTRY(ui->entry);
  widgets->query.history_list = GTK_LIST_BOX(ui->history_list);
  widgets->query.spinner = GTK_SPINNER(ui->spinner);
  widgets->query.search_button = GTK_BUTTON(ui->search_button);
  widgets->query.list_button = GTK_BUTTON(ui->list_button);
  widgets->query.list_available_button = GTK_BUTTON(ui->list_available_button);
  widgets->query.list_upgradeable_button = GTK_BUTTON(ui->list_upgradeable_button);
  widgets->query.status_label = GTK_LABEL(ui->status_label);
  widgets->query.desc_checkbox = GTK_CHECK_BUTTON(ui->desc_checkbox);
  widgets->query.exact_checkbox = GTK_CHECK_BUTTON(ui->exact_checkbox);

  widgets->results.listbox = GTK_LIST_BOX(ui->listbox);
  widgets->results.list_scroller = GTK_SCROLLED_WINDOW(ui->scrolled_list);
  widgets->results.inner_paned = GTK_PANED(ui->inner_paned);
  widgets->results.details_buffer = ui->details_buffer;
  widgets->results.files_buffer = ui->files_buffer;
  widgets->results.deps_buffer = ui->deps_buffer;
  widgets->results.changelog_buffer = ui->changelog_buffer;
  widgets->results.count_label = GTK_LABEL(ui->count_label);

  widgets->transaction.install_button = GTK_BUTTON(ui->install_button);
  widgets->transaction.remove_button = GTK_BUTTON(ui->remove_button);
  widgets->transaction.reinstall_button = GTK_BUTTON(ui->reinstall_button);
  widgets->transaction.upgrade_all_button = GTK_BUTTON(ui->upgrade_all_button);
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
  widgets->window_state.backend_warmup_cancellable = nullptr;

  return widgets;
}

// -----------------------------------------------------------------------------
// Setup GTK CSS styling
// -----------------------------------------------------------------------------
static void
setup_css(SearchWidgets *widgets)
{
  GtkCssProvider *css = gtk_css_provider_new();
  // The few custom style rules are kept next to the widgets they style.
  gtk_css_provider_load_from_string(css,
                                    "label.status-bar { padding: 4px; border-radius: 4px; } "
                                    ".bottom-bar { padding: 5px; border-top: 1px solid #666; } "
                                    ".package-status { "
                                    "  padding: 3px 10px; "
                                    "  border-radius: 999px; "
                                    "  border: 1px solid transparent; "
                                    "  font-weight: 800; "
                                    "} "
                                    ".package-status-available { "
                                    "  background-color: #cbd8e8; "
                                    "  border-color: #6f89a8; "
                                    "  color: #10263f; "
                                    "} "
                                    ".package-status-installed { "
                                    "  background-color: #cfe6c2; "
                                    "  border-color: #668f58; "
                                    "  color: #173915; "
                                    "} "
                                    ".package-status-local-only { "
                                    "  background-color: #d7ebea; "
                                    "  border-color: #4f8c86; "
                                    "  color: #0e3a39; "
                                    "} "
                                    ".package-status-upgradeable { "
                                    "  background-color: #f0ddb0; "
                                    "  border-color: #9f7a24; "
                                    "  color: #4a3200; "
                                    "} "
                                    ".package-status-installed-newer { "
                                    "  background-color: #f2d5cb; "
                                    "  border-color: #b0674c; "
                                    "  color: #4f1c0c; "
                                    "} "
                                    ".package-status-pending-install { "
                                    "  background-color: #2b64b5; "
                                    "  border-color: #163d74; "
                                    "  color: #ffffff; "
                                    "} "
                                    ".package-status-pending-reinstall { "
                                    "  background-color: #d89a19; "
                                    "  border-color: #8c5f00; "
                                    "  color: #2d1a00; "
                                    "} "
                                    ".package-status-pending-remove { "
                                    "  background-color: #bf4a33; "
                                    "  border-color: #7c281b; "
                                    "  color: #ffffff; "
                                    "} "
                                    ".package-meta { "
                                    "  opacity: 0.78; "
                                    "} "
                                    ".package-summary { "
                                    "  opacity: 0.92; "
                                    "} "
                                    ".thin-line { "
                                    "  background-color: @borders; "
                                    "  margin: 0; "
                                    "  padding: 0; "
                                    "  min-height: 1px; "
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
initialize_ui_state(SearchWidgets *widgets)
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
connect_signals(const AppWidgets *ui, SearchWidgets *widgets)
{
  MainMenuWidgets menu_widgets;
  menu_widgets.window = ui->window;
  menu_widgets.history_panel = ui->vbox_history;
  menu_widgets.info_panel = ui->notebook;
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

  g_signal_connect(ui->search_button, "clicked", G_CALLBACK(package_query_on_search_button_clicked), widgets);

  g_signal_connect(ui->entry, "activate", G_CALLBACK(package_query_on_search_button_clicked), widgets);
  g_signal_connect(ui->history_list, "row-selected", G_CALLBACK(package_query_on_history_row_selected), widgets);

  g_signal_connect(ui->apply_button, "clicked", G_CALLBACK(pending_transaction_on_apply_button_clicked), widgets);
  g_signal_connect(
      ui->clear_pending_button, "clicked", G_CALLBACK(pending_transaction_on_clear_pending_button_clicked), widgets);

  g_signal_connect(ui->refresh_button, "clicked", G_CALLBACK(widgets_on_refresh_button_clicked), widgets);

  // Intercept window close so unapplied marked changes can be confirmed first.
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
show_pending_quit_dialog(SearchWidgets *widgets)
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
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
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
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     widgets->window_state.pending_quit_dialog_open = false;
                   }),
                   widgets);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// Confirm before closing the app when there are unapplied pending changes.
// -----------------------------------------------------------------------------
static gboolean
on_main_window_close_request(GtkWindow *window, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  if (!widgets || widgets->window_state.allow_close_with_pending) {
    config_save_window_geometry(window);
    if (widgets && widgets->results.inner_paned) {
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
// Connect window destroy callback for SearchWidgets cleanup
// -----------------------------------------------------------------------------
static void
connect_cleanup(GtkWidget *window, std::shared_ptr<SearchWidgets> widgets, GCancellable *startup_cancellable)
{
  MainWindowCleanupData *cleanup_data = new MainWindowCleanupData();
  cleanup_data->widgets = std::move(widgets);
  cleanup_data->startup_cancellable = G_CANCELLABLE(g_object_ref(startup_cancellable));

  g_signal_connect(window,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer data) {
                     MainWindowCleanupData *cleanup_data = static_cast<MainWindowCleanupData *>(data);
                     SearchWidgets *widgets = cleanup_data ? cleanup_data->widgets.get() : nullptr;
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
                     if (!widgets->transaction.preview_transaction_path.empty()) {
                       transaction_service_client_release_request(widgets->transaction.preview_transaction_path);
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

  build_main_ui(&ui);

  std::shared_ptr<SearchWidgets> widgets = create_search_widgets(&ui);
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
