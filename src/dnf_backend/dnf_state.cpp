// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_state.cpp
// Installed package cache and UI install-state helpers
//
// Owns backend global state used by the UI to mark exact installed packages,
// classify visible package rows, and prevent modifying the running application package from inside the app itself.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_internal.hpp"

#include "dnf_backend/base_manager.hpp"

#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>

#ifdef DNFUI_BUILD_TESTS
#include <glib.h>
#endif

#include <libdnf5/rpm/package_query.hpp>
#include <libdnf5/rpm/nevra.hpp>

namespace {

struct InstalledStateSnapshot {
  std::map<std::string, PackageInstallReason> nevra_reasons;
  std::map<std::string, PackageRow> rows_by_name_arch;
  std::set<std::string> self_protected_package_names;
};

// Installed-package data published together after a complete installed scan.
InstalledStateSnapshot g_installed_state;
// Mutex for thread-safe access to the installed snapshot.
std::mutex g_installed_mutex;

// -----------------------------------------------------------------------------
// Return true when one package spec names a protected package.
// -----------------------------------------------------------------------------
static bool
package_spec_matches_protected_name(const std::string &spec, const std::set<std::string> &protected_names)
{
  if (protected_names.count(spec) > 0) {
    return true;
  }

  try {
    for (const auto &nevra : libdnf5::rpm::Nevra::parse(spec)) {
      if (protected_names.count(nevra.get_name()) > 0) {
        return true;
      }
    }
  } catch (const std::exception &) {
    return false;
  }

  return false;
}

// -----------------------------------------------------------------------------
// Resolve the current GUI executable path so the app can block self-modification
// without hard-coding the RPM package name.
// -----------------------------------------------------------------------------
std::string
self_protected_file_path()
{
  try {
    return std::filesystem::canonical("/proc/self/exe").string();
  } catch (const std::exception &) {
  }

  return {};
}

// -----------------------------------------------------------------------------
// Resolve all installed-state values for one row while holding g_installed_mutex.
// -----------------------------------------------------------------------------
InstalledPackageResolution
resolve_installed_package_locked(const PackageRow &row)
{
  InstalledPackageResolution resolution;
  resolution.exact_installed = g_installed_state.nevra_reasons.count(row.nevra) > 0;
  resolution.self_protected = g_installed_state.self_protected_package_names.count(row.name) > 0;

  auto installed_it = g_installed_state.rows_by_name_arch.find(row.name_arch_key());
  if (installed_it != g_installed_state.rows_by_name_arch.end()) {
    resolution.has_installed_row = true;
    resolution.installed_row = installed_it->second;
  }

  if (resolution.exact_installed) {
    if (resolution.has_installed_row && libdnf5::rpm::evrcmp(row, resolution.installed_row) < 0) {
      resolution.state = PackageInstallState::INSTALLED;
      return resolution;
    }

    switch (row.repo_candidate_relation) {
    case PackageRepoCandidateRelation::UNKNOWN:
      // Annotation was not run or failed. The package is known-installed, but the repo relation is unknown.
      // Without a successful repo query, use INSTALLED so the UI does not misrepresent the package state.
    case PackageRepoCandidateRelation::SAME:
      resolution.state = PackageInstallState::INSTALLED;
      break;
    case PackageRepoCandidateRelation::NONE:
      resolution.state = PackageInstallState::LOCAL_ONLY;
      break;
    case PackageRepoCandidateRelation::NEWER:
      resolution.state = PackageInstallState::UPGRADEABLE;
      break;
    case PackageRepoCandidateRelation::OLDER:
      resolution.state = PackageInstallState::INSTALLED_NEWER_THAN_REPO;
      break;
    default:
      resolution.state = PackageInstallState::INSTALLED;
      break;
    }
    return resolution;
  }

  if (!resolution.has_installed_row) {
    resolution.state = PackageInstallState::AVAILABLE;
    return resolution;
  }

  int cmp = libdnf5::rpm::evrcmp(row, resolution.installed_row);
  if (cmp > 0) {
    resolution.state = PackageInstallState::UPGRADEABLE;
  } else if (cmp < 0) {
    resolution.state = PackageInstallState::DOWNGRADEABLE;
  } else {
    resolution.state = PackageInstallState::INSTALLED_NEWER_THAN_REPO;
  }

  return resolution;
}

// -----------------------------------------------------------------------------
// Keep repo-derived reinstall availability across a local rpmdb-only refresh
// only when the refreshed row is the same exact installed package.
// -----------------------------------------------------------------------------
void
preserve_local_refresh_reinstall_availability_locked(dnf_backend_internal::InstalledQueryResult &installed)
{
  for (auto &row : installed.rows) {
    auto old_it = g_installed_state.rows_by_name_arch.find(row.name_arch_key());
    if (old_it != g_installed_state.rows_by_name_arch.end() && old_it->second.nevra == row.nevra) {
      row.repo_candidate_exact_available = old_it->second.repo_candidate_exact_available;
    }
  }

  for (auto &[key, row] : installed.rows_by_name_arch) {
    auto old_it = g_installed_state.rows_by_name_arch.find(key);
    if (old_it != g_installed_state.rows_by_name_arch.end() && old_it->second.nevra == row.nevra) {
      row.repo_candidate_exact_available = old_it->second.repo_candidate_exact_available;
    }
  }
}

// -----------------------------------------------------------------------------
// Check identities, installation reasons, and protected package names.
// -----------------------------------------------------------------------------
bool
installed_snapshot_changed_locked(const dnf_backend_internal::InstalledQueryResult &installed,
                                  const std::set<std::string> &protected_names)
{
  if (g_installed_state.nevra_reasons != installed.nevra_reasons) {
    return true;
  }

  for (const auto &name : protected_names) {
    if (g_installed_state.self_protected_package_names.count(name) == 0) {
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Publish installed-package state while g_installed_mutex is already held.
// -----------------------------------------------------------------------------
bool
publish_installed_snapshot_locked(dnf_backend_internal::InstalledQueryResult installed,
                                  std::set<std::string> protected_names)
{
  std::set<std::string> next_protected_names = g_installed_state.self_protected_package_names;
  if (!protected_names.empty()) {
    next_protected_names.insert(protected_names.begin(), protected_names.end());
  }
  const bool changed = installed_snapshot_changed_locked(installed, protected_names);
  g_installed_state.nevra_reasons.swap(installed.nevra_reasons);
  g_installed_state.rows_by_name_arch.swap(installed.rows_by_name_arch);
  g_installed_state.self_protected_package_names.swap(next_protected_names);
  return changed;
}

} // namespace

namespace dnf_backend_internal {

// -----------------------------------------------------------------------------
// Collect installed package names that own the currently running GUI binary.
// The result is stored in the installed-state snapshot and used to block
// self-modification actions from inside the app.
// -----------------------------------------------------------------------------
std::set<std::string>
collect_self_protected_package_names(libdnf5::Base &base)
{
  std::set<std::string> protected_names;

  const std::string path = self_protected_file_path();
  if (!path.empty()) {
    libdnf5::rpm::PackageQuery query(base);
    query.filter_installed();
    query.filter_file(path);

    for (const auto &pkg : query) {
      protected_names.insert(pkg.get_name());
    }
  }

#ifdef DNFUI_BUILD_TESTS
  // Let tests exercise self-protection with an installed package chosen by the test case.
  // Production builds only use the running executable owner above.
  const char *test_name = g_getenv("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME");
  if (test_name && *test_name) {
    protected_names.insert(test_name);
  }
#endif

  return protected_names;
}

// -----------------------------------------------------------------------------
// Return protected package names, refreshing from rpmdb if installed state has
// not been published yet.
// -----------------------------------------------------------------------------
std::set<std::string>
self_protected_package_name_snapshot()
{
  std::set<std::string> protected_names;
  {
    std::lock_guard<std::mutex> lock(g_installed_mutex);
    protected_names = g_installed_state.self_protected_package_names;
  }

  if (!protected_names.empty()) {
    return protected_names;
  }

  auto read = BaseManager::instance().acquire_system_only_read();
  protected_names = collect_self_protected_package_names(*read.base);

  if (!protected_names.empty()) {
    std::lock_guard<std::mutex> lock(g_installed_mutex);
    if (g_installed_state.self_protected_package_names.empty()) {
      g_installed_state.self_protected_package_names = protected_names;
    } else {
      protected_names = g_installed_state.self_protected_package_names;
    }
  }

  return protected_names;
}

// -----------------------------------------------------------------------------
// Publish installed-package state only after callers have finished all libdnf
// Base reads. Do not hold the Base lock while taking g_installed_mutex.
// The return value tracks installed identities, reasons, and self-protection changes.
// -----------------------------------------------------------------------------
bool
publish_installed_snapshot(InstalledQueryResult installed, std::set<std::string> protected_names)
{
  auto publish_lock = BaseManager::instance().acquire_installed_snapshot_publication_lock();
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  return publish_installed_snapshot_locked(std::move(installed), std::move(protected_names));
}

// -----------------------------------------------------------------------------
// Prevent an older package query from overwriting a refreshed installed snapshot.
// -----------------------------------------------------------------------------
bool
publish_installed_snapshot_if_generation_matches(InstalledQueryResult installed,
                                                 std::set<std::string> protected_names,
                                                 uint64_t base_generation)
{
  auto publish_lock = BaseManager::instance().acquire_installed_snapshot_publication_lock();
  if (BaseManager::instance().current_generation() != base_generation) {
    return false;
  }

  std::lock_guard<std::mutex> lock(g_installed_mutex);
  publish_installed_snapshot_locked(std::move(installed), std::move(protected_names));
  return true;
}

// -----------------------------------------------------------------------------
// Publish a local rpmdb-only installed scan.
// A local refresh cannot prove repository availability, so keep the previous
// exact reinstall value only when the same installed NEVRA is still present.
// -----------------------------------------------------------------------------
bool
publish_local_installed_snapshot(InstalledQueryResult installed,
                                 std::set<std::string> protected_names,
                                 bool invalidate_retained_base)
{
  auto publish_lock = BaseManager::instance().acquire_installed_snapshot_publication_lock();
  if (invalidate_retained_base) {
    bool changed;
    {
      std::lock_guard<std::mutex> lock(g_installed_mutex);
      changed = installed_snapshot_changed_locked(installed, protected_names);
    }
    if (changed) {
      BaseManager::instance().invalidate_cached_base_for_installed_state_change();
    }
  }

  std::lock_guard<std::mutex> lock(g_installed_mutex);
  preserve_local_refresh_reinstall_availability_locked(installed);
  return publish_installed_snapshot_locked(std::move(installed), std::move(protected_names));
}

// -----------------------------------------------------------------------------
// Refresh the installed snapshot through a short-lived system-only Base.
// The caller chooses whether this should also discard the cached shared Base.
//
// Thread-safety:
//   The Base read lock and g_installed_mutex must never be held simultaneously.
//   Installed rows are collected into local containers while the Base lock is
//   held, then published after that lock has been released.
// -----------------------------------------------------------------------------
bool
refresh_installed_snapshot(bool drop_cached_base, bool invalidate_retained_base = false)
{
  InstalledQueryResult installed;
  std::set<std::string> protected_names;
  {
    auto read = drop_cached_base ? BaseManager::instance().acquire_system_only_read_after_dropping_cached_base()
                                 : BaseManager::instance().acquire_system_only_read();
    libdnf5::Base &base = *read.base;
    const DnfBackendSearchOptions search_options {};
    installed = collect_installed_rows(base, nullptr, search_options);
    protected_names = collect_self_protected_package_names(base);
  } // Base read lock released before acquiring g_installed_mutex

  return publish_local_installed_snapshot(std::move(installed), std::move(protected_names), invalidate_retained_base);
}

} // namespace dnf_backend_internal

using namespace dnf_backend_internal;

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Return the number of exact NEVRAs in the installed-package snapshot.
// -----------------------------------------------------------------------------
size_t
dnf_backend_installed_snapshot_size_for_tests()
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  return g_installed_state.nevra_reasons.size();
}

// -----------------------------------------------------------------------------
// Return true when the installed-package snapshot contains the exact NEVRA.
// -----------------------------------------------------------------------------
bool
dnf_backend_installed_snapshot_contains_for_tests(const std::string &nevra)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  return g_installed_state.nevra_reasons.count(nevra) > 0;
}
#endif

// -----------------------------------------------------------------------------
// Refresh the installed snapshot used by UI state classification.
// Returns true when installed identities or self-protected package names changed.
// This path uses only the local rpmdb.
// The short-lived system-only Base prevents future queries from inheriting that mode.
//
// Thread-safety:
//   The Base read lock and g_installed_mutex must never be held simultaneously.
//   Installed rows are collected into local containers while the Base lock is
//   held, then published after that lock has been released.
// -----------------------------------------------------------------------------
bool
dnf_backend_refresh_installed_snapshot()
{
  return refresh_installed_snapshot(/*drop_cached_base=*/true);
}

// -----------------------------------------------------------------------------
// Refresh the installed snapshot without discarding the warmed shared Base.
// This path is used by passive startup checks that should not slow the first query.
// -----------------------------------------------------------------------------
bool
dnf_backend_refresh_installed_snapshot_preserving_cached_base()
{
  return refresh_installed_snapshot(/*drop_cached_base=*/false);
}

// -----------------------------------------------------------------------------
// Preserve the shared Base during periodic checks only while retention is enabled.
// A changed installed snapshot invalidates the Base before the next package query.
// -----------------------------------------------------------------------------
bool
dnf_backend_refresh_installed_snapshot_for_periodic_check()
{
  auto &manager = BaseManager::instance();
  if (!manager.keep_base_between_queries_enabled()) {
    return dnf_backend_refresh_installed_snapshot();
  }

  return refresh_installed_snapshot(/*drop_cached_base=*/false, /*invalidate_retained_base=*/true);
}

// -----------------------------------------------------------------------------
// Resolve installed-package state for one visible row from one installed snapshot.
// -----------------------------------------------------------------------------
InstalledPackageResolution
dnf_backend_resolve_installed_package(const PackageRow &row)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  return resolve_installed_package_locked(row);
}

