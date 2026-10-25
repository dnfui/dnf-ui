// -----------------------------------------------------------------------------
// transaction_history_filters.cpp
// Text and local-date validation for transaction history filters
// -----------------------------------------------------------------------------
#include "ui/history/transaction_history_filters.hpp"

#include "i18n.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <limits>

// -----------------------------------------------------------------------------
// Remove simple ASCII whitespace from both ends of a filter field.
// -----------------------------------------------------------------------------
std::string
history_trim_filter_text(const char *text)
{
  std::string trimmed = text ? text : "";
  size_t start = 0;
  while (start < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[start]))) {
    ++start;
  }

  size_t end = trimmed.size();
  while (end > start && std::isspace(static_cast<unsigned char>(trimmed[end - 1]))) {
    --end;
  }

  return trimmed.substr(start, end - start);
}

namespace {

// -----------------------------------------------------------------------------
// Parse a local date filter in YYYY-MM-DD format.
// -----------------------------------------------------------------------------
bool
history_parse_date_filter(const char *text, bool end_of_day, int64_t &timestamp, std::string &error)
{
  std::string value = history_trim_filter_text(text);
  if (value.empty()) {
    timestamp = end_of_day ? std::numeric_limits<int64_t>::max() : 0;
    return true;
  }

  if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
    error = _("Dates must use YYYY-MM-DD.");
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  char extra = 0;
  if (std::sscanf(value.c_str(), "%d-%d-%d%c", &year, &month, &day, &extra) != 3) {
    error = _("Dates must use YYYY-MM-DD.");
    return false;
  }

  std::tm local {};
  local.tm_year = year - 1900;
  local.tm_mon = month - 1;
  local.tm_mday = day;
  local.tm_hour = end_of_day ? 23 : 0;
  local.tm_min = end_of_day ? 59 : 0;
  local.tm_sec = end_of_day ? 59 : 0;
  local.tm_isdst = -1;

  std::time_t parsed = std::mktime(&local);
  if (parsed == static_cast<std::time_t>(-1) || local.tm_year != year - 1900 || local.tm_mon != month - 1 ||
      local.tm_mday != day) {
    error = _("Dates must use YYYY-MM-DD.");
    return false;
  }

  timestamp = static_cast<int64_t>(parsed);
  return true;
}

} // namespace

// -----------------------------------------------------------------------------
// Parse both date bounds and check that the selected range is valid.
// -----------------------------------------------------------------------------
bool
history_parse_date_range(const char *from_text, const char *to_text, int64_t &from, int64_t &to, std::string &error)
{
  if (!history_parse_date_filter(from_text, false, from, error)) {
    return false;
  }

  if (!history_parse_date_filter(to_text, true, to, error)) {
    return false;
  }

  if (from > to) {
    error = _("The start date must be before the end date.");
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
