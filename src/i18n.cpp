// -----------------------------------------------------------------------------
// i18n.cpp
// -----------------------------------------------------------------------------
#include "i18n.hpp"

#include <cstdarg>
#include <clocale>

// -----------------------------------------------------------------------------
// Format a runtime-selected gettext string.
// Public callers should use dnfui_i18n_format() so literal call sites stay compiler checked.
// -----------------------------------------------------------------------------
static std::string
dnfui_i18n_format_unchecked(const char *format, ...)
{
  if (!format) {
    return {};
  }

  va_list args;
  va_start(args, format);
  char *text = g_strdup_vprintf(format, args);
  va_end(args);

  std::string result = text ? text : "";
  g_free(text);
  return result;
}

// -----------------------------------------------------------------------------
// Initialize gettext for DNF UI.
// -----------------------------------------------------------------------------
void
dnfui_i18n_init(void)
{
  std::setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);
}

// -----------------------------------------------------------------------------
// Format translated text that contains printf placeholders.
// -----------------------------------------------------------------------------
std::string
dnfui_i18n_format(const char *format, ...)
{
  if (!format) {
    return {};
  }

  va_list args;
  va_start(args, format);
  char *text = g_strdup_vprintf(format, args);
  va_end(args);

  std::string result = text ? text : "";
  g_free(text);
  return result;
}

// -----------------------------------------------------------------------------
// Format translated singular or plural text that contains one count placeholder.
// -----------------------------------------------------------------------------
std::string
dnfui_i18n_format_count(size_t count, const char *singular, const char *plural)
{
  return dnfui_i18n_format_unchecked(g_dngettext(GETTEXT_PACKAGE, singular, plural, count), count);
}

// -----------------------------------------------------------------------------
// Format translated singular or plural text that contains two count placeholders.
// -----------------------------------------------------------------------------
std::string
dnfui_i18n_format_two_counts(size_t first, size_t count, const char *singular, const char *plural)
{
  return dnfui_i18n_format_unchecked(g_dngettext(GETTEXT_PACKAGE, singular, plural, count), first, count);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
