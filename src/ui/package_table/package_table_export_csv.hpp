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
// Append one already formatted header or data row to the CSV.
// -----------------------------------------------------------------------------
void package_table_export_csv_append_row(std::string &csv, const std::vector<std::string> &fields);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