// -----------------------------------------------------------------------------
// Return the default package table sort priority for one install state. Lower
// values sort first and keep installed rows ahead of repo-only rows.
// -----------------------------------------------------------------------------
int
dnf_backend_get_install_state_sort_rank(PackageInstallState state)
{
  switch (state) {
  case PackageInstallState::INSTALLED:
    return 0;
  case PackageInstallState::INSTALLED_NEWER_THAN_REPO:
    return 1;
  case PackageInstallState::LOCAL_ONLY:
    return 2;
  case PackageInstallState::UPGRADEABLE:
    return 3;
  case PackageInstallState::DOWNGRADEABLE:
    return 4;
  case PackageInstallState::AVAILABLE:
  default:
    return 5;
  }
}

// -----------------------------------------------------------------------------
// Check one package name against the current installed snapshot.
// The caller refreshes that snapshot before this check when fresh rpmdb state matters.
// -----------------------------------------------------------------------------
bool
dnf_backend_is_self_protected_package_name(const std::string &name)
{
  if (name.empty()) {
    return false;
  }

  std::set<std::string> protected_names;
  {
    std::lock_guard<std::mutex> lock(g_installed_mutex);
    protected_names = g_installed_state.self_protected_package_names;
  }

  return protected_names.count(name) > 0;
}

