// -----------------------------------------------------------------------------
// Failed Apply refresh state tests
// Covers rebuild and query completion in either order.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "ui/transaction/failed_apply_refresh_state.hpp"

// -----------------------------------------------------------------------------
// Wait for a cancelled query before reloading the rebuilt package view.
// -----------------------------------------------------------------------------
TEST_CASE("Failed Apply refresh waits for an older package query")
{
  FailedApplyRefreshState state = FailedApplyRefreshState::REBUILDING;

  REQUIRE(failed_apply_refresh_after_rebuild(state, true, true) == FailedApplyRefreshAction::NONE);
  REQUIRE(state == FailedApplyRefreshState::WAITING_FOR_QUERY);
  REQUIRE(failed_apply_refresh_after_query(state) == FailedApplyRefreshAction::RELOAD);
  REQUIRE(state == FailedApplyRefreshState::RELOADING);
  REQUIRE(failed_apply_refresh_after_reload_start(state, true) == FailedApplyRefreshAction::NONE);
  REQUIRE(failed_apply_refresh_after_query(state) == FailedApplyRefreshAction::FINISH);
  REQUIRE(state == FailedApplyRefreshState::IDLE);
}

// -----------------------------------------------------------------------------
// Reload immediately when the older query finishes before the rebuild.
// -----------------------------------------------------------------------------
TEST_CASE("Failed Apply refresh reloads after a completed rebuild")
{
  FailedApplyRefreshState state = FailedApplyRefreshState::REBUILDING;

  REQUIRE(failed_apply_refresh_after_query(state) == FailedApplyRefreshAction::NONE);
  REQUIRE(failed_apply_refresh_after_rebuild(state, true, false) == FailedApplyRefreshAction::RELOAD);
  REQUIRE(state == FailedApplyRefreshState::RELOADING);
  REQUIRE(failed_apply_refresh_after_reload_start(state, false) == FailedApplyRefreshAction::FINISH);
  REQUIRE(state == FailedApplyRefreshState::IDLE);
}

// -----------------------------------------------------------------------------
// Wait for a cancelled query before finishing after rebuild failure.
// -----------------------------------------------------------------------------
TEST_CASE("Failed Apply refresh waits after rebuild failure")
{
  FailedApplyRefreshState state = FailedApplyRefreshState::REBUILDING;

  REQUIRE(failed_apply_refresh_after_rebuild(state, false, true) == FailedApplyRefreshAction::NONE);
  REQUIRE(state == FailedApplyRefreshState::WAITING_AFTER_FAILURE);
  REQUIRE(failed_apply_refresh_after_query(state) == FailedApplyRefreshAction::FINISH);
  REQUIRE(state == FailedApplyRefreshState::IDLE);
}

// -----------------------------------------------------------------------------
// Finish immediately when the rebuild fails without an older query.
// -----------------------------------------------------------------------------
TEST_CASE("Failed Apply refresh finishes after rebuild failure")
{
  FailedApplyRefreshState state = FailedApplyRefreshState::REBUILDING;

  REQUIRE(failed_apply_refresh_after_rebuild(state, false, false) == FailedApplyRefreshAction::FINISH);
  REQUIRE(state == FailedApplyRefreshState::IDLE);
  REQUIRE(failed_apply_refresh_after_query(state) == FailedApplyRefreshAction::NONE);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
