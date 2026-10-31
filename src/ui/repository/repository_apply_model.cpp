// -----------------------------------------------------------------------------
// src/ui/repository/repository_apply_model.cpp
// Repository apply planning helpers
// -----------------------------------------------------------------------------
#include "ui/repository/repository_apply_model.hpp"

#include "i18n.hpp"

#include <algorithm>

namespace {

const RepositoryInfo *
find_repository(const std::vector<RepositoryInfo> &repositories, const std::string &repo_id)
{
  auto it = std::find_if(repositories.begin(), repositories.end(), [&](const RepositoryInfo &repository) {
    return repository.id == repo_id;
  });

  if (it == repositories.end()) {
    return nullptr;
  }

  return &*it;
}

} // namespace

RepositoryChangePlan
repository_apply_plan_changes(const std::vector<RepositoryInfo> &repositories,
                              const std::vector<std::string> &requested_enable_ids,
                              const std::vector<std::string> &requested_disable_ids)
{
  RepositoryChangePlan plan;

  for (const auto &repo_id : requested_enable_ids) {
    const RepositoryInfo *repository = find_repository(repositories, repo_id);
    if (!repository) {
      plan.valid = false;
      plan.error = _("Repository configuration changed before Apply. Reload repositories and try again.");
      return plan;
    }
    if (!repository->enabled) {
      plan.enable_ids.push_back(repo_id);
    }
  }

  for (const auto &repo_id : requested_disable_ids) {
    const RepositoryInfo *repository = find_repository(repositories, repo_id);
    if (!repository) {
      plan.valid = false;
      plan.error = _("Repository configuration changed before Apply. Reload repositories and try again.");
      return plan;
    }
    if (repository->enabled) {
      plan.disable_ids.push_back(repo_id);
    }
  }

  return plan;
}

bool
repository_apply_requested_states_match(const std::vector<RepositoryInfo> &repositories,
                                        const std::vector<std::string> &repo_ids,
                                        bool enabled)
{
  for (const auto &repo_id : repo_ids) {
    const RepositoryInfo *repository = find_repository(repositories, repo_id);
    if (!repository || repository->enabled != enabled) {
      return false;
    }
  }

  return true;
}

bool
repository_apply_requested_states_match(const std::vector<RepositoryInfo> &repositories,
                                        const std::vector<std::string> &enable_ids,
                                        const std::vector<std::string> &disable_ids)
{
  return repository_apply_requested_states_match(repositories, enable_ids, true) &&
      repository_apply_requested_states_match(repositories, disable_ids, false);
}

RepositoryBackendSyncResult
repository_apply_backend_sync_result(BaseRepoState repo_state)
{
  switch (repo_state) {
  case BaseRepoState::LIVE_METADATA:
    return RepositoryBackendSyncResult::LIVE_METADATA;
  case BaseRepoState::CACHED_METADATA:
    return RepositoryBackendSyncResult::CACHED_METADATA;
  case BaseRepoState::INSTALLED_ONLY:
    return RepositoryBackendSyncResult::INSTALLED_ONLY;
  }

  return RepositoryBackendSyncResult::FAILED;
}

RepositoryWriteOutcome
repository_apply_write_outcome(const RepositoryWriteResult &write_result)
{
  if (!write_result.enable_attempted && !write_result.disable_attempted) {
    return RepositoryWriteOutcome::NOT_ATTEMPTED;
  }

  if (write_result.enable_attempted && !write_result.enable_succeeded) {
    return RepositoryWriteOutcome::FAILED;
  }

  if (write_result.disable_attempted && !write_result.disable_succeeded) {
    if (write_result.enable_attempted && write_result.enable_succeeded) {
      return RepositoryWriteOutcome::PARTIAL;
    }
    return RepositoryWriteOutcome::FAILED;
  }

  return RepositoryWriteOutcome::SUCCEEDED;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
