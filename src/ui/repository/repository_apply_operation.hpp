// -----------------------------------------------------------------------------
// repository_apply_operation.hpp
// Repository Apply workflow independent of GTK
// -----------------------------------------------------------------------------
#pragma once

#include "ui/repository/repository_apply_model.hpp"

#include <functional>
#include <string>
#include <vector>

struct RepositoryApplyResult {
  bool sync_needed = false;
  bool no_write_needed = false;
  bool clear_pending = false;
  RepositoryWriteOutcome write_outcome = RepositoryWriteOutcome::NOT_ATTEMPTED;
  RepositoryVerificationOutcome verification_outcome = RepositoryVerificationOutcome::NOT_RUN;
  RepositoryBackendSyncResult backend_sync_result = RepositoryBackendSyncResult::FAILED;
  // False when the latest repository read failed, even if older rows remain below.
  bool repository_state_loaded = false;
  std::vector<RepositoryInfo> repositories;
  std::string error;
};

// -----------------------------------------------------------------------------
// Supply all three operations when testing without daemon writes or a backend rebuild.
// -----------------------------------------------------------------------------
struct RepositoryApplyDependencies {
  std::function<bool(std::vector<RepositoryInfo> &, std::string &)> list_repositories;
  std::function<RepositoryWriteResult(const std::vector<std::string> &, const std::vector<std::string> &)>
      apply_changes;
  std::function<BaseRepoState()> rebuild_backend;
};

// -----------------------------------------------------------------------------
// Apply requested repository states and verify the result through the service clients.
// Runs synchronously without cancellation. Call from a worker thread.
// -----------------------------------------------------------------------------
RepositoryApplyResult repository_apply_run(const std::vector<std::string> &desired_enable_ids,
                                           const std::vector<std::string> &desired_disable_ids);

// -----------------------------------------------------------------------------
// Run the same workflow with explicitly supplied repository and backend operations.
// -----------------------------------------------------------------------------
RepositoryApplyResult repository_apply_run(const std::vector<std::string> &desired_enable_ids,
                                           const std::vector<std::string> &desired_disable_ids,
                                           const RepositoryApplyDependencies &dependencies);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
