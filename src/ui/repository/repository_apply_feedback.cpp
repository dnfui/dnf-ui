// -----------------------------------------------------------------------------
// repository_apply_feedback.cpp
// Repository Apply feedback independent of GTK widgets
// -----------------------------------------------------------------------------
#include "ui/repository/repository_apply_feedback.hpp"

#include "i18n.hpp"
#include "ui/repository/repository_apply_operation.hpp"

namespace {

// -----------------------------------------------------------------------------
// Return the main-window status text for a repository Apply backend sync.
// -----------------------------------------------------------------------------
const char *
repository_view_main_status_for_backend_sync(RepositoryBackendSyncResult sync_result, bool no_write_needed)
{
  if (no_write_needed) {
    switch (sync_result) {
    case RepositoryBackendSyncResult::LIVE_METADATA:
      return _("Repository state already matched the requested changes.");
    case RepositoryBackendSyncResult::CACHED_METADATA:
      return _("Repository state already matched the requested changes. Using cached repository metadata.");
    case RepositoryBackendSyncResult::INSTALLED_ONLY:
      return _("Repository state already matched the requested changes. Showing installed packages only.");
    case RepositoryBackendSyncResult::FAILED:
      return _("Repository changes may have changed package data. Reload packages before continuing.");
    }
  }

  switch (sync_result) {
  case RepositoryBackendSyncResult::LIVE_METADATA:
    return _("Repository changes applied. Package data refreshed.");
  case RepositoryBackendSyncResult::CACHED_METADATA:
    return _("Repository changes applied. Using cached repository metadata.");
  case RepositoryBackendSyncResult::INSTALLED_ONLY:
    return _("Repository changes applied. Showing installed packages only.");
  case RepositoryBackendSyncResult::FAILED:
    return _("Repository changes may have changed package data. Reload packages before continuing.");
  }

  return _("Repository changes may have changed package data. Reload packages before continuing.");
}

// -----------------------------------------------------------------------------
// Return the repository-window status text for a completed repository Apply.
// -----------------------------------------------------------------------------
const char *
repository_view_window_status_for_backend_sync(RepositoryBackendSyncResult sync_result, bool no_write_needed)
{
  if (no_write_needed) {
    switch (sync_result) {
    case RepositoryBackendSyncResult::LIVE_METADATA:
      return _("Repository state already matched the requested changes.");
    case RepositoryBackendSyncResult::CACHED_METADATA:
      return _("Repository state already matched the requested changes. Using cached repository metadata.");
    case RepositoryBackendSyncResult::INSTALLED_ONLY:
      return _("Repository state already matched the requested changes. Showing installed packages only.");
    case RepositoryBackendSyncResult::FAILED:
      return _("Failed to apply repository changes.");
    }
  }

  switch (sync_result) {
  case RepositoryBackendSyncResult::LIVE_METADATA:
    return _("Repository changes applied.");
  case RepositoryBackendSyncResult::CACHED_METADATA:
    return _("Repository changes applied. Using cached repository metadata.");
  case RepositoryBackendSyncResult::INSTALLED_ONLY:
    return _("Repository changes applied. Showing installed packages only.");
  case RepositoryBackendSyncResult::FAILED:
    return _("Failed to apply repository changes.");
  }

  return _("Failed to apply repository changes.");
}

// -----------------------------------------------------------------------------
// Return the main-window status text for an incomplete repository Apply.
// -----------------------------------------------------------------------------
const char *
repository_view_incomplete_main_status_for_backend_sync(RepositoryBackendSyncResult sync_result)
{
  switch (sync_result) {
  case RepositoryBackendSyncResult::LIVE_METADATA:
    return _("Repository changes may be incomplete. Package data refreshed.");
  case RepositoryBackendSyncResult::CACHED_METADATA:
    return _("Repository changes may be incomplete. Using cached repository metadata.");
  case RepositoryBackendSyncResult::INSTALLED_ONLY:
    return _("Repository changes may be incomplete. Showing installed packages only.");
  case RepositoryBackendSyncResult::FAILED:
    return _("Repository changes may have changed package data. Reload packages before continuing.");
  }

  return _("Repository changes may have changed package data. Reload packages before continuing.");
}

// -----------------------------------------------------------------------------
// Return true when the daemon write changed or may have changed repository state.
// -----------------------------------------------------------------------------
bool
repository_view_write_may_have_changed_state(RepositoryWriteOutcome outcome)
{
  return outcome == RepositoryWriteOutcome::SUCCEEDED || outcome == RepositoryWriteOutcome::PARTIAL;
}

// -----------------------------------------------------------------------------
// Return true when package data was rebuilt well enough to keep browsing.
// -----------------------------------------------------------------------------
bool
repository_view_backend_is_available(RepositoryBackendSyncResult sync_result)
{
  return sync_result != RepositoryBackendSyncResult::FAILED;
}

} // namespace

