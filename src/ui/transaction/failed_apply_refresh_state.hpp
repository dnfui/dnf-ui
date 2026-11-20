// -----------------------------------------------------------------------------
// failed_apply_refresh_state.hpp
// State transitions for refreshing package data after a failed live Apply.
// -----------------------------------------------------------------------------
#pragma once

enum class FailedApplyRefreshState { IDLE, REBUILDING, WAITING_FOR_QUERY, WAITING_AFTER_FAILURE, RELOADING };

enum class FailedApplyRefreshAction { NONE, RELOAD, FINISH };

// -----------------------------------------------------------------------------
// Continue when the Base rebuild finishes.
// -----------------------------------------------------------------------------
FailedApplyRefreshAction
failed_apply_refresh_after_rebuild(FailedApplyRefreshState &state, bool success, bool query_active);
// -----------------------------------------------------------------------------
// Continue when a package query finishes.
// -----------------------------------------------------------------------------
FailedApplyRefreshAction failed_apply_refresh_after_query(FailedApplyRefreshState &state);
// -----------------------------------------------------------------------------
// Finish when reloading did not start a package query.
// -----------------------------------------------------------------------------
FailedApplyRefreshAction failed_apply_refresh_after_reload_start(FailedApplyRefreshState &state, bool query_active);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
