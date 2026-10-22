// -----------------------------------------------------------------------------
// i18n.hpp
// -----------------------------------------------------------------------------
#pragma once

#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "dnf-ui"
#endif

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif

#include <glib/gi18n.h>

#include <string>

// -----------------------------------------------------------------------------
// Initialize gettext for DNF UI.
// -----------------------------------------------------------------------------
void dnfui_i18n_init(void);

// -----------------------------------------------------------------------------
// Format translated text that contains printf placeholders.
// -----------------------------------------------------------------------------
std::string dnfui_i18n_format(const char *format, ...) G_GNUC_PRINTF(1, 2);

// -----------------------------------------------------------------------------
// Format translated singular or plural text that contains one count placeholder.
// -----------------------------------------------------------------------------
std::string dnfui_i18n_format_count(size_t count, const char *singular, const char *plural);

// -----------------------------------------------------------------------------
// Format translated singular or plural text that contains two count placeholders.
// The second count selects the plural form.
// -----------------------------------------------------------------------------
std::string dnfui_i18n_format_two_counts(size_t first, size_t count, const char *singular, const char *plural);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
