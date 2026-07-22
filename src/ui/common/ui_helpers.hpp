// -----------------------------------------------------------------------------
// src/ui/common/ui_helpers.hpp
// Shared UI helper functions
//
// Provides small helpers for common labels, status messages, and action buttons.
// -----------------------------------------------------------------------------
#pragma once

#include <string>

#include <gtk/gtk.h>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Create a button with an icon and text label.
// -----------------------------------------------------------------------------
GtkWidget *ui_helpers_create_icon_button(const char *icon_name, const char *label);
// -----------------------------------------------------------------------------
// Update the icon and text label for an icon button.
// -----------------------------------------------------------------------------
void ui_helpers_set_icon_button(GtkButton *button, const char *icon_name, const char *label);
// -----------------------------------------------------------------------------
// Set the status label text and background color.
// -----------------------------------------------------------------------------
void ui_helpers_set_status(GtkLabel *label, const std::string &text, const std::string &color);
// -----------------------------------------------------------------------------
// Hide a timing label.
// -----------------------------------------------------------------------------
void ui_helpers_clear_duration_label(GtkLabel *label);
// -----------------------------------------------------------------------------
// Show elapsed time in a timing label.
// -----------------------------------------------------------------------------
void
ui_helpers_show_duration_label(GtkLabel *label, const char *title, const char *fallback_title, gint64 started_at_us);
// Update action button labels when the visible row and installed row differ.
// -----------------------------------------------------------------------------
void ui_helpers_update_action_button_labels_for_selection(MainWindowUiState *widgets,
                                                          const std::string &install_nevra,
                                                          const std::string &remove_nevra,
                                                          const std::string &reinstall_nevra,
                                                          bool install_is_upgrade);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
