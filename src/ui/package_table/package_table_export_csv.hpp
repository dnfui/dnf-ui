// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_export_csv.hpp
// Package table CSV formatting
//
// Converts already formatted package table values into CSV text.
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Build CSV text from already formatted headers and rows.
// -----------------------------------------------------------------------------
std::string package_table_export_csv_text(const std::vector<std::string> &headers,
                                          const std::vector<std::vector<std::string>> &rows);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
