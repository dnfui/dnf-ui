// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_query.cpp
// Package row query and merge helpers
//
// Owns browse, search, installed-list, and exact-NEVRA row lookups. The query
// layer reads libdnf5 package data under a caller-local Base read lock and only
// publishes installed-package cache state after a complete uncancelled scan.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_internal.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gio/gio.h>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>

namespace dnf_backend_internal {

// -----------------------------------------------------------------------------
// Keep the newest row for one package name and architecture tuple.
// -----------------------------------------------------------------------------
static void
remember_newest_row(std::map<std::string, PackageRow> &rows_by_name_arch, const PackageRow &row)
{
  auto [it, inserted] = rows_by_name_arch.emplace(row.name_arch_key(), row);
  if (!inserted && libdnf5::rpm::evrcmp(row, it->second) > 0) {
    it->second = row;
  }
}

// -----------------------------------------------------------------------------
// Fold UTF-8 package search text before comparing it against libdnf5 metadata
// fields. This keeps manual name and description matching aligned with GTK's
// case-insensitive text handling for non-ASCII package summaries.
// -----------------------------------------------------------------------------
static std::string
utf8_casefold_copy(const std::string &text)
{
  char *folded = g_utf8_casefold(text.c_str(), -1);
  std::string result = folded ? folded : "";
  g_free(folded);
  return result;
}

// -----------------------------------------------------------------------------
// Return true when one package matches the active search term using the same
// name and description flag semantics as the main UI search controls.
// -----------------------------------------------------------------------------
static bool
package_matches_search(const libdnf5::rpm::Package &pkg,
                       const std::string &pattern_lower,
                       const DnfBackendSearchOptions &search_options)
{
  std::string name = utf8_casefold_copy(pkg.get_name());
  if (search_options.exact_match) {
    return name == pattern_lower;
  }

  if (name.find(pattern_lower) != std::string::npos) {
    return true;
  }

  if (!search_options.search_in_description) {
    return false;
  }

  std::string description = utf8_casefold_copy(pkg.get_description());
  return description.find(pattern_lower) != std::string::npos;
}

// -----------------------------------------------------------------------------
// Collect the newest visible repo candidate for each name and architecture tuple. When a
// search term is provided, apply the same name and description filtering as the
// main search flow before deduplicating the results.
// -----------------------------------------------------------------------------
std::map<std::string, PackageRow>
collect_available_rows_by_name_arch(libdnf5::Base &base,
                                    GCancellable *cancellable,
                                    const DnfBackendSearchOptions &search_options,
                                    const std::string *pattern)
{
  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();
  query.filter_latest_evr();

  if (pattern && !search_options.search_in_description) {
    if (search_options.exact_match) {
      query.filter_name(*pattern, libdnf5::sack::QueryCmp::EQ);
    } else {
      query.filter_name(*pattern, libdnf5::sack::QueryCmp::CONTAINS);
    }
  }

  const std::string pattern_lower = pattern ? utf8_casefold_copy(*pattern) : "";
  std::map<std::string, PackageRow> rows_by_name_arch;

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      rows_by_name_arch.clear();
      return rows_by_name_arch;
    }

    if (pattern && search_options.search_in_description &&
        !package_matches_search(pkg, pattern_lower, search_options)) {
      continue;
    }

    // Provenance is UNKNOWN until compared against the installed set. The
    // merge or annotation helpers resolve it when installed rows are available.
    PackageRow row = make_package_row(pkg, PackageRepoCandidateRelation::UNKNOWN);
    remember_newest_row(rows_by_name_arch, row);
  }

  return rows_by_name_arch;
}

