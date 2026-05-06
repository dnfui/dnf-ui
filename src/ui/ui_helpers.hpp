// -----------------------------------------------------------------------------
// src/ui/ui_helpers.hpp
// Shared UI helper functions
//
// Provides small helpers for action buttons and status messages.
// -----------------------------------------------------------------------------
#pragma once

#include <string>

#include <gtk/gtk.h>

struct SearchWidgets;

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
// Update action button labels for the selected package.
// -----------------------------------------------------------------------------
void ui_helpers_update_action_button_labels(SearchWidgets *widgets, const std::string &pkg);
// -----------------------------------------------------------------------------
// Update action button labels when the visible row and installed row differ.
// -----------------------------------------------------------------------------
void ui_helpers_update_action_button_labels_for_selection(SearchWidgets *widgets,
                                                          const std::string &install_nevra,
                                                          const std::string &remove_nevra,
                                                          const std::string &reinstall_nevra,
                                                          bool install_is_upgrade);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
