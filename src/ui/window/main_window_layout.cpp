// -----------------------------------------------------------------------------
// src/ui/window/main_window_layout.cpp
// Main window layout
//
// Builds the GTK widget tree for the main window.
// This file does not connect application behavior.
// Signal wiring stays in main_window.cpp.
// -----------------------------------------------------------------------------
#include "ui/window/main_window_layout.hpp"

#include "config.hpp"
#include "i18n.hpp"
#include "ui/window/main_menu.hpp"
#include "ui/common/ui_helpers.hpp"

// -----------------------------------------------------------------------------
// Create selectable read-only text view inside a scrolled window.
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
// Create one path label for a visible file list row.
// -----------------------------------------------------------------------------
static void
setup_file_list_item(GtkSignalListItemFactory *, GtkListItem *item, gpointer)
{
  GtkWidget *label = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_margin_start(label, 10);
  gtk_widget_set_margin_end(label, 10);
  gtk_list_item_set_child(item, label);
}

// -----------------------------------------------------------------------------
// Set a recycled file list row to its current path.
// -----------------------------------------------------------------------------
static void
bind_file_list_item(GtkSignalListItemFactory *, GtkListItem *item, gpointer)
{
  GtkStringObject *path = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
  GtkWidget *label = gtk_list_item_get_child(item);
  gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(path));
}

// -----------------------------------------------------------------------------
// Copy the selected path without copying the entire file list.
// -----------------------------------------------------------------------------
static void
copy_selected_file_path(GtkListView *view, guint position)
{
  GtkSingleSelection *selection = GTK_SINGLE_SELECTION(gtk_list_view_get_model(view));
  GListModel *model = gtk_single_selection_get_model(selection);
  GtkStringObject *item = GTK_STRING_OBJECT(g_list_model_get_item(model, position));
  if (!item) {
    return;
  }

  GdkClipboard *clipboard = gdk_display_get_clipboard(gtk_widget_get_display(GTK_WIDGET(view)));
  gdk_clipboard_set_text(clipboard, gtk_string_object_get_string(item));
  g_object_unref(item);
}

// -----------------------------------------------------------------------------
// Update the visible file paths when the filter text changes.
// -----------------------------------------------------------------------------
static void
on_file_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
  GtkStringFilter *filter = GTK_STRING_FILTER(user_data);
  gtk_string_filter_set_search(filter, gtk_editable_get_text(GTK_EDITABLE(entry)));
}

// -----------------------------------------------------------------------------
// Copy the selected file from the keyboard.
// -----------------------------------------------------------------------------
static gboolean
on_file_list_key_pressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer user_data)
{
  GdkModifierType modifiers = static_cast<GdkModifierType>(state & gtk_accelerator_get_default_mod_mask());
  if (modifiers != GDK_CONTROL_MASK || gdk_keyval_to_lower(keyval) != GDK_KEY_c) {
    return FALSE;
  }

  GtkListView *view = GTK_LIST_VIEW(user_data);
  GtkSingleSelection *selection = GTK_SINGLE_SELECTION(gtk_list_view_get_model(view));
  guint position = gtk_single_selection_get_selected(selection);
  if (position == GTK_INVALID_LIST_POSITION) {
    return FALSE;
  }

  copy_selected_file_path(view, position);
  return TRUE;
}

