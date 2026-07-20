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

  // Package query buttons.
  GtkWidget *hbox_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_add_css_class(hbox_buttons, "control-row");
  gtk_box_append(GTK_BOX(controls_box), hbox_buttons);

  GtkWidget *list_button = ui_helpers_create_icon_button("view-list-symbolic", _("List Installed"));
  gtk_box_append(GTK_BOX(hbox_buttons), list_button);
  ui->list_button = list_button;

  GtkWidget *list_available_button = ui_helpers_create_icon_button("view-list-symbolic", _("List Packages"));
  gtk_box_append(GTK_BOX(hbox_buttons), list_available_button);
  ui->list_available_button = list_available_button;

  GtkWidget *list_upgradeable_button = ui_helpers_create_icon_button("view-list-symbolic", _("List Upgradable"));
  gtk_box_append(GTK_BOX(hbox_buttons), list_upgradeable_button);
  ui->list_upgradeable_button = list_upgradeable_button;

  GtkWidget *upgrade_all_button = ui_helpers_create_icon_button("view-refresh-symbolic", _("Upgrade All"));
  gtk_box_append(GTK_BOX(hbox_buttons), upgrade_all_button);
  ui->upgrade_all_button = upgrade_all_button;

  // Starts repository refresh on a background task.
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

  GtkWidget *reinstall_button = ui_helpers_create_icon_button("view-refresh-symbolic", _("Mark for Reinstall"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), reinstall_button);
  ui->reinstall_button = reinstall_button;

  GtkWidget *remove_button = ui_helpers_create_icon_button("list-remove-symbolic", _("Mark for Removal"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), remove_button);
  ui->remove_button = remove_button;

  GtkWidget *mark_listed_upgrades_button =
      ui_helpers_create_icon_button("object-select-symbolic", _("Mark Listed Upgrades"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), mark_listed_upgrades_button);
  ui->mark_listed_upgrades_button = mark_listed_upgrades_button;

  GtkWidget *apply_button = ui_helpers_create_icon_button("object-select-symbolic", _("Apply Transactions"));
  gtk_widget_add_css_class(apply_button, "suggested-action");
  gtk_box_append(GTK_BOX(hbox_tx_buttons), apply_button);
  ui->apply_button = apply_button;

  GtkWidget *clear_pending_button = ui_helpers_create_icon_button("edit-clear-symbolic", _("Clear Transactions"));
  gtk_box_append(GTK_BOX(hbox_tx_buttons), clear_pending_button);
  ui->clear_pending_button = clear_pending_button;

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
  GtkTextBuffer *details_buffer = NULL;
  GtkWidget *scrolled_details =
      create_scrolled_text_view(_("Select a package for details."), GTK_WRAP_WORD, &details_buffer);
  ui->details_buffer = details_buffer;

  gtk_stack_add_titled(GTK_STACK(details_stack), scrolled_details, "info", _("Info"));

  // File list tab.
  GtkTextBuffer *files_buffer = NULL;
  GtkWidget *scrolled_files =
      create_scrolled_text_view(_("Select an installed package to view its file list."), GTK_WRAP_NONE, &files_buffer);
  ui->files_buffer = files_buffer;

  gtk_stack_add_titled(GTK_STACK(details_stack), scrolled_files, "files", _("Files"));

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