// -----------------------------------------------------------------------------
// Resolve one queued transaction spec back to the installed rpmdb so request
// validation can reject self-modification even if the UI state is outdated or bypassed.
// -----------------------------------------------------------------------------
bool
dnf_backend_is_self_protected_transaction_spec(const std::string &spec)
{
  std::set<std::string> protected_names = self_protected_package_name_snapshot();

  if (protected_names.empty()) {
    return false;
  }

  if (package_spec_matches_protected_name(spec, protected_names)) {
    return true;
  }

  auto read = BaseManager::instance().acquire_system_only_read();
  libdnf5::Base &base = *read.base;
  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();
  query.filter_nevra(spec);

  for (const auto &pkg : query) {
    if (protected_names.count(pkg.get_name()) > 0) {
      return true;
    }
  }

  libdnf5::rpm::PackageQuery name_query(base);
  name_query.filter_installed();
  name_query.filter_name(spec, libdnf5::sack::QueryCmp::EQ);

  for (const auto &pkg : name_query) {
    if (protected_names.count(pkg.get_name()) > 0) {
      return true;
    }
  }

  return false;
}

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Clear the installed-package snapshot for tests that seed exact NEVRA state without querying the host rpmdb.
// -----------------------------------------------------------------------------
void
dnf_backend_testonly_clear_installed_snapshot()
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_state = {};
}