// -----------------------------------------------------------------------------
// Collect installed package rows and the corresponding exact NEVRA and name and architecture
// caches in one pass. When a search term is provided, filter the installed list
// with the same search semantics used for repo-backed rows.
// -----------------------------------------------------------------------------
InstalledQueryResult
collect_installed_rows(libdnf5::Base &base,
                       GCancellable *cancellable,
                       const DnfBackendSearchOptions &search_options,
                       const std::string *pattern)
{
  InstalledQueryResult result;
  const std::string pattern_lower = pattern ? utf8_casefold_copy(*pattern) : "";

  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      result.rows.clear();
      result.nevras.clear();
      result.rows_by_name_arch.clear();
      return result;
    }

    if (pattern && !package_matches_search(pkg, pattern_lower, search_options)) {
      continue;
    }

    PackageRow row = make_package_row(pkg);
    result.nevras.insert(row.nevra);
    remember_newest_row(result.rows_by_name_arch, row);
    result.rows.push_back(row);
  }

  return result;
}

// -----------------------------------------------------------------------------
// Compare one installed row against the newest visible repo candidate for the
// same name and architecture tuple and annotate the row with the resolved relationship.
// -----------------------------------------------------------------------------
void
annotate_installed_row_with_repo_candidate(PackageRow &installed_row,
                                           const std::map<std::string, PackageRow> &available_rows)
{
  auto it = available_rows.find(installed_row.name_arch_key());
  if (it == available_rows.end()) {
    installed_row.repo_candidate_relation = PackageRepoCandidateRelation::NONE;
    return;
  }

  int cmp = libdnf5::rpm::evrcmp(it->second, installed_row);
  if (cmp > 0) {
    installed_row.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  } else if (cmp < 0) {
    installed_row.repo_candidate_relation = PackageRepoCandidateRelation::OLDER;
  } else {
    installed_row.repo_candidate_relation = PackageRepoCandidateRelation::SAME;
  }
}

// -----------------------------------------------------------------------------
// Best-effort repo annotation for installed rows. Installed queries should keep
// working from the local rpmdb even when repository metadata is unavailable, so
// failures here only leave repo provenance as UNKNOWN.
// -----------------------------------------------------------------------------
void
annotate_installed_rows_with_repo_candidates_best_effort(std::vector<PackageRow> &installed_rows,
                                                         GCancellable *cancellable,
                                                         const AvailableRowsProvider &available_rows_provider)
{
  if (installed_rows.empty()) {
    return;
  }

  try {
    auto available_rows = available_rows_provider(cancellable);
    if (package_query_cancelled(cancellable)) {
      return;
    }

    for (auto &row : installed_rows) {
      annotate_installed_row_with_repo_candidate(row, available_rows);
    }
  } catch (const std::exception &e) {
    DNFUI_TRACE("Installed row repo annotation skipped: %s", e.what());
  }
}

// -----------------------------------------------------------------------------
// Build the merged package view used by search and browse: start with the
// visible repo-backed candidates, then add installed-only rows for name and architecture
// tuples that are missing from enabled repositories. If an installed package is
// newer than the repo candidate, keep the installed row so the UI can surface
// that state directly.
//
// Note on repo_candidate_relation in the returned rows:
//   - Installed rows that are promoted into the map (LOCAL_ONLY, OLDER, or the
//     installed-newer-than-repo case) carry a fully resolved relation.
//   - Available rows that stay in the map without a matching installed entry
//     keep repo_candidate_relation = UNKNOWN because no installed counterpart
//     was found during this pass.
//   - dnf_backend_get_package_install_state handles UNKNOWN on available rows
//     through its installed-cache EVR comparison fallback.
//
// Code that reads repo_candidate_relation directly should treat UNKNOWN on a
// non-installed row as "no installed counterpart known", not as a failed repo
// lookup.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
visible_rows_from_maps(std::map<std::string, PackageRow> available_rows,
                       const std::map<std::string, PackageRow> &installed_rows)
{
  for (const auto &[key, stored_installed_row] : installed_rows) {
    PackageRow installed_row = stored_installed_row;
    annotate_installed_row_with_repo_candidate(installed_row, available_rows);

    auto visible_it = available_rows.find(key);
    if (visible_it == available_rows.end()) {
      available_rows.emplace(key, installed_row);
      continue;
    }

    if (libdnf5::rpm::evrcmp(installed_row, visible_it->second) > 0) {
      visible_it->second = installed_row;
    }
  }

  std::vector<PackageRow> rows;
  rows.reserve(available_rows.size());
  for (auto &[key, row] : available_rows) {
    rows.push_back(row);
  }
  return rows;
}

} // namespace dnf_backend_internal

