// -----------------------------------------------------------------------------
// transaction_history_filters.hpp
// Text and local-date validation for transaction history filters
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

// -----------------------------------------------------------------------------
// Trim surrounding whitespace from package and detail text filters.
// -----------------------------------------------------------------------------
std::string history_trim_filter_text(const char *text);

// -----------------------------------------------------------------------------
// Parse an optional local date range (YYYY-MM-DD) with an inclusive end day.
// Empty bounds mean no limit. On failure, error contains the user-facing message.
// -----------------------------------------------------------------------------
bool
history_parse_date_range(const char *from_text, const char *to_text, int64_t &from, int64_t &to, std::string &error);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