// -----------------------------------------------------------------------------
// Replace the installed-package snapshot for tests that need deterministic
// install-state classification without depending on host package state.
// -----------------------------------------------------------------------------
void
dnf_backend_testonly_replace_installed_snapshot(const std::set<std::string> &nevras)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_state.nevra_reasons.clear();
  for (const auto &nevra : nevras) {
    g_installed_state.nevra_reasons.emplace(nevra, PackageInstallReason::UNKNOWN);
  }
  g_installed_state.rows_by_name_arch.clear();
  g_installed_state.self_protected_package_names.clear();
}

// -----------------------------------------------------------------------------
// Replace the installed-package snapshot with full rows for name and architecture lookup tests.
// -----------------------------------------------------------------------------
void
dnf_backend_testonly_replace_installed_snapshot_rows(const std::vector<PackageRow> &rows)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_state = {};

  for (const auto &row : rows) {
    g_installed_state.nevra_reasons.emplace(row.nevra, row.install_reason);
    g_installed_state.rows_by_name_arch[row.name_arch_key()] = row;
  }
}

// -----------------------------------------------------------------------------
// Publish an installed-package snapshot through the normal publication path for tests.
// -----------------------------------------------------------------------------
bool
dnf_backend_testonly_publish_installed_snapshot_rows(const std::vector<PackageRow> &rows,
                                                     const std::set<std::string> &protected_names)
{
  InstalledQueryResult installed;
  for (const auto &row : rows) {
    installed.nevra_reasons.emplace(row.nevra, row.install_reason);
    installed.rows_by_name_arch[row.name_arch_key()] = row;
  }

  return publish_installed_snapshot(std::move(installed), protected_names);
}
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