using namespace dnf_backend_internal;

// -----------------------------------------------------------------------------
// Search merged repo and installed-only package rows and stop early when the
// task cancellable is set.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_search_package_rows_interruptible(const std::string &pattern, GCancellable *cancellable)
{
  const DnfBackendSearchOptions search_options = dnf_backend_get_search_options();

  std::vector<PackageRow> rows;
  InstalledQueryResult installed_snapshot;
  std::set<std::string> protected_names;
  {
    auto [base, guard, generation] = BaseManager::instance().acquire_read();
    auto available_rows = collect_available_rows_by_name_arch(base, cancellable, search_options, &pattern);
    if (package_query_cancelled(cancellable)) {
      return {};
    }

    InstalledQueryResult filtered_installed = collect_installed_rows(base, cancellable, search_options, &pattern);
    if (package_query_cancelled(cancellable)) {
      return {};
    }

    const DnfBackendSearchOptions snapshot_search_options {};
    installed_snapshot = collect_installed_rows(base, cancellable, snapshot_search_options);
    if (package_query_cancelled(cancellable)) {
      return {};
    }

    protected_names = collect_self_protected_package_names(base);
    rows = visible_rows_from_maps(std::move(available_rows), filtered_installed.rows_by_name_arch);
  }

  publish_installed_snapshot(std::move(installed_snapshot), std::move(protected_names));
  return rows;
}

// -----------------------------------------------------------------------------
// Query installed packages via libdnf5 and return structured rows in one pass.
// The exact-NEVRA cache is updated only after a complete uncancelled scan, so a
// cancelled worker cannot publish a partial installed snapshot.
//
// Thread-safety:
//   The Base read lock and g_installed_mutex must never be held at the same
//   time. Rows are collected while the Base lock is held, then the lock is
//   released before publish_installed_snapshot acquires g_installed_mutex.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_installed_package_rows_interruptible(GCancellable *cancellable)
{
  InstalledQueryResult installed;
  std::set<std::string> protected_names;
  {
    auto [base, guard, generation] = BaseManager::instance().acquire_read();
    const DnfBackendSearchOptions search_options {};
    installed = collect_installed_rows(base, cancellable, search_options);
    if (package_query_cancelled(cancellable)) {
      return {};
    }

    annotate_installed_rows_with_repo_candidates_best_effort(
        installed.rows, cancellable, [&base](GCancellable *annotation_cancellable) {
          const DnfBackendSearchOptions annotation_search_options {};
          return collect_available_rows_by_name_arch(base, annotation_cancellable, annotation_search_options);
        });
    protected_names = collect_self_protected_package_names(base);
  }

  // Publish the new installed-package cache only after a complete uncancelled scan.
  publish_installed_snapshot(installed, protected_names);
  return installed.rows;
}

// -----------------------------------------------------------------------------
// Query the combined browse view via libdnf5. The returned rows include the
// newest available candidate for each package stream plus installed-only local
// RPMs that are missing from enabled repositories.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_browse_package_rows_interruptible(GCancellable *cancellable)
{
  const DnfBackendSearchOptions search_options {};

  std::vector<PackageRow> rows;
  InstalledQueryResult installed;
  std::set<std::string> protected_names;
  {
    auto [base, guard, generation] = BaseManager::instance().acquire_read();
    auto available_rows = collect_available_rows_by_name_arch(base, cancellable, search_options);
    if (package_query_cancelled(cancellable)) {
      return {};
    }

    installed = collect_installed_rows(base, cancellable, search_options);
    if (package_query_cancelled(cancellable)) {
      return {};
    }

    protected_names = collect_self_protected_package_names(base);
    rows = visible_rows_from_maps(std::move(available_rows), installed.rows_by_name_arch);
  }

  publish_installed_snapshot(std::move(installed), std::move(protected_names));
  return rows;
}

