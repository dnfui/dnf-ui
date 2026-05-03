// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_state.cpp
// Installed package cache and UI install-state helpers
//
// Owns backend global state used by the UI to mark exact installed packages,
// classify visible package rows, and prevent removing the running application
// package from inside the app itself.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_internal.hpp"

#include "base_manager.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <libdnf5/rpm/package_query.hpp>

namespace {

// Cached NEVRAs of installed packages for UI highlighting.
std::set<std::string> g_installed_nevras;
// Mutex for thread-safe access to the installed-package cache and derived state.
std::mutex g_installed_mutex;
// Packed search flags read by query workers when they start a search. Keeping
// both options in one atomic makes dnf_backend_get_search_options a single
// coherent snapshot.
constexpr unsigned kSearchInDescriptionBit = 1U << 0;
constexpr unsigned kExactMatchBit = 1U << 1;
std::atomic<unsigned> g_search_option_bits { 0 };
// Cached installed rows keyed by name and arch for upgrade-state classification.
std::map<std::string, PackageRow> g_installed_rows_by_name_arch;
// Installed package names that own the running GUI binary.
std::set<std::string> g_self_protected_package_names;

struct StateBackendBaseDropGuard {
  ~StateBackendBaseDropGuard()
  {
    BaseManager::instance().drop_cached_base();
  }
};

// -----------------------------------------------------------------------------
// Resolve the current GUI executable path so the app can block self-removal
// without hard-coding the RPM package name.
// -----------------------------------------------------------------------------
std::vector<std::string>
self_protected_file_paths()
{
  std::set<std::string> paths;

  try {
    paths.insert(std::filesystem::canonical("/proc/self/exe").string());
  } catch (const std::exception &) {
  }

  return { paths.begin(), paths.end() };
}

} // namespace

namespace dnf_backend_internal {

// -----------------------------------------------------------------------------
// Collect installed package names that own the currently running GUI binary.
// The result is stored in the installed-state snapshot and used to block
// self-removal and self-reinstall actions from inside the app.
// -----------------------------------------------------------------------------
std::set<std::string>
collect_self_protected_package_names(libdnf5::Base &base)
{
  std::set<std::string> protected_names;

  for (const auto &path : self_protected_file_paths()) {
    libdnf5::rpm::PackageQuery query(base);
    query.filter_installed();
    query.filter_file(path);

    for (const auto &pkg : query) {
      protected_names.insert(pkg.get_name());
    }
  }

  return protected_names;
}

// -----------------------------------------------------------------------------
// Publish installed-package state only after callers have finished all libdnf
// Base reads. Holding the Base lock while taking g_installed_mutex would make
// future UI cache callers vulnerable to lock-order deadlocks.
// -----------------------------------------------------------------------------
void
publish_installed_snapshot(InstalledQueryResult installed, std::set<std::string> protected_names)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_nevras.swap(installed.nevras);
  g_installed_rows_by_name_arch.swap(installed.rows_by_name_arch);
  g_self_protected_package_names.swap(protected_names);
}

} // namespace dnf_backend_internal

using namespace dnf_backend_internal;

// -----------------------------------------------------------------------------
// Publish the search options used by future backend search workers.
// -----------------------------------------------------------------------------
void
dnf_backend_set_search_options(const DnfBackendSearchOptions &options)
{
  unsigned bits = 0;
  if (options.search_in_description) {
    bits |= kSearchInDescriptionBit;
  }
  if (options.exact_match) {
    bits |= kExactMatchBit;
  }
  g_search_option_bits.store(bits, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// Return one consistent snapshot of the backend search options.
// -----------------------------------------------------------------------------
DnfBackendSearchOptions
dnf_backend_get_search_options()
{
  const unsigned bits = g_search_option_bits.load(std::memory_order_relaxed);
  return {
    .search_in_description = (bits & kSearchInDescriptionBit) != 0,
    .exact_match = (bits & kExactMatchBit) != 0,
  };
}

// -----------------------------------------------------------------------------
// Return true when the installed-package snapshot contains one exact NEVRA.
// -----------------------------------------------------------------------------
bool
dnf_backend_installed_snapshot_contains(const std::string &nevra)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  return g_installed_nevras.count(nevra) > 0;
}

// -----------------------------------------------------------------------------
// Return the number of exact NEVRAs in the installed-package snapshot.
// -----------------------------------------------------------------------------
size_t
dnf_backend_installed_snapshot_size()
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  return g_installed_nevras.size();
}

// -----------------------------------------------------------------------------
// Refresh the exact-installed and self-protection snapshots used by UI state
// classification. This path is intentionally local-first: it does not require
// repository metadata and should keep working from the rpmdb alone.
//
// Thread-safety:
//   The Base read lock and g_installed_mutex must never be held simultaneously.
//   Installed rows are collected into local containers while the Base lock is
//   held, then published after that lock has been released.
// -----------------------------------------------------------------------------
void
dnf_backend_refresh_installed_nevras()
{
  InstalledQueryResult installed;
  std::set<std::string> protected_names;
  {
    auto [base, guard, generation] = BaseManager::instance().acquire_read();
    const DnfBackendSearchOptions search_options {};
    installed = collect_installed_rows(base, nullptr, search_options);
    protected_names = collect_self_protected_package_names(base);
  } // Base read lock released before acquiring g_installed_mutex

  publish_installed_snapshot(installed, protected_names);
}

