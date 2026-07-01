// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_export_csv.cpp
// Package table CSV formatting
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_export_csv.hpp"

namespace {

// -----------------------------------------------------------------------------
// Return text escaped for one CSV field.
// -----------------------------------------------------------------------------
std::string
csv_escape(const std::string &text)
{
  bool quote = false;
  for (char c : text) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      quote = true;
      break;
    }
  }

  if (!quote) {
    return text;
  }

  std::string escaped = "\"";
  for (char c : text) {
    if (c == '"') {
      escaped += "\"\"";
    } else {
      escaped += c;
    }
  }
  escaped += "\"";
  return escaped;
}

}

// -----------------------------------------------------------------------------
// Build CSV text from already formatted headers and rows.
// -----------------------------------------------------------------------------
std::string
package_table_export_csv_text(const std::vector<std::string> &headers,
                              const std::vector<std::vector<std::string>> &rows)
{
  std::string csv;
  auto append_line = [&csv](const std::vector<std::string> &fields) {
    for (size_t i = 0; i < fields.size(); ++i) {
      if (i > 0) {
        csv += ",";
      }
      csv += csv_escape(fields[i]);
    }
    csv += "\n";
  };

  append_line(headers);
  for (const auto &row : rows) {
    append_line(row);
  }

  return csv;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