// -----------------------------------------------------------------------------
// Query available repo packages that are upgrades to installed packages.
// The returned rows are the available update candidates so selecting one shows
// the version that would be installed by an upgrade transaction.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_upgradeable_package_rows_interruptible(GCancellable *cancellable)
{
  std::vector<PackageRow> rows;
  InstalledQueryResult installed;
  std::set<std::string> protected_names;
  {
    auto [base, guard, generation] = BaseManager::instance().acquire_read();

    libdnf5::rpm::PackageQuery query(base);
    query.filter_available();
    query.filter_upgrades();
    query.filter_latest_evr();

    std::map<std::string, PackageRow> rows_by_name_arch;
    for (auto pkg : query) {
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      PackageRow row = make_package_row(pkg, PackageRepoCandidateRelation::UNKNOWN);
      remember_newest_row(rows_by_name_arch, row);
    }

    installed = collect_installed_rows(base, cancellable, DnfBackendSearchOptions {});
    if (package_query_cancelled(cancellable)) {
      return {};
    }

    protected_names = collect_self_protected_package_names(base);

    rows.reserve(rows_by_name_arch.size());
    for (auto &[key, row] : rows_by_name_arch) {
      rows.push_back(row);
    }
  }

  publish_installed_snapshot(std::move(installed), std::move(protected_names));
  return rows;
}

// -----------------------------------------------------------------------------
// Return installed package rows that exactly match one NEVRA. Repo provenance is
// annotated when repository data is available so pending-action navigation can still show
// local-only or newer-than-repo status when possible.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_installed_package_rows_by_nevra(const std::string &pkg_nevra)
{
  std::vector<PackageRow> packages;

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(pkg_nevra);
  query.filter_installed();

  for (auto pkg : query) {
    packages.push_back(make_package_row(pkg));
  }

  // Scope the annotation query to the package name so we load only the one
  // relevant name and architecture entry instead of the entire available package set.
  const std::string annotation_pattern = packages.empty() ? "" : packages[0].name;
  annotate_installed_rows_with_repo_candidates_best_effort(
      packages, nullptr, [&base, &annotation_pattern](GCancellable *annotation_cancellable) {
        const DnfBackendSearchOptions search_options {};
        return collect_available_rows_by_name_arch(
            base, annotation_cancellable, search_options, annotation_pattern.empty() ? nullptr : &annotation_pattern);
      });

  return packages;
}

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Test-only hook that forces annotation failure and verifies rows retain
// UNKNOWN repo-candidate relation rather than being misclassified.
// -----------------------------------------------------------------------------
bool
dnf_backend_testonly_annotation_fallback_leaves_rows_unknown(std::vector<PackageRow> &rows)
{
  for (auto &row : rows) {
    row.repo_candidate_relation = PackageRepoCandidateRelation::UNKNOWN;
  }

  annotate_installed_rows_with_repo_candidates_best_effort(
      rows, nullptr, [](GCancellable *) -> std::map<std::string, PackageRow> {
        throw std::runtime_error("forced annotation failure");
      });

  return std::all_of(rows.begin(), rows.end(), [](const PackageRow &row) {
    return row.repo_candidate_relation == PackageRepoCandidateRelation::UNKNOWN;
  });
}
#endif

// -----------------------------------------------------------------------------
// Return available package rows that exactly match one NEVRA. This helper stays
// repo-only and is used for install-side pending-action navigation and details
// loading.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_available_package_rows_by_nevra(const std::string &pkg_nevra)
{
  std::vector<PackageRow> packages;

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(pkg_nevra);
  query.filter_available();

  for (auto pkg : query) {
    packages.push_back(make_package_row(pkg));
  }

  return packages;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
