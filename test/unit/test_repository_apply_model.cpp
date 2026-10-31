// -----------------------------------------------------------------------------
// test_repository_apply_model.cpp
// Repository apply model tests
// Exercises repository change planning, write results, verification, and backend state classification.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "ui/repository/repository_apply_model.hpp"

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Verify that only still-needed repository changes are sent to dnf5daemon.
// -----------------------------------------------------------------------------
TEST_CASE("Repository apply plan skips states that already match")
{
  std::vector<RepositoryInfo> repositories {
    { "fedora", "Fedora", true },
    { "updates", "Fedora Updates", false },
    { "testing", "Fedora Testing", true },
  };

  RepositoryChangePlan plan = repository_apply_plan_changes(repositories, { "fedora", "updates" }, { "testing" });

  REQUIRE(plan.valid);
  REQUIRE(plan.error.empty());
  REQUIRE(plan.enable_ids == std::vector<std::string> { "updates" });
  REQUIRE(plan.disable_ids == std::vector<std::string> { "testing" });
}

// -----------------------------------------------------------------------------
// Verify that a disappeared staged repository stops the write before it starts.
// -----------------------------------------------------------------------------
TEST_CASE("Repository apply plan rejects disappeared staged repositories")
{
  std::vector<RepositoryInfo> repositories {
    { "fedora", "Fedora", true },
  };

  RepositoryChangePlan plan = repository_apply_plan_changes(repositories, { "missing" }, {});

  REQUIRE_FALSE(plan.valid);
  REQUIRE_FALSE(plan.error.empty());
  REQUIRE(plan.enable_ids.empty());
  REQUIRE(plan.disable_ids.empty());
}

// -----------------------------------------------------------------------------
// Verify final daemon state checks both enabled and disabled requests.
// -----------------------------------------------------------------------------
TEST_CASE("Repository apply final state verification checks requested states")
{
  std::vector<RepositoryInfo> repositories {
    { "fedora", "Fedora", true },
    { "updates", "Fedora Updates", false },
  };

  REQUIRE(repository_apply_requested_states_match(
      repositories, std::vector<std::string> { "fedora" }, std::vector<std::string> { "updates" }));
  REQUIRE_FALSE(repository_apply_requested_states_match(
      repositories, std::vector<std::string> { "updates" }, std::vector<std::string> {}));
  REQUIRE_FALSE(repository_apply_requested_states_match(
      repositories, std::vector<std::string> {}, std::vector<std::string> { "fedora" }));
  REQUIRE_FALSE(repository_apply_requested_states_match(
      repositories, std::vector<std::string> { "missing" }, std::vector<std::string> {}));
}

// -----------------------------------------------------------------------------
// Verify that BaseManager rebuild outcomes remain distinct.
// -----------------------------------------------------------------------------
TEST_CASE("Repository apply backend sync classification preserves Base state")
{
  REQUIRE(repository_apply_backend_sync_result(BaseRepoState::LIVE_METADATA) ==
          RepositoryBackendSyncResult::LIVE_METADATA);
  REQUIRE(repository_apply_backend_sync_result(BaseRepoState::CACHED_METADATA) ==
          RepositoryBackendSyncResult::CACHED_METADATA);
  REQUIRE(repository_apply_backend_sync_result(BaseRepoState::INSTALLED_ONLY) ==
          RepositoryBackendSyncResult::INSTALLED_ONLY);
}

// -----------------------------------------------------------------------------
// Verify that daemon write outcome remains separate from later checks.
// -----------------------------------------------------------------------------
TEST_CASE("Repository apply write outcome keeps partial writes distinct")
{
  RepositoryWriteResult not_attempted;
  not_attempted.enable_succeeded = false;
  not_attempted.disable_succeeded = false;
  REQUIRE(repository_apply_write_outcome(not_attempted) == RepositoryWriteOutcome::NOT_ATTEMPTED);

  RepositoryWriteResult enable_failed;
  enable_failed.enable_attempted = true;
  enable_failed.enable_succeeded = false;
  REQUIRE(repository_apply_write_outcome(enable_failed) == RepositoryWriteOutcome::FAILED);

  RepositoryWriteResult disable_failed;
  disable_failed.enable_attempted = true;
  disable_failed.enable_succeeded = true;
  disable_failed.disable_attempted = true;
  disable_failed.disable_succeeded = false;
  REQUIRE(repository_apply_write_outcome(disable_failed) == RepositoryWriteOutcome::PARTIAL);

  RepositoryWriteResult succeeded;
  succeeded.disable_attempted = true;
  succeeded.disable_succeeded = true;
  REQUIRE(repository_apply_write_outcome(succeeded) == RepositoryWriteOutcome::SUCCEEDED);
}

// -----------------------------------------------------------------------------
// Verify final-state verification does not replace the daemon write result.
// -----------------------------------------------------------------------------
TEST_CASE("Repository apply verification outcome is classified separately")
{
  REQUIRE(repository_apply_verification_outcome(false, false) == RepositoryVerificationOutcome::UNAVAILABLE);
  REQUIRE(repository_apply_verification_outcome(true, false) == RepositoryVerificationOutcome::MISMATCH);
  REQUIRE(repository_apply_verification_outcome(true, true) == RepositoryVerificationOutcome::CONFIRMED);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
