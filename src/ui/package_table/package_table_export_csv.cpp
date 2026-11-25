// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_export_csv.cpp
// Package table CSV formatting
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_export_csv.hpp"

namespace {

// -----------------------------------------------------------------------------
// Return true when one character can start a spreadsheet formula.
// -----------------------------------------------------------------------------
bool
csv_formula_marker(char c)
{
  return c == '=' || c == '+' || c == '-' || c == '@';
}

// -----------------------------------------------------------------------------
// Prefix fields that spreadsheet applications may interpret as formulas.
// -----------------------------------------------------------------------------
std::string
csv_protect_formula(const std::string &text)
{
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      continue;
    }

    if (csv_formula_marker(c)) {
      return "'" + text;
    }

    break;
  }

  return text;
}

// -----------------------------------------------------------------------------
// Return text escaped for one CSV field.
// -----------------------------------------------------------------------------
std::string
csv_escape(const std::string &text)
{
  std::string safe_text = csv_protect_formula(text);
  bool quote = false;
  for (char c : safe_text) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      quote = true;
      break;
    }
  }

  if (!quote) {
    return safe_text;
  }

  std::string escaped = "\"";
  for (char c : safe_text) {
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
// Append one already formatted header or data row to the CSV.
// -----------------------------------------------------------------------------
void
package_table_export_csv_append_row(std::string &csv, const std::vector<std::string> &fields)
{
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) {
      csv += ",";
    }
    csv += csv_escape(fields[i]);
  }
  csv += "\n";
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
