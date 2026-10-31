// -----------------------------------------------------------------------------
// test_repository_apply_operation.cpp
// Repository Apply workflow tests without GTK or system changes
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "ui/repository/repository_apply_operation.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct RepositoryApplyFixture {
  std::vector<RepositoryInfo> initial_repositories {
    { "fedora", "Fedora", true },
    { "updates", "Fedora Updates", false },
    { "testing", "Fedora Testing", true },
  };
  std::vector<RepositoryInfo> final_repositories {
    { "fedora", "Fedora", true },
    { "updates", "Fedora Updates", true },
    { "testing", "Fedora Testing", false },
  };
  RepositoryWriteResult write_result {
    .enable_attempted = true,
    .enable_succeeded = true,
    .disable_attempted = true,
    .disable_succeeded = true,
    .error = {},
  };
  BaseRepoState backend_state = BaseRepoState::LIVE_METADATA;
  bool initial_list_succeeds = true;
  bool final_list_succeeds = true;
  std::string list_error;
  std::string rebuild_error;
  unsigned int list_calls = 0;
  std::vector<std::string> calls;
  std::vector<std::string> written_enable_ids;
  std::vector<std::string> written_disable_ids;

  RepositoryApplyDependencies dependencies()
  {
    return {
      .list_repositories =
          [this](std::vector<RepositoryInfo> &repositories, std::string &error) {
            calls.push_back("list");
            ++list_calls;
            bool initial_read = list_calls == 1;
            bool succeeds = initial_read ? initial_list_succeeds : final_list_succeeds;
            if (!succeeds) {
              error = list_error;
              return false;
            }
            repositories = initial_read ? initial_repositories : final_repositories;
            return true;
          },
      .apply_changes =
          [this](const std::vector<std::string> &enable_ids, const std::vector<std::string> &disable_ids) {
            calls.push_back("write");
            written_enable_ids = enable_ids;
            written_disable_ids = disable_ids;
            return write_result;
          },
      .rebuild_backend =
          [this]() {
            calls.push_back("rebuild");
            if (!rebuild_error.empty()) {
              throw std::runtime_error(rebuild_error);
            }
            return backend_state;
          },
    };
  }

  RepositoryApplyResult run()
  {
    return repository_apply_run({ "fedora", "updates" }, { "testing" }, dependencies());
  }
};

} // namespace

// -----------------------------------------------------------------------------
// A failed initial read must not write repositories or discard pending choices.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply stops when the initial state cannot be loaded", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.initial_list_succeeds = false;

  SECTION("daemon error")
  {
    fixture.list_error = "Daemon unavailable";
  }
  SECTION("missing error text")
  {
  }

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.calls == std::vector<std::string> { "list" });
  REQUIRE(result.write_outcome == RepositoryWriteOutcome::NOT_ATTEMPTED);
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::NOT_RUN);
  REQUIRE_FALSE(result.repository_state_loaded);
  REQUIRE_FALSE(result.sync_needed);
  REQUIRE_FALSE(result.clear_pending);
  REQUIRE_FALSE(result.error.empty());
  if (!fixture.list_error.empty()) {
    REQUIRE(result.error == fixture.list_error);
  }
}

// -----------------------------------------------------------------------------
// Changed configuration invalidates pending choices and refreshes package data without writing.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply rejects disappeared repositories before writing", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.initial_repositories.pop_back();

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.calls == std::vector<std::string> { "list", "rebuild" });
  REQUIRE(result.write_outcome == RepositoryWriteOutcome::NOT_ATTEMPTED);
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::NOT_RUN);
  REQUIRE(result.backend_sync_result == RepositoryBackendSyncResult::LIVE_METADATA);
  REQUIRE(result.repository_state_loaded);
  REQUIRE(result.repositories.size() == 2);
  REQUIRE(result.sync_needed);
  REQUIRE(result.clear_pending);
  REQUIRE_FALSE(result.no_write_needed);
  REQUIRE_FALSE(result.error.empty());
}

// -----------------------------------------------------------------------------
// Matching daemon state still requires local package data to be rebuilt.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply refreshes package data when no write is needed", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.initial_repositories = fixture.final_repositories;

  SECTION("cached metadata")
  {
    fixture.backend_state = BaseRepoState::CACHED_METADATA;
  }
  SECTION("failed rebuild")
  {
    fixture.rebuild_error = "Backend rebuild failed";
  }

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.calls == std::vector<std::string> { "list", "rebuild" });
  REQUIRE(result.no_write_needed);
  REQUIRE(result.sync_needed);
  REQUIRE(result.clear_pending);
  REQUIRE(result.repository_state_loaded);
  REQUIRE(result.write_outcome == RepositoryWriteOutcome::NOT_ATTEMPTED);
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::NOT_RUN);
  REQUIRE(result.error == fixture.rebuild_error);
  if (fixture.rebuild_error.empty()) {
    REQUIRE(result.backend_sync_result == RepositoryBackendSyncResult::CACHED_METADATA);
  } else {
    REQUIRE(result.backend_sync_result == RepositoryBackendSyncResult::FAILED);
  }
}

