// -----------------------------------------------------------------------------
// repository_apply_feedback.hpp
// Repository Apply status messages
// -----------------------------------------------------------------------------
#pragma once

struct RepositoryApplyResult;

// -----------------------------------------------------------------------------
// Return the repository-window status for a completed repository Apply.
// -----------------------------------------------------------------------------
const char *repository_view_window_status_for_result(const RepositoryApplyResult &result);

// -----------------------------------------------------------------------------
// Return the main-window status for a completed repository Apply.
// -----------------------------------------------------------------------------
const char *repository_view_main_status_for_result(const RepositoryApplyResult &result);

// -----------------------------------------------------------------------------
// Return the main-window status color for a completed repository Apply.
// -----------------------------------------------------------------------------
const char *repository_view_main_status_color_for_result(const RepositoryApplyResult &result);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
