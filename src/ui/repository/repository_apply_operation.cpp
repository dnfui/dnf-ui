// -----------------------------------------------------------------------------
// repository_apply_operation.cpp
// Repository Apply workflow independent of GTK
// -----------------------------------------------------------------------------
#include "ui/repository/repository_apply_operation.hpp"

#include "i18n.hpp"

#include <exception>
#include <utility>

namespace {

// -----------------------------------------------------------------------------
// Keep rebuild failures separate from the write result and preserve earlier errors.
// -----------------------------------------------------------------------------
void
rebuild_backend(RepositoryApplyResult &result, const RepositoryApplyDependencies &dependencies)
{
  try {
    BaseRepoState repo_state = dependencies.rebuild_backend();
    result.backend_sync_result = repository_apply_backend_sync_result(true, repo_state);
  } catch (const std::exception &e) {
    result.backend_sync_result = RepositoryBackendSyncResult::FAILED;
    if (!result.error.empty()) {
      result.error += " ";
    }
    result.error += e.what();
  }
}

} // namespace

RepositoryApplyResult
repository_apply_run(const std::vector<std::string> &desired_enable_ids,
                     const std::vector<std::string> &desired_disable_ids)
{
  RepositoryApplyDependencies dependencies {
    .list_repositories =
        [](std::vector<RepositoryInfo> &repositories, std::string &error) {
          return repository_service_client_list(repositories, error, nullptr);
        },
    .apply_changes = repository_service_client_apply_changes,
    .rebuild_backend = []() { return BaseManager::instance().rebuild(BaseRefreshMode::NORMAL); },
  };

  return repository_apply_run(desired_enable_ids, desired_disable_ids, dependencies);
}

RepositoryApplyResult
repository_apply_run(const std::vector<std::string> &desired_enable_ids,
                     const std::vector<std::string> &desired_disable_ids,
                     const RepositoryApplyDependencies &dependencies)
{
  RepositoryApplyResult result;
  std::vector<RepositoryInfo> current_repositories;
  std::string current_list_error;
  if (!dependencies.list_repositories(current_repositories, current_list_error)) {
    result.error = current_list_error.empty() ? _("Repository state could not be reloaded.") : current_list_error;
    return result;
  }

  result.repository_state_loaded = true;
  result.repositories = current_repositories;

  RepositoryChangePlan plan =
      repository_apply_plan_changes(current_repositories, desired_enable_ids, desired_disable_ids);
  if (!plan.valid) {
    result.error = plan.error;
    result.sync_needed = true;
    result.clear_pending = true;
    rebuild_backend(result, dependencies);
    return result;
  }

  if (plan.enable_ids.empty() && plan.disable_ids.empty()) {
    result.sync_needed = true;
    result.no_write_needed = true;
    result.clear_pending = true;
    rebuild_backend(result, dependencies);
    return result;
  }

  RepositoryWriteResult write_result = dependencies.apply_changes(plan.enable_ids, plan.disable_ids);
  result.write_outcome = repository_apply_write_outcome(write_result);
  result.sync_needed = result.write_outcome != RepositoryWriteOutcome::NOT_ATTEMPTED;
  if (result.write_outcome == RepositoryWriteOutcome::FAILED ||
      result.write_outcome == RepositoryWriteOutcome::PARTIAL) {
    result.error = write_result.error.empty() ? _("Repository change failed.") : write_result.error;
  }

  if (result.sync_needed) {
    result.clear_pending = true;
    rebuild_backend(result, dependencies);

    std::vector<RepositoryInfo> repositories;
    std::string list_error;
    if (dependencies.list_repositories(repositories, list_error)) {
      result.repository_state_loaded = true;
      result.repositories = std::move(repositories);
      bool requested_states_match =
          repository_apply_requested_states_match(result.repositories, desired_enable_ids, desired_disable_ids);
      result.verification_outcome = repository_apply_verification_outcome(true, requested_states_match);
      if (result.verification_outcome == RepositoryVerificationOutcome::MISMATCH) {
        if (!result.error.empty()) {
          result.error += " ";
        }
        result.error += _("Some repository changes were not confirmed by dnf5daemon.");
      }
    } else {
      result.repository_state_loaded = false;
      result.verification_outcome = repository_apply_verification_outcome(false, false);
      if (!result.error.empty()) {
        result.error += " ";
      }
      result.error += list_error.empty() ? _("Repository state could not be reloaded.") : list_error;
    }
  }

  return result;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
