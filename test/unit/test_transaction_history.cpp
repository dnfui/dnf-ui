#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "ui/history/transaction_history_filters.hpp"

#include <ctime>
#include <limits>
#include <string>

// -----------------------------------------------------------------------------
// Verify that transaction history actions have user-facing labels.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction history actions have labels")
{
  REQUIRE(dnf_backend_transaction_history_action_to_string(TransactionHistoryAction::INSTALL) == "Install");
  REQUIRE(dnf_backend_transaction_history_action_to_string(TransactionHistoryAction::UPGRADE) == "Upgrade");
  REQUIRE(dnf_backend_transaction_history_action_to_string(TransactionHistoryAction::REMOVE) == "Remove");
}

// -----------------------------------------------------------------------------
// Verify that history cursors map between row offsets and one-based pages.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction history cursor maps pages to row offsets")
{
  TransactionHistoryCursor first_page = TransactionHistoryCursor::for_page(1, 100);
  TransactionHistoryCursor later_page = TransactionHistoryCursor::for_page(75, 100);

  REQUIRE(first_page.row_offset == 0);
  REQUIRE(first_page.page(100) == 1);
  REQUIRE(later_page.row_offset == 7400);
  REQUIRE(later_page.page(100) == 75);
}

// -----------------------------------------------------------------------------
// Verify that history filtering compares UTF-8 text case-insensitively.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction history filter uses UTF-8 case folding")
{
  REQUIRE(dnf_backend_history_text_matches_filter_for_tests("Ångström", "ång"));
  REQUIRE(dnf_backend_history_text_matches_filter_for_tests("MÜNCHEN", "mün"));
  REQUIRE_FALSE(dnf_backend_history_text_matches_filter_for_tests("Ångström", "mun"));
}

// -----------------------------------------------------------------------------
// Verify whitespace handling shared by the history filter entries.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction history filters trim surrounding text", "[history-filters]")
{
  REQUIRE(history_trim_filter_text("  firefox  ") == "firefox");
  REQUIRE(history_trim_filter_text("\t command text \r\n") == "command text");
  REQUIRE(history_trim_filter_text("  two words  ") == "two words");
  REQUIRE(history_trim_filter_text(" \t ") == "");
  REQUIRE(history_trim_filter_text(nullptr).empty());
}

// -----------------------------------------------------------------------------
// Empty date bounds must not restrict the history query.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction history dates accept optional bounds", "[history-filters]")
{
  int64_t from = -1;
  int64_t to = -1;
  std::string error;

  REQUIRE(history_parse_date_range(" \t ", "", from, to, error));
  REQUIRE(from == 0);
  REQUIRE(to == std::numeric_limits<int64_t>::max());
  REQUIRE(error.empty());

  REQUIRE(history_parse_date_range("2024-02-29", "", from, to, error));
  REQUIRE(from > 0);
  REQUIRE(to == std::numeric_limits<int64_t>::max());
  REQUIRE(error.empty());

  REQUIRE(history_parse_date_range("", "2024-02-29", from, to, error));
  REQUIRE(from == 0);
  REQUIRE(to < std::numeric_limits<int64_t>::max());
  REQUIRE(error.empty());
}

// -----------------------------------------------------------------------------
// A single selected date includes its first and last second in local time.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction history dates include the complete local day", "[history-filters]")
{
  int64_t from = 0;
  int64_t to = 0;
  std::string error;
  REQUIRE(history_parse_date_range(" 2024-02-29 ", "2024-02-29", from, to, error));
  REQUIRE(error.empty());
  REQUIRE(from < to);

  std::time_t from_time = static_cast<std::time_t>(from);
  std::time_t to_time = static_cast<std::time_t>(to);
  std::tm from_local {};
  std::tm to_local {};
  REQUIRE(localtime_r(&from_time, &from_local) != nullptr);
  REQUIRE(localtime_r(&to_time, &to_local) != nullptr);

  REQUIRE(from_local.tm_year == 2024 - 1900);
  REQUIRE(from_local.tm_mon == 1);
  REQUIRE(from_local.tm_mday == 29);
  REQUIRE(from_local.tm_hour == 0);
  REQUIRE(from_local.tm_min == 0);
  REQUIRE(from_local.tm_sec == 0);

  REQUIRE(to_local.tm_year == 2024 - 1900);
  REQUIRE(to_local.tm_mon == 1);
  REQUIRE(to_local.tm_mday == 29);
  REQUIRE(to_local.tm_hour == 23);
  REQUIRE(to_local.tm_min == 59);
  REQUIRE(to_local.tm_sec == 59);
}

// -----------------------------------------------------------------------------
// Keep malformed dates distinct from reversed but valid date ranges.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction history dates reject invalid ranges", "[history-filters]")
{
  int64_t from = 0;
  int64_t to = 0;
  std::string error;

  SECTION("invalid format")
  {
    REQUIRE_FALSE(history_parse_date_range("2024/02/29", "", from, to, error));
    REQUIRE(error == _("Dates must use YYYY-MM-DD."));
  }

  SECTION("bad month")
  {
    REQUIRE_FALSE(history_parse_date_range("", "2024-13-01", from, to, error));
    REQUIRE(error == _("Dates must use YYYY-MM-DD."));
  }

  SECTION("nonexistent day")
  {
    REQUIRE_FALSE(history_parse_date_range("2024-02-30", "", from, to, error));
    REQUIRE(error == _("Dates must use YYYY-MM-DD."));
  }

  SECTION("non-leap-year February 29")
  {
    REQUIRE_FALSE(history_parse_date_range("2023-02-29", "", from, to, error));
    REQUIRE(error == _("Dates must use YYYY-MM-DD."));
  }

  SECTION("start date after end date")
  {
    REQUIRE_FALSE(history_parse_date_range("2024-03-01", "2024-02-29", from, to, error));
    REQUIRE(error == _("The start date must be before the end date."));
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