// -----------------------------------------------------------------------------
// Return true only when the queried row exactly matches an installed NEVRA in
// the cached installed snapshot.
// -----------------------------------------------------------------------------
bool
dnf_backend_is_package_installed_exact(const PackageRow &row)
{
  return dnf_backend_installed_snapshot_contains(row.nevra);
}

// -----------------------------------------------------------------------------
// Classify a package row as available, upgradeable, exact-installed,
// local-only, or installed-newer-than-repo. Exact-installed rows prefer the row
// provenance annotation; available rows fall back to the installed name and architecture
// cache so upgrade-state badges can be shown without duplicate visible rows.
// -----------------------------------------------------------------------------
PackageInstallState
dnf_backend_get_package_install_state(const PackageRow &row)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  if (g_installed_nevras.count(row.nevra) > 0) {
    switch (row.repo_candidate_relation) {
    case PackageRepoCandidateRelation::UNKNOWN:
      // Annotation was not run or failed. The package is
      // known-installed but we cannot distinguish LOCAL_ONLY from INSTALLED
      // without a successful repo query. Fall back to INSTALLED so the UI
      // does not misrepresent the package state.
    case PackageRepoCandidateRelation::SAME:
      return PackageInstallState::INSTALLED;
    case PackageRepoCandidateRelation::NONE:
      return PackageInstallState::LOCAL_ONLY;
    case PackageRepoCandidateRelation::NEWER:
      return PackageInstallState::UPGRADEABLE;
    case PackageRepoCandidateRelation::OLDER:
      return PackageInstallState::INSTALLED_NEWER_THAN_REPO;
    default:
      return PackageInstallState::INSTALLED;
    }
  }

  auto it = g_installed_rows_by_name_arch.find(row.name_arch_key());
  if (it == g_installed_rows_by_name_arch.end()) {
    return PackageInstallState::AVAILABLE;
  }

  if (libdnf5::rpm::evrcmp(row, it->second) > 0) {
    return PackageInstallState::UPGRADEABLE;
  }

  return PackageInstallState::INSTALLED_NEWER_THAN_REPO;
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
  case PackageInstallState::AVAILABLE:
  default:
    return 4;
  }
}

// -----------------------------------------------------------------------------
// Return true only when the exact installed NEVRA is also available from the
// current package sources and can therefore be reinstalled through libdnf5.
// -----------------------------------------------------------------------------
bool
dnf_backend_can_reinstall_package(const PackageRow &row)
{
  // Reinstall is valid only for the exact installed NEVRA that the user is
  // looking at, not for an older or newer visible candidate with the same name.
  if (!dnf_backend_is_package_installed_exact(row)) {
    return false;
  }

  StateBackendBaseDropGuard base_drop_guard;

  bool can_reinstall = false;
  {
    auto [base, guard, generation] = BaseManager::instance().acquire_read();
    libdnf5::rpm::PackageQuery query(base);
    query.filter_nevra(row.nevra);
    query.filter_available();
    can_reinstall = !query.empty();
  }

  return can_reinstall;
}

// -----------------------------------------------------------------------------
// Check the cached self-protection snapshot collected from the owner of the
// running GUI executable during the latest installed-package refresh.
// -----------------------------------------------------------------------------
bool
dnf_backend_is_package_self_protected(const PackageRow &row)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  return g_self_protected_package_names.count(row.name) > 0;
}

// -----------------------------------------------------------------------------
// Resolve one queued transaction spec back to the installed rpmdb so request
// validation can reject self-removal even if the UI state is outdated or bypassed.
// -----------------------------------------------------------------------------
bool
dnf_backend_is_self_protected_transaction_spec(const std::string &spec)
{
  std::set<std::string> protected_names;
  {
    std::lock_guard<std::mutex> lock(g_installed_mutex);
    protected_names = g_self_protected_package_names;
  }

  if (protected_names.empty()) {
    return false;
  }

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
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
// Clear the installed-package snapshot for tests that seed exact NEVRA state
// without querying the host rpmdb.
// -----------------------------------------------------------------------------
void
dnf_backend_testonly_clear_installed_snapshot()
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_nevras.clear();
  g_installed_rows_by_name_arch.clear();
  g_self_protected_package_names.clear();
}

// -----------------------------------------------------------------------------
// Replace the installed-package snapshot for tests that need deterministic
// install-state classification without depending on host package state.
// -----------------------------------------------------------------------------
void
dnf_backend_testonly_replace_installed_snapshot(const std::set<std::string> &nevras)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  g_installed_nevras = nevras;
  g_installed_rows_by_name_arch.clear();
  g_self_protected_package_names.clear();
}
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