static void
append_button_group_separator(GtkWidget *box)
{
  GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(separator, "button-group-separator");
  gtk_widget_set_valign(separator, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(separator, 1, 24);
  gtk_widget_set_margin_start(separator, 6);
  gtk_widget_set_margin_end(separator, 6);
  gtk_box_append(GTK_BOX(box), separator);
}

// -----------------------------------------------------------------------------
// Build all GTK widgets and main application layout.
// -----------------------------------------------------------------------------
void
main_window_build_layout(AppWidgets *ui)
{
  GtkWidget *window = ui->window;

  GtkWidget *vbox_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_window_set_child(GTK_WINDOW(window), vbox_root);

  GtkWidget *menu_bar = main_menu_create();
  gtk_box_append(GTK_BOX(vbox_root), menu_bar);

  GtkWidget *controls_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(controls_box, "top-controls");
  gtk_box_append(GTK_BOX(vbox_root), controls_box);

  GtkWidget *outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(outer_paned), 200);

  GtkWidget *vbox_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_paned_set_end_child(GTK_PANED(outer_paned), vbox_main);

  GtkWidget *vbox_history = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_vexpand(vbox_history, TRUE);
  gtk_widget_set_hexpand(vbox_history, TRUE);
  gtk_widget_add_css_class(vbox_history, "history-sidebar");
  gtk_paned_set_start_child(GTK_PANED(outer_paned), vbox_history);
  ui->vbox_history = vbox_history;

  GtkWidget *history_label = gtk_label_new(_("Search History"));
  gtk_label_set_xalign(GTK_LABEL(history_label), 0.0);
  gtk_widget_add_css_class(history_label, "history-heading");
  gtk_box_append(GTK_BOX(vbox_history), history_label);

  GtkWidget *scrolled_history = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled_history, TRUE);
  gtk_widget_set_hexpand(scrolled_history, TRUE);
  gtk_box_append(GTK_BOX(vbox_history), scrolled_history);

  GtkWidget *history_list = gtk_list_box_new();
  gtk_widget_add_css_class(history_list, "history-list");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_history), history_list);
  ui->history_list = history_list;

  // Search controls.
  GtkWidget *hbox_search = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_add_css_class(hbox_search, "control-row");
  gtk_box_append(GTK_BOX(controls_box), hbox_search);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("Search packages..."));
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(hbox_search), entry);
  ui->entry = entry;

  GtkWidget *search_button = ui_helpers_create_icon_button("edit-find-symbolic", _("Search"));
  gtk_box_append(GTK_BOX(hbox_search), search_button);
  ui->search_button = search_button;

  append_button_group_separator(hbox_search);

  const char *search_modes[] = { _("Packages"), _("Provides"), nullptr };
  GtkWidget *mode_dropdown = gtk_drop_down_new_from_strings(search_modes);
  gtk_widget_set_tooltip_text(mode_dropdown, _("Search package names or find packages by file path and capability."));
  gtk_box_append(GTK_BOX(hbox_search), mode_dropdown);
  ui->mode_dropdown = mode_dropdown;

  // Keep package-only search options accessible without crowding the search row.
  GtkWidget *options_button = gtk_menu_button_new();
  GtkWidget *options_button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(options_button_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(options_button_box, GTK_ALIGN_CENTER);
  GtkWidget *options_icon = gtk_image_new_from_icon_name("system-search-symbolic");
  gtk_box_append(GTK_BOX(options_button_box), options_icon);
  GtkWidget *options_label = gtk_label_new(_("Options"));
  gtk_box_append(GTK_BOX(options_button_box), options_label);
  gtk_menu_button_set_child(GTK_MENU_BUTTON(options_button), options_button_box);
  gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(options_button), TRUE);

  GtkWidget *options_popover = gtk_popover_new();
  GtkWidget *options_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(options_box, 12);
  gtk_widget_set_margin_end(options_box, 12);
  gtk_widget_set_margin_top(options_box, 12);
  gtk_widget_set_margin_bottom(options_box, 12);

  GtkWidget *desc_checkbox = gtk_check_button_new_with_label(_("Search in description"));
  gtk_box_append(GTK_BOX(options_box), desc_checkbox);
  ui->desc_checkbox = desc_checkbox;

  GtkWidget *exact_checkbox = gtk_check_button_new_with_label(_("Exact match"));
  gtk_box_append(GTK_BOX(options_box), exact_checkbox);
  ui->exact_checkbox = exact_checkbox;

  gtk_popover_set_child(GTK_POPOVER(options_popover), options_box);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(options_button), options_popover);
  gtk_box_append(GTK_BOX(hbox_search), options_button);
  ui->options_button = options_button;

  GtkWidget *latest_checkbox = gtk_check_button_new_with_label(_("Latest only"));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(latest_checkbox), TRUE);
  gtk_box_append(GTK_BOX(hbox_search), latest_checkbox);
  ui->latest_checkbox = latest_checkbox;

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(hbox_search), spinner);
  ui->spinner = spinner;

  // Package query buttons.
  GtkWidget *hbox_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_add_css_class(hbox_buttons, "control-row");
  gtk_box_append(GTK_BOX(controls_box), hbox_buttons);

  GtkWidget *list_button = ui_helpers_create_icon_button_with_fallback(
      "drive-harddisk-system-symbolic", "view-list-symbolic", _("List Installed"));
  gtk_box_append(GTK_BOX(hbox_buttons), list_button);
  ui->list_button = list_button;

  GtkWidget *list_packages_button = ui_helpers_create_icon_button_with_fallback(
      "package-x-generic-symbolic", "view-list-symbolic", _("List Packages"));
  gtk_box_append(GTK_BOX(hbox_buttons), list_packages_button);
  ui->list_packages_button = list_packages_button;

  GtkWidget *list_upgradeable_button = ui_helpers_create_icon_button_with_fallback(
      "software-update-available-symbolic", "view-list-symbolic", _("List Upgradable"));
  gtk_box_append(GTK_BOX(hbox_buttons), list_upgradeable_button);
  ui->list_upgradeable_button = list_upgradeable_button;

  append_button_group_separator(hbox_buttons);

  GtkWidget *upgrade_all_button = ui_helpers_create_icon_button_with_fallback(
      "software-update-urgent-symbolic", "view-refresh-symbolic", _("Upgrade All"));
  gtk_box_append(GTK_BOX(hbox_buttons), upgrade_all_button);
  ui->upgrade_all_button = upgrade_all_button;

  // Starts repository refresh on a background task.
  append_button_group_separator(hbox_buttons);
  GtkWidget *refresh_button = ui_helpers_create_icon_button("view-refresh-symbolic", _("Refresh Repositories"));
  gtk_box_append(GTK_BOX(hbox_buttons), refresh_button);
  ui->refresh_button = refresh_button;

  // Transaction buttons row for marking and applying package actions.
  GtkWidget *hbox_tx_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_add_css_class(hbox_tx_buttons, "control-row");
  gtk_box_append(GTK_BOX(controls_box), hbox_tx_buttons);

  GtkWidget *install_button = ui_helpers_create_icon_button("list-add-symbolic", _("Mark for Install"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), install_button);
  ui->install_button = install_button;

  GtkWidget *reinstall_button = ui_helpers_create_icon_button_with_fallback(
      "document-revert-symbolic", "view-refresh-symbolic", _("Mark for Reinstall"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), reinstall_button);
  ui->reinstall_button = reinstall_button;

  GtkWidget *remove_button = ui_helpers_create_icon_button("user-trash-symbolic", _("Mark for Removal"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), remove_button);
  ui->remove_button = remove_button;

  GtkWidget *mark_listed_upgrades_button = ui_helpers_create_icon_button_with_fallback(
      "software-update-available-symbolic", "object-select-symbolic", _("Mark Listed Upgrades"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), mark_listed_upgrades_button);
  ui->mark_listed_upgrades_button = mark_listed_upgrades_button;

  append_button_group_separator(hbox_tx_buttons);

  GtkWidget *apply_button = ui_helpers_create_icon_button("system-run-symbolic", _("Apply Transactions"));
  gtk_widget_add_css_class(apply_button, "suggested-action");
  gtk_box_append(GTK_BOX(hbox_tx_buttons), apply_button);
  ui->apply_button = apply_button;

  GtkWidget *clear_pending_button = ui_helpers_create_icon_button("edit-clear-symbolic", _("Clear Transactions"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), clear_pending_button);
  ui->clear_pending_button = clear_pending_button;

  // Persistent cue for a ready offline transaction. The ordinary status label
  // below is transient and may be replaced by search or package-detail messages.
  GtkWidget *prepared_updates_banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(prepared_updates_banner, "prepared-updates-banner");
  gtk_widget_set_visible(prepared_updates_banner, FALSE);
  gtk_box_append(GTK_BOX(controls_box), prepared_updates_banner);
  ui->prepared_updates_banner = prepared_updates_banner;

  GtkWidget *prepared_updates_icon = gtk_image_new_from_icon_name("system-reboot-symbolic");
  gtk_widget_set_valign(prepared_updates_icon, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(prepared_updates_banner), prepared_updates_icon);

  GtkWidget *prepared_updates_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(prepared_updates_text, TRUE);
  gtk_box_append(GTK_BOX(prepared_updates_banner), prepared_updates_text);

  GtkWidget *prepared_updates_title = gtk_label_new(_("Updates Prepared for Reboot"));
  gtk_label_set_xalign(GTK_LABEL(prepared_updates_title), 0.0f);
  gtk_widget_add_css_class(prepared_updates_title, "prepared-updates-title");
  gtk_box_append(GTK_BOX(prepared_updates_text), prepared_updates_title);

  GtkWidget *prepared_updates_message = gtk_label_new(
      _("A package transaction is already prepared for reboot. Other package changes cannot be applied until it is "
        "installed or discarded."));
  gtk_label_set_xalign(GTK_LABEL(prepared_updates_message), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(prepared_updates_message), TRUE);
  gtk_box_append(GTK_BOX(prepared_updates_text), prepared_updates_message);

  GtkWidget *discard_prepared_button = gtk_button_new_with_label(_("Discard Prepared Updates"));
  gtk_widget_set_valign(discard_prepared_button, GTK_ALIGN_CENTER);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(discard_prepared_button), "win.discard-prepared-updates");
  gtk_box_append(GTK_BOX(prepared_updates_banner), discard_prepared_button);

  GtkWidget *status_label = gtk_label_new(_("Ready."));
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
  gtk_label_set_selectable(GTK_LABEL(status_label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
  gtk_box_append(GTK_BOX(controls_box), status_label);
  ui->status_label = status_label;

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

  // Package table.
  GtkWidget *scrolled_list = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_list, TRUE);
  gtk_widget_set_vexpand(scrolled_list, TRUE);
  gtk_paned_set_start_child(GTK_PANED(inner_paned), scrolled_list);
  ui->scrolled_list = scrolled_list;

  // Details panel.
  GtkWidget *details_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(details_panel, TRUE);
  gtk_widget_set_vexpand(details_panel, TRUE);
  gtk_paned_set_end_child(GTK_PANED(inner_paned), details_panel);
  ui->details_panel = details_panel;

  GtkWidget *details_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(details_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_hexpand(details_stack, TRUE);
  gtk_widget_set_vexpand(details_stack, TRUE);
  ui->details_stack = details_stack;

  GtkWidget *details_switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(details_switcher), GTK_STACK(details_stack));
  gtk_widget_add_css_class(details_switcher, "details-switcher");
  gtk_box_append(GTK_BOX(details_panel), details_switcher);
  gtk_box_append(GTK_BOX(details_panel), details_stack);

  // Package info tab.
  GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(info_box, TRUE);
  gtk_widget_set_vexpand(info_box, TRUE);

  GtkWidget *details_status_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(details_status_label), 0.0f);
  gtk_widget_set_margin_start(details_status_label, 8);
  gtk_widget_set_margin_end(details_status_label, 8);
  gtk_widget_set_margin_top(details_status_label, 6);
  gtk_widget_set_margin_bottom(details_status_label, 6);
  gtk_widget_set_visible(details_status_label, FALSE);
  ui->details_status_label = details_status_label;
  gtk_box_append(GTK_BOX(info_box), details_status_label);

  GtkTextBuffer *details_buffer = NULL;
  GtkWidget *scrolled_details =
      create_scrolled_text_view(_("Select a package for details."), GTK_WRAP_WORD, &details_buffer);
  ui->details_buffer = details_buffer;
  gtk_box_append(GTK_BOX(info_box), scrolled_details);

  gtk_stack_add_titled(GTK_STACK(details_stack), info_box, "info", _("Info"));

  // File list tab.
  GtkWidget *files_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(files_box, TRUE);
  gtk_widget_set_vexpand(files_box, TRUE);

  GtkWidget *files_search_entry = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(files_search_entry), _("Filter files..."));
  gtk_widget_set_hexpand(files_search_entry, TRUE);
  gtk_widget_set_margin_start(files_search_entry, 6);
  gtk_widget_set_margin_end(files_search_entry, 6);
  gtk_widget_set_margin_top(files_search_entry, 4);
  gtk_widget_set_margin_bottom(files_search_entry, 4);
  ui->files_search_entry = GTK_SEARCH_ENTRY(files_search_entry);

  GtkWidget *files_count_label = gtk_label_new(NULL);
  gtk_widget_set_margin_end(files_count_label, 6);
  gtk_widget_set_visible(files_count_label, FALSE);
  ui->files_count_label = GTK_LABEL(files_count_label);

  GtkWidget *files_search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(files_search_row), files_search_entry);
  gtk_box_append(GTK_BOX(files_search_row), files_count_label);
  gtk_box_append(GTK_BOX(files_box), files_search_row);

  GtkWidget *scrolled_files = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_files, TRUE);
  gtk_widget_set_vexpand(scrolled_files, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_files), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_append(GTK_BOX(files_box), scrolled_files);

  GtkStringList *files_model = gtk_string_list_new(nullptr);
  gtk_string_list_append(files_model, _("Select an installed package to view its file list."));
  ui->files_model = files_model;

  GtkExpression *path_expression = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
  GtkStringFilter *files_filter = gtk_string_filter_new(path_expression);
  gtk_string_filter_set_match_mode(files_filter, GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
  gtk_string_filter_set_ignore_case(files_filter, TRUE);
  g_signal_connect_object(
      files_search_entry, "search-changed", G_CALLBACK(on_file_search_changed), files_filter, G_CONNECT_DEFAULT);

  GtkFilterListModel *filtered_files = gtk_filter_list_model_new(G_LIST_MODEL(files_model), GTK_FILTER(files_filter));
  gtk_filter_list_model_set_incremental(filtered_files, TRUE);
  ui->filtered_files_model = filtered_files;

  GtkSingleSelection *files_selection = gtk_single_selection_new(G_LIST_MODEL(filtered_files));
  gtk_single_selection_set_autoselect(files_selection, FALSE);
  gtk_single_selection_set_can_unselect(files_selection, TRUE);

  GtkListItemFactory *files_factory = gtk_signal_list_item_factory_new();
  g_signal_connect(files_factory, "setup", G_CALLBACK(setup_file_list_item), nullptr);
  g_signal_connect(files_factory, "bind", G_CALLBACK(bind_file_list_item), nullptr);

  GtkWidget *files_view = gtk_list_view_new(GTK_SELECTION_MODEL(files_selection), files_factory);
  gtk_widget_set_hexpand(files_view, TRUE);
  gtk_widget_set_vexpand(files_view, TRUE);
  gtk_widget_set_focusable(files_view, TRUE);
  gtk_widget_set_margin_top(files_view, 4);
  gtk_widget_set_margin_bottom(files_view, 10);

  GtkEventController *copy_key = GTK_EVENT_CONTROLLER(gtk_event_controller_key_new());
  gtk_event_controller_set_propagation_phase(copy_key, GTK_PHASE_CAPTURE);
  g_signal_connect(copy_key, "key-pressed", G_CALLBACK(on_file_list_key_pressed), files_view);
  gtk_widget_add_controller(files_view, copy_key);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_files), files_view);

  gtk_stack_add_titled(GTK_STACK(details_stack), files_box, "files", _("Files"));

  // Dependencies tab.
  GtkTextBuffer *deps_buffer = NULL;
  GtkWidget *scrolled_deps =
      create_scrolled_text_view(_("Select a package to view dependencies."), GTK_WRAP_WORD, &deps_buffer);
  ui->deps_buffer = deps_buffer;

  gtk_stack_add_titled(GTK_STACK(details_stack), scrolled_deps, "dependencies", _("Dependencies"));

  // Changelog tab.
  GtkTextBuffer *changelog_buffer = NULL;
  GtkWidget *scrolled_changelog =
      create_scrolled_text_view(_("Select a package to view its changelog."), GTK_WRAP_WORD, &changelog_buffer);
  ui->changelog_buffer = changelog_buffer;

  gtk_stack_add_titled(GTK_STACK(details_stack), scrolled_changelog, "changelog", _("Changelog"));

  // Pending actions tab.
  GtkWidget *pending_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(pending_scrolled, TRUE);
  gtk_widget_set_vexpand(pending_scrolled, TRUE);

  GtkWidget *pending_list = gtk_list_box_new();
  // Pending rows act as direct action buttons and should not keep listbox selection state.
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(pending_list), GTK_SELECTION_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pending_scrolled), pending_list);
  ui->pending_list = pending_list;

  gtk_stack_add_titled(GTK_STACK(details_stack), pending_scrolled, "pending", _("Pending"));

  // Item count bar.
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

  GtkWidget *query_duration_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(query_duration_label), 1.0);
  gtk_label_set_ellipsize(GTK_LABEL(query_duration_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(query_duration_label), 60);
  gtk_widget_set_visible(query_duration_label, FALSE);
  gtk_box_append(GTK_BOX(bottom_bar), query_duration_label);
  ui->query_duration_label = query_duration_label;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