// -----------------------------------------------------------------------------
// Return the repository-window status for the completed Apply result.
// -----------------------------------------------------------------------------
const char *
repository_view_window_status_for_result(const RepositoryApplyResult &result)
{
  if (result.no_write_needed) {
    if (repository_view_backend_is_available(result.backend_sync_result)) {
      return repository_view_window_status_for_backend_sync(result.backend_sync_result, true);
    }
    return _("Repository state already matched the requested changes, but package data could not be reloaded.");
  }

  if (result.write_outcome == RepositoryWriteOutcome::NOT_ATTEMPTED ||
      result.write_outcome == RepositoryWriteOutcome::FAILED) {
    return _("Failed to apply repository changes.");
  }

  if (result.write_outcome == RepositoryWriteOutcome::PARTIAL) {
    if (result.verification_outcome == RepositoryVerificationOutcome::UNAVAILABLE) {
      return _("Some repository changes were applied, but final state could not be verified.");
    }
    return _("Some repository changes were applied.");
  }

  if (result.verification_outcome == RepositoryVerificationOutcome::UNAVAILABLE) {
    return _("Repository changes were sent, but final state could not be verified.");
  }

  if (result.verification_outcome == RepositoryVerificationOutcome::MISMATCH) {
    return _("Repository changes were sent, but final state did not match the request.");
  }

  if (!repository_view_backend_is_available(result.backend_sync_result)) {
    return _("Repository changes applied, but package data could not be reloaded.");
  }

  return repository_view_window_status_for_backend_sync(result.backend_sync_result, false);
}

// -----------------------------------------------------------------------------
// Return the main-window status for the completed Apply result.
// -----------------------------------------------------------------------------
const char *
repository_view_main_status_for_result(const RepositoryApplyResult &result)
{
  if (result.no_write_needed) {
    if (repository_view_backend_is_available(result.backend_sync_result)) {
      return repository_view_main_status_for_backend_sync(result.backend_sync_result, true);
    }
    return _("Repository state already matched the requested changes. Reload packages before continuing.");
  }

  if (!repository_view_write_may_have_changed_state(result.write_outcome)) {
    return repository_view_incomplete_main_status_for_backend_sync(result.backend_sync_result);
  }

  if (result.write_outcome == RepositoryWriteOutcome::PARTIAL) {
    return repository_view_incomplete_main_status_for_backend_sync(result.backend_sync_result);
  }

  if (result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED &&
      repository_view_backend_is_available(result.backend_sync_result)) {
    return repository_view_main_status_for_backend_sync(result.backend_sync_result, false);
  }

  if (result.write_outcome == RepositoryWriteOutcome::SUCCEEDED &&
      result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED &&
      !repository_view_backend_is_available(result.backend_sync_result)) {
    return _("Repository changes applied. Reload packages before continuing.");
  }

  return repository_view_incomplete_main_status_for_backend_sync(result.backend_sync_result);
}

// -----------------------------------------------------------------------------
// Return the main-window status color for the completed Apply result.
// -----------------------------------------------------------------------------
const char *
repository_view_main_status_color_for_result(const RepositoryApplyResult &result)
{
  if (result.write_outcome == RepositoryWriteOutcome::SUCCEEDED &&
      result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED &&
      result.backend_sync_result == RepositoryBackendSyncResult::LIVE_METADATA) {
    return "green";
  }

  if (result.no_write_needed && result.backend_sync_result == RepositoryBackendSyncResult::LIVE_METADATA) {
    return "green";
  }

  if (result.no_write_needed && repository_view_backend_is_available(result.backend_sync_result)) {
    return "blue";
  }

  if (result.write_outcome == RepositoryWriteOutcome::SUCCEEDED &&
      result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED &&
      repository_view_backend_is_available(result.backend_sync_result)) {
    return "blue";
  }

  return "red";
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