// -----------------------------------------------------------------------------
// Send only needed changes, then rebuild and verify through a fresh repository read.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply writes needed changes and confirms the final state", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.backend_state = BaseRepoState::INSTALLED_ONLY;

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.calls == std::vector<std::string> { "list", "write", "rebuild", "list" });
  REQUIRE(fixture.written_enable_ids == std::vector<std::string> { "updates" });
  REQUIRE(fixture.written_disable_ids == std::vector<std::string> { "testing" });
  REQUIRE(result.write_outcome == RepositoryWriteOutcome::SUCCEEDED);
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED);
  REQUIRE(result.backend_sync_result == RepositoryBackendSyncResult::INSTALLED_ONLY);
  REQUIRE(result.repository_state_loaded);
  REQUIRE(result.repositories.at(1).enabled);
  REQUIRE_FALSE(result.repositories.at(2).enabled);
  REQUIRE(result.sync_needed);
  REQUIRE(result.clear_pending);
  REQUIRE_FALSE(result.no_write_needed);
  REQUIRE(result.error.empty());
}

// -----------------------------------------------------------------------------
// No submitted write means the pending choices can remain available for retry.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply preserves pending changes when no write was attempted", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.write_result = {};

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.calls == std::vector<std::string> { "list", "write" });
  REQUIRE(result.write_outcome == RepositoryWriteOutcome::NOT_ATTEMPTED);
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::NOT_RUN);
  REQUIRE(result.repository_state_loaded);
  REQUIRE_FALSE(result.sync_needed);
  REQUIRE_FALSE(result.clear_pending);
  REQUIRE_FALSE(result.no_write_needed);
}

// -----------------------------------------------------------------------------
// Errors before a write attempt must remain available to the user.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply reports errors before a write was attempted", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.write_result = {};
  fixture.write_result.enable_succeeded = false;
  fixture.write_result.disable_succeeded = false;
  fixture.write_result.error = "Failed to open repository session";

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.calls == std::vector<std::string> { "list", "write" });
  REQUIRE(result.write_outcome == RepositoryWriteOutcome::NOT_ATTEMPTED);
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::NOT_RUN);
  REQUIRE(result.repository_state_loaded);
  REQUIRE_FALSE(result.sync_needed);
  REQUIRE_FALSE(result.clear_pending);
  REQUIRE(result.error == fixture.write_result.error);
}

// -----------------------------------------------------------------------------
// Later matching state must not overwrite a failed or partial daemon write result.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply keeps write failures distinct from verification", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.write_result.disable_succeeded = false;
  fixture.write_result.error = "Repository write failed";
  RepositoryWriteOutcome expected_outcome = RepositoryWriteOutcome::PARTIAL;

  SECTION("partial write")
  {
  }
  SECTION("failed write")
  {
    fixture.write_result.enable_succeeded = false;
    fixture.write_result.disable_attempted = false;
    expected_outcome = RepositoryWriteOutcome::FAILED;
  }

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.calls == std::vector<std::string> { "list", "write", "rebuild", "list" });
  REQUIRE(result.write_outcome == expected_outcome);
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED);
  REQUIRE(result.error == fixture.write_result.error);
  REQUIRE(result.sync_needed);
  REQUIRE(result.clear_pending);
}

// -----------------------------------------------------------------------------
// Verification covers every original choice, including those already matched before writing.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply verifies the complete requested state", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.final_repositories.at(0).enabled = false;

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.written_enable_ids == std::vector<std::string> { "updates" });
  REQUIRE(result.write_outcome == RepositoryWriteOutcome::SUCCEEDED);
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::MISMATCH);
  REQUIRE(result.repository_state_loaded);
  REQUIRE_FALSE(result.repositories.at(0).enabled);
  REQUIRE_FALSE(result.error.empty());
}

// -----------------------------------------------------------------------------
// A backend failure must not prevent the final daemon state from being checked.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply verifies writes even when the backend rebuild fails", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.rebuild_error = "Backend rebuild failed";

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.calls == std::vector<std::string> { "list", "write", "rebuild", "list" });
  REQUIRE(result.write_outcome == RepositoryWriteOutcome::SUCCEEDED);
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED);
  REQUIRE(result.backend_sync_result == RepositoryBackendSyncResult::FAILED);
  REQUIRE(result.repository_state_loaded);
  REQUIRE(result.error == fixture.rebuild_error);
  REQUIRE(result.sync_needed);
  REQUIRE(result.clear_pending);
}

// -----------------------------------------------------------------------------
// Old rows must not be presented as verified state when the final daemon read fails.
// -----------------------------------------------------------------------------
TEST_CASE("Repository Apply reports unavailable verification after a write", "[repository-apply]")
{
  RepositoryApplyFixture fixture;
  fixture.final_list_succeeds = false;

  SECTION("missing error text")
  {
  }
  SECTION("write and rebuild also failed")
  {
    fixture.write_result.disable_succeeded = false;
    fixture.write_result.error = "Disable failed";
    fixture.rebuild_error = "Rebuild failed";
    fixture.list_error = "Verification failed";
  }

  RepositoryApplyResult result = fixture.run();

  REQUIRE(fixture.calls == std::vector<std::string> { "list", "write", "rebuild", "list" });
  REQUIRE(result.verification_outcome == RepositoryVerificationOutcome::UNAVAILABLE);
  REQUIRE_FALSE(result.repository_state_loaded);
  REQUIRE(result.sync_needed);
  REQUIRE(result.clear_pending);
  REQUIRE_FALSE(result.error.empty());
  if (!fixture.list_error.empty()) {
    REQUIRE(result.write_outcome == RepositoryWriteOutcome::PARTIAL);
    REQUIRE(result.backend_sync_result == RepositoryBackendSyncResult::FAILED);
    REQUIRE(result.error == "Disable failed Rebuild failed Verification failed");
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
