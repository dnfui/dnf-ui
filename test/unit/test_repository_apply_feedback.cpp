// -----------------------------------------------------------------------------
// test_repository_apply_feedback.cpp
// Repository Apply status messages without GTK windows or daemon calls
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "i18n.hpp"
#include "ui/repository/repository_apply_feedback.hpp"
#include "ui/repository/repository_apply_operation.hpp"

#include <string>

// -----------------------------------------------------------------------------
// Confirm success feedback distinguishes the available backend states.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply feedback describes successful writes", "[repository-apply]")
{
  RepositoryApplyResult result;
  result.write_outcome = RepositoryWriteOutcome::SUCCEEDED;
  result.verification_outcome = RepositoryVerificationOutcome::CONFIRMED;

  SECTION("live metadata")
  {
    result.backend_sync_result = RepositoryBackendSyncResult::LIVE_METADATA;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) == _("Repository changes applied."));
    REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
            _("Repository changes applied. Package data refreshed."));
    REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "green");
  }

  SECTION("cached metadata")
  {
    result.backend_sync_result = RepositoryBackendSyncResult::CACHED_METADATA;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Repository changes applied. Using cached repository metadata."));
    REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
            _("Repository changes applied. Using cached repository metadata."));
    REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "blue");
  }

  SECTION("installed packages only")
  {
    result.backend_sync_result = RepositoryBackendSyncResult::INSTALLED_ONLY;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Repository changes applied. Showing installed packages only."));
    REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
            _("Repository changes applied. Showing installed packages only."));
    REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "blue");
  }

  SECTION("failed backend rebuild")
  {
    result.backend_sync_result = RepositoryBackendSyncResult::FAILED;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Repository changes applied, but package data could not be reloaded."));
    REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
            _("Repository changes applied. Reload packages before continuing."));
    REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "red");
  }
}

// -----------------------------------------------------------------------------
// Confirm matching repository state still reports backend rebuild results.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply feedback describes changes already applied", "[repository-apply]")
{
  RepositoryApplyResult result;
  result.no_write_needed = true;
  result.write_outcome = RepositoryWriteOutcome::NOT_ATTEMPTED;

  SECTION("live metadata")
  {
    result.backend_sync_result = RepositoryBackendSyncResult::LIVE_METADATA;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Repository state already matched the requested changes."));
    REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
            _("Repository state already matched the requested changes."));
    REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "green");
  }

  SECTION("cached metadata")
  {
    result.backend_sync_result = RepositoryBackendSyncResult::CACHED_METADATA;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Repository state already matched the requested changes. Using cached repository metadata."));
    REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
            _("Repository state already matched the requested changes. Using cached repository metadata."));
    REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "blue");
  }

  SECTION("installed packages only")
  {
    result.backend_sync_result = RepositoryBackendSyncResult::INSTALLED_ONLY;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Repository state already matched the requested changes. Showing installed packages only."));
    REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
            _("Repository state already matched the requested changes. Showing installed packages only."));
    REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "blue");
  }

  SECTION("failed backend rebuild")
  {
    result.backend_sync_result = RepositoryBackendSyncResult::FAILED;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Repository state already matched the requested changes, but package data could not be reloaded."));
    REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
            _("Repository state already matched the requested changes. Reload packages before continuing."));
    REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "red");
  }
}

// -----------------------------------------------------------------------------
// Confirm failed and partial writes cannot be reported as complete.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply feedback preserves incomplete write status", "[repository-apply]")
{
  RepositoryApplyResult result;
  result.backend_sync_result = RepositoryBackendSyncResult::LIVE_METADATA;

  SECTION("write not attempted")
  {
    result.write_outcome = RepositoryWriteOutcome::NOT_ATTEMPTED;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) == _("Failed to apply repository changes."));
  }

  SECTION("write failed")
  {
    result.write_outcome = RepositoryWriteOutcome::FAILED;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) == _("Failed to apply repository changes."));
  }

  SECTION("partial write with unverified state")
  {
    result.write_outcome = RepositoryWriteOutcome::PARTIAL;
    result.verification_outcome = RepositoryVerificationOutcome::UNAVAILABLE;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Some repository changes were applied, but final state could not be verified."));
  }

  SECTION("partial write with verified state")
  {
    result.write_outcome = RepositoryWriteOutcome::PARTIAL;
    result.verification_outcome = RepositoryVerificationOutcome::CONFIRMED;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Some repository changes were applied."));
  }

  REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
          _("Repository changes may be incomplete. Package data refreshed."));
  REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "red");
}

// -----------------------------------------------------------------------------
// Confirm that successful D-Bus writes need verified final repository state.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply feedback distinguishes verification failures", "[repository-apply]")
{
  RepositoryApplyResult result;
  result.write_outcome = RepositoryWriteOutcome::SUCCEEDED;
  result.backend_sync_result = RepositoryBackendSyncResult::LIVE_METADATA;

  SECTION("final state unavailable")
  {
    result.verification_outcome = RepositoryVerificationOutcome::UNAVAILABLE;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Repository changes were sent, but final state could not be verified."));
  }

  SECTION("final state mismatch")
  {
    result.verification_outcome = RepositoryVerificationOutcome::MISMATCH;
    REQUIRE(std::string(repository_view_window_status_for_result(result)) ==
            _("Repository changes were sent, but final state did not match the request."));
  }

  REQUIRE(std::string(repository_view_main_status_for_result(result)) ==
          _("Repository changes may be incomplete. Package data refreshed."));
  REQUIRE(std::string(repository_view_main_status_color_for_result(result)) == "red");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
