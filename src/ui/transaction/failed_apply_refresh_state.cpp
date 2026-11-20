// -----------------------------------------------------------------------------
// failed_apply_refresh_state.cpp
// State transitions for refreshing package data after a failed live Apply.
// -----------------------------------------------------------------------------
#include "ui/transaction/failed_apply_refresh_state.hpp"

// -----------------------------------------------------------------------------
// Wait for an older query or start reloading after the Base rebuild ends.
// -----------------------------------------------------------------------------
FailedApplyRefreshAction
failed_apply_refresh_after_rebuild(FailedApplyRefreshState &state, bool success, bool query_active)
{
  if (state != FailedApplyRefreshState::REBUILDING) {
    return FailedApplyRefreshAction::NONE;
  }

  if (query_active) {
    state = success ? FailedApplyRefreshState::WAITING_FOR_QUERY : FailedApplyRefreshState::WAITING_AFTER_FAILURE;
    return FailedApplyRefreshAction::NONE;
  }

  if (success) {
    state = FailedApplyRefreshState::RELOADING;
    return FailedApplyRefreshAction::RELOAD;
  }

  state = FailedApplyRefreshState::IDLE;
  return FailedApplyRefreshAction::FINISH;
}

// -----------------------------------------------------------------------------
// Continue after an older query or the refreshed view finishes loading.
// -----------------------------------------------------------------------------
FailedApplyRefreshAction
failed_apply_refresh_after_query(FailedApplyRefreshState &state)
{
  switch (state) {
  case FailedApplyRefreshState::WAITING_FOR_QUERY:
    state = FailedApplyRefreshState::RELOADING;
    return FailedApplyRefreshAction::RELOAD;
  case FailedApplyRefreshState::WAITING_AFTER_FAILURE:
  case FailedApplyRefreshState::RELOADING:
    state = FailedApplyRefreshState::IDLE;
    return FailedApplyRefreshAction::FINISH;
  default:
    return FailedApplyRefreshAction::NONE;
  }
}

// -----------------------------------------------------------------------------
// Finish recovery if the current view did not start a query.
// -----------------------------------------------------------------------------
FailedApplyRefreshAction
failed_apply_refresh_after_reload_start(FailedApplyRefreshState &state, bool query_active)
{
  if (state != FailedApplyRefreshState::RELOADING || query_active) {
    return FailedApplyRefreshAction::NONE;
  }

  state = FailedApplyRefreshState::IDLE;
  return FailedApplyRefreshAction::FINISH;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
