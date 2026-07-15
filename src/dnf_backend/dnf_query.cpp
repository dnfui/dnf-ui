// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_query.cpp
// Package row query and merge helpers
//
// Owns browse, search, installed-list, and exact-NEVRA row lookups. The query
// layer reads libdnf5 package data under a caller-local Base read lock and only
// publishes installed-package cache state after a complete uncancelled scan.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_internal.hpp"

#include "dnf_backend/base_manager.hpp"
#include "debug_trace.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fnmatch.h>
#include <gio/gio.h>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>

namespace dnf_backend_internal {

#ifdef DNFUI_DEBUG_TRACE
static long long
elapsed_ms_since(gint64 started_at_us)
{
  return static_cast<long long>((g_get_monotonic_time() - started_at_us) / 1000);
}
#endif

// -----------------------------------------------------------------------------
// Bridge a GCancellable into the atomic token used by BaseManager.
// GLib cancellation is signalled through GCancellable.
// BaseManager cannot use that type directly, so query workers pass this small atomic flag instead.
// -----------------------------------------------------------------------------
class BaseCancelToken {
  public:
  explicit BaseCancelToken(GCancellable *cancellable)
      : cancellable(cancellable)
      , cancel_requested(std::make_shared<std::atomic<bool>>(package_query_cancelled(cancellable)))
  {
    if (cancellable) {
      handler_id = g_cancellable_connect(cancellable, G_CALLBACK(on_cancelled), cancel_requested.get(), nullptr);
    }
  }

  ~BaseCancelToken()
  {
    // Disconnect before the object is destroyed.
    // Otherwise GLib could call back with a pointer to an atomic flag that no longer exists.
    if (cancellable && handler_id != 0) {
      g_cancellable_disconnect(cancellable, handler_id);
    }
  }

  std::shared_ptr<std::atomic<bool>> token() const
  {
    return cancel_requested;
  }

  private:
  // Runs on the thread that cancels the task and wakes BaseManager checks waiting for a package list operation to stop.
  static void on_cancelled(GCancellable *, gpointer user_data)
  {
    auto *cancelled = static_cast<std::atomic<bool> *>(user_data);
    if (cancelled) {
      cancelled->store(true, std::memory_order_relaxed);
    }
  }

  GCancellable *cancellable = nullptr;
  gulong handler_id = 0;
  // Shared with BaseManager while acquiring or initializing the shared Base.
  std::shared_ptr<std::atomic<bool>> cancel_requested;
};

// -----------------------------------------------------------------------------
// Return Base access for a query worker that can still be stopped.
// -----------------------------------------------------------------------------
static BaseRead
acquire_interruptible_base_read(GCancellable *cancellable)
{
  BaseCancelToken cancel_token(cancellable);
  return BaseManager::instance().acquire_read(cancel_token.token());
}

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

struct AvailableViewRows {
  std::vector<PackageRow> rows;
  std::map<std::string, PackageRow> newest_by_name_arch;
  std::set<std::string> available_nevras;
};

// -----------------------------------------------------------------------------
// Return each installed package name once.
// Repo annotation only needs candidates for names that are already installed.
// -----------------------------------------------------------------------------
static std::vector<std::string>
installed_package_names(const std::vector<PackageRow> &installed_rows)
{
  std::set<std::string> names;
  for (const auto &row : installed_rows) {
    names.insert(row.name);
  }

  return { names.begin(), names.end() };
}

// -----------------------------------------------------------------------------
// Fold UTF-8 package search text before comparing it against libdnf5 metadata fields.
// This keeps manual name and description matching aligned with GTK's
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
// Return true when the user search term asks for shell-style name matching.
// Exact search stays literal, so wildcard characters only matter in normal search.
// -----------------------------------------------------------------------------
static bool
search_pattern_uses_wildcards(const std::string &pattern)
{
  return pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos;
}

// -----------------------------------------------------------------------------
// Match folded package text against the folded search term.
// Normal search is substring based unless the term contains wildcard characters.
// -----------------------------------------------------------------------------
static bool
search_text_matches_pattern(const std::string &text_lower, const std::string &pattern_lower, bool use_wildcards)
{
  if (use_wildcards) {
    return fnmatch(pattern_lower.c_str(), text_lower.c_str(), 0) == 0;
  }

  return text_lower.find(pattern_lower) != std::string::npos;
}

// -----------------------------------------------------------------------------
// Return true when one package matches the active search term using the same
// name and description flag semantics as the main UI search controls.
// Summary text is included with description search because it is shown as package
// descriptive text in the table.
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

  const bool use_wildcards = search_pattern_uses_wildcards(pattern_lower);
  if (search_text_matches_pattern(name, pattern_lower, use_wildcards)) {
    return true;
  }

  if (!search_options.search_in_description) {
    return false;
  }

  std::string summary = utf8_casefold_copy(pkg.get_summary());
  if (search_text_matches_pattern(summary, pattern_lower, use_wildcards)) {
    return true;
  }

  std::string description = utf8_casefold_copy(pkg.get_description());
  return search_text_matches_pattern(description, pattern_lower, use_wildcards);
}

// -----------------------------------------------------------------------------
// Collect the newest visible repo candidate for each name and architecture tuple.
// When a search term is provided, apply the same name and description filtering
// as the main search flow before deduplicating the results.
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

  const std::string pattern_lower = pattern ? utf8_casefold_copy(*pattern) : "";
  std::map<std::string, PackageRow> rows_by_name_arch;

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      rows_by_name_arch.clear();
      return rows_by_name_arch;
    }

    // Use the same case-folded search check as installed rows.
    // Otherwise search casing can change whether an installed row finds its repository candidate.
    if (pattern && !package_matches_search(pkg, pattern_lower, search_options)) {
      continue;
    }

    // The visible table shows one row per package name and architecture.
    // Repositories can contain more than one EVR, so keep only the newest row before installed rows are merged in.
    // The repo relation is UNKNOWN until compared against the installed set.
    // The merge or annotation helpers resolve it when installed rows are available.
    PackageRow row = make_package_row(pkg, PackageRepoCandidateRelation::UNKNOWN);
    remember_newest_row(rows_by_name_arch, row);
  }

  return rows_by_name_arch;
}

// -----------------------------------------------------------------------------
// Collect available rows for the browse and search table.
// With Latest only enabled, keep the same one-row-per-name-and-architecture view
// used by the default package table. With it disabled, keep every available NEVRA so an
// older package version can be selected explicitly.
// -----------------------------------------------------------------------------
static AvailableViewRows
collect_available_rows_for_view(libdnf5::Base &base,
                                GCancellable *cancellable,
                                const DnfBackendSearchOptions &search_options,
                                const std::string *pattern)
{
  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();
  if (search_options.latest_only) {
    query.filter_latest_evr();
  }

  const std::string pattern_lower = pattern ? utf8_casefold_copy(*pattern) : "";
  AvailableViewRows result;

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      result.rows.clear();
      result.newest_by_name_arch.clear();
      result.available_nevras.clear();
      return result;
    }

    if (pattern && !package_matches_search(pkg, pattern_lower, search_options)) {
      continue;
    }

    PackageRow row = make_package_row(pkg, PackageRepoCandidateRelation::UNKNOWN);
    remember_newest_row(result.newest_by_name_arch, row);
    result.available_nevras.insert(row.nevra);
    if (search_options.latest_only) {
      continue;
    }

    result.rows.push_back(row);
  }

  if (search_options.latest_only) {
    result.rows.reserve(result.newest_by_name_arch.size());
    for (const auto &entry : result.newest_by_name_arch) {
      result.rows.push_back(entry.second);
    }
  }

  return result;
}

// -----------------------------------------------------------------------------
// Collect repo candidates only for package names that are installed locally.
// This keeps installed-list annotation from scanning unrelated repo packages.
// -----------------------------------------------------------------------------
static std::map<std::string, PackageRow>
collect_available_rows_for_installed_names(libdnf5::Base &base,
                                           GCancellable *cancellable,
                                           const std::vector<PackageRow> &installed_rows)
{
  std::map<std::string, PackageRow> rows_by_name_arch;
  std::vector<std::string> names = installed_package_names(installed_rows);
  if (names.empty()) {
    return rows_by_name_arch;
  }

  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();
  query.filter_name(names, libdnf5::sack::QueryCmp::EQ);
  query.filter_latest_evr();

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      rows_by_name_arch.clear();
      return rows_by_name_arch;
    }

    PackageRow row = make_package_row(pkg, PackageRepoCandidateRelation::UNKNOWN);
    remember_newest_row(rows_by_name_arch, row);
  }

  return rows_by_name_arch;
}

// -----------------------------------------------------------------------------
// Collect installed package rows and the corresponding exact NEVRA and name and architecture caches in one pass.
// When a search term is provided, filter the installed list with the same search semantics used for repo-backed rows.
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
    // The exact NEVRA set answers "is this precise package installed".
    // The name and architecture map answers "which installed package matches
    // this available update candidate".
    result.nevras.insert(row.nevra);
    remember_newest_row(result.rows_by_name_arch, row);
    result.rows.push_back(row);
  }

  return result;
}

// -----------------------------------------------------------------------------
// Rebuild the installed name and architecture lookup from the current row data.
// Call this after changing installed rows so the published lookup matches the rows returned to the UI.
// -----------------------------------------------------------------------------
static void
refresh_installed_row_lookup(InstalledQueryResult &installed)
{
  installed.rows_by_name_arch.clear();
  for (const auto &row : installed.rows) {
    remember_newest_row(installed.rows_by_name_arch, row);
  }
}

// -----------------------------------------------------------------------------
// Compare one installed row against the newest visible repo candidate for the same name and architecture tuple.
// Store the resolved relation on the installed row.
// -----------------------------------------------------------------------------
void
annotate_installed_row_with_repo_candidate(PackageRow &installed_row,
                                           const std::map<std::string, PackageRow> &available_rows)
{
  auto it = available_rows.find(installed_row.name_arch_key());
  if (it == available_rows.end()) {
    installed_row.repo_candidate_relation = PackageRepoCandidateRelation::NONE;
    installed_row.repo_candidate_nevra.clear();
    installed_row.repo_candidate_version.clear();
    installed_row.repo_candidate_release.clear();
    installed_row.repo_candidate_repo.clear();
    return;
  }

  installed_row.repo_candidate_nevra = it->second.nevra;
  installed_row.repo_candidate_version = it->second.version;
  installed_row.repo_candidate_release = it->second.release;
  installed_row.repo_candidate_repo = it->second.repo;
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
// Add repo-candidate state to installed rows when repo metadata is available.
// Installed queries must keep working from the local rpmdb, so failures here leave the repo relation as UNKNOWN.
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
// Build the merged package view used by search and browse.
// Start with visible repo-backed candidates, then add installed-only rows for missing name and architecture tuples.
// If an installed package is newer than the repo candidate,
// keep the installed row so the UI can show that state directly.
//
// Note on repo_candidate_relation in the returned rows:
//   - Installed rows promoted into the map carry a fully resolved relation.
//     This covers LOCAL_ONLY, OLDER, and installed-newer-than-repo rows.
//   - Available rows that stay in the map without a matching installed entry
//     keep repo_candidate_relation = UNKNOWN because no installed counterpart
//     was found during this pass.
//   - dnf_backend_get_package_install_state handles UNKNOWN on available rows
//     through its installed-cache EVR comparison fallback.
//
// Code that reads repo_candidate_relation directly should treat UNKNOWN on a non-installed row
// as "no installed counterpart known", not as a failed repo lookup.
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

// -----------------------------------------------------------------------------
// Build the all-versions browse view.
// This keeps every available row and adds installed rows only when the installed
// NEVRA is not already visible from a repository.
// -----------------------------------------------------------------------------
static std::vector<PackageRow>
visible_rows_from_available_view(AvailableViewRows available_rows,
                                 const std::map<std::string, PackageRow> &installed_rows)
{
  std::vector<PackageRow> rows = std::move(available_rows.rows);

  for (const auto &entry : installed_rows) {
    const PackageRow &stored_installed_row = entry.second;
    if (available_rows.available_nevras.count(stored_installed_row.nevra) > 0) {
      continue;
    }

    PackageRow installed_row = stored_installed_row;
    annotate_installed_row_with_repo_candidate(installed_row, available_rows.newest_by_name_arch);
    rows.push_back(installed_row);
  }

  return rows;
}

} // namespace dnf_backend_internal

using namespace dnf_backend_internal;

// -----------------------------------------------------------------------------
// Search merged repo and installed-only package rows and stop early when the task cancellable is set.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_search_package_rows_interruptible(const std::string &pattern, GCancellable *cancellable)
{
  const DnfBackendSearchOptions search_options = dnf_backend_get_search_options();

  std::vector<PackageRow> rows;
  InstalledQueryResult installed_snapshot;
  std::set<std::string> protected_names;
  {
    try {
      auto [base, guard, generation] = acquire_interruptible_base_read(cancellable);
      auto available_rows = collect_available_rows_for_view(base, cancellable, search_options, &pattern);
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      // This scan is only for installed rows that match the search term.
      // Those rows can still appear in the visible search result when no repo candidate
      // is shown for the same package name and architecture.
      InstalledQueryResult filtered_installed = collect_installed_rows(base, cancellable, search_options, &pattern);
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      // The shared installed snapshot must contain every installed package, not
      // only the rows that matched this search.
      // The UI uses it later for package status, action buttons, and pending action handling.
      const DnfBackendSearchOptions snapshot_search_options {};
      installed_snapshot = collect_installed_rows(base, cancellable, snapshot_search_options);
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      protected_names = collect_self_protected_package_names(base);
      if (search_options.latest_only) {
        rows =
            visible_rows_from_maps(std::move(available_rows.newest_by_name_arch), filtered_installed.rows_by_name_arch);
      } else {
        rows = visible_rows_from_available_view(std::move(available_rows), filtered_installed.rows_by_name_arch);
      }
    } catch (const BaseOperationCancelled &) {
      return {};
    }
  }

  publish_installed_snapshot(std::move(installed_snapshot), std::move(protected_names));
  return rows;
}

// -----------------------------------------------------------------------------
// Query installed packages via libdnf5 and return structured rows in one pass.
// The exact-NEVRA cache is updated only after a complete uncancelled scan.
// A cancelled worker cannot publish a partial installed snapshot.
//
// Thread-safety:
//   The Base read lock and g_installed_mutex must never be held at the same time.
//   Rows are collected while the Base lock is held, then the lock is
//   released before publish_installed_snapshot acquires g_installed_mutex.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_installed_package_rows_interruptible(GCancellable *cancellable)
{
  InstalledQueryResult installed;
  std::set<std::string> protected_names;
  {
    try {
      auto [base, guard, generation] = acquire_interruptible_base_read(cancellable);
      const DnfBackendSearchOptions search_options {};
      installed = collect_installed_rows(base, cancellable, search_options);
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      // Installed listing is allowed to work from the local rpmdb alone.
      // Repo annotation adds upgrade and local-only status when repo metadata
      // is available, but it must not make the installed list fail.
      annotate_installed_rows_with_repo_candidates_best_effort(
          installed.rows, cancellable, [&base, &installed](GCancellable *annotation_cancellable) {
            return collect_available_rows_for_installed_names(base, annotation_cancellable, installed.rows);
          });
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      refresh_installed_row_lookup(installed);
      protected_names = collect_self_protected_package_names(base);
    } catch (const BaseOperationCancelled &) {
      return {};
    }
  }

  // Publish the new installed-package cache only after a complete uncancelled scan.
  publish_installed_snapshot(installed, protected_names);
  return installed.rows;
}

// -----------------------------------------------------------------------------
// Query the combined browse view via libdnf5.
// The returned rows include the newest available candidate for each package stream.
// Installed RPMs missing from enabled repositories are included as local-only rows.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_browse_package_rows_interruptible(GCancellable *cancellable,
                                                  const DnfBackendSearchOptions &search_options)
{
  std::vector<PackageRow> rows;
  InstalledQueryResult installed;
  std::set<std::string> protected_names;
  {
    try {
      auto [base, guard, generation] = acquire_interruptible_base_read(cancellable);
      auto available_rows = collect_available_rows_for_view(base, cancellable, search_options, nullptr);
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      // Browse uses the full installed set because installed-only packages must
      // still be visible even when no enabled repo contains them.
      installed = collect_installed_rows(base, cancellable, search_options);
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      protected_names = collect_self_protected_package_names(base);
      if (search_options.latest_only) {
        rows = visible_rows_from_maps(std::move(available_rows.newest_by_name_arch), installed.rows_by_name_arch);
      } else {
        rows = visible_rows_from_available_view(std::move(available_rows), installed.rows_by_name_arch);
      }
    } catch (const BaseOperationCancelled &) {
      return {};
    }
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
#ifdef DNFUI_DEBUG_TRACE
  const gint64 started_at_us = g_get_monotonic_time();
  DNFUI_TRACE("Upgradable query start");
#endif

  std::vector<PackageRow> rows;
  InstalledQueryResult installed;
  std::set<std::string> protected_names;
  {
    try {
#ifdef DNFUI_DEBUG_TRACE
      const gint64 base_started_at_us = g_get_monotonic_time();
#endif
      auto [base, guard, generation] = acquire_interruptible_base_read(cancellable);
#ifdef DNFUI_DEBUG_TRACE
      DNFUI_TRACE("Upgradable query base ready elapsed_ms=%lld total_ms=%lld",
                  elapsed_ms_since(base_started_at_us),
                  elapsed_ms_since(started_at_us));
#endif

#ifdef DNFUI_DEBUG_TRACE
      const gint64 upgrade_query_started_at_us = g_get_monotonic_time();
#endif
      libdnf5::rpm::PackageQuery query(base);
      query.filter_available();
      query.filter_upgrades();
      query.filter_latest_evr();

      // libdnf returns the available package that would satisfy each upgrade.
      // That is the correct row to show here because the main action installs
      // this newer package over the currently installed one.
      std::map<std::string, PackageRow> rows_by_name_arch;
      for (auto pkg : query) {
        if (package_query_cancelled(cancellable)) {
          return {};
        }

        PackageRow row = make_package_row(pkg, PackageRepoCandidateRelation::UNKNOWN);
        remember_newest_row(rows_by_name_arch, row);
      }
#ifdef DNFUI_DEBUG_TRACE
      DNFUI_TRACE("Upgradable query libdnf candidates=%zu elapsed_ms=%lld total_ms=%lld",
                  rows_by_name_arch.size(),
                  elapsed_ms_since(upgrade_query_started_at_us),
                  elapsed_ms_since(started_at_us));
#endif

      // Even though visible rows are available update candidates, the UI still needs the installed snapshot.
      // That snapshot resolves remove and reinstall actions back to the currently installed package.
#ifdef DNFUI_DEBUG_TRACE
      const gint64 installed_started_at_us = g_get_monotonic_time();
#endif
      installed = collect_installed_rows(base, cancellable, DnfBackendSearchOptions {});
      if (package_query_cancelled(cancellable)) {
        return {};
      }
#ifdef DNFUI_DEBUG_TRACE
      DNFUI_TRACE("Upgradable query installed snapshot rows=%zu elapsed_ms=%lld total_ms=%lld",
                  installed.rows.size(),
                  elapsed_ms_since(installed_started_at_us),
                  elapsed_ms_since(started_at_us));
#endif

#ifdef DNFUI_DEBUG_TRACE
      const gint64 protected_started_at_us = g_get_monotonic_time();
#endif
      protected_names = collect_self_protected_package_names(base);
#ifdef DNFUI_DEBUG_TRACE
      DNFUI_TRACE("Upgradable query self protection names=%zu elapsed_ms=%lld total_ms=%lld",
                  protected_names.size(),
                  elapsed_ms_since(protected_started_at_us),
                  elapsed_ms_since(started_at_us));
#endif

      rows.reserve(rows_by_name_arch.size());
      for (auto &[key, row] : rows_by_name_arch) {
        rows.push_back(row);
      }
    } catch (const BaseOperationCancelled &) {
      return {};
    }
  }

#ifdef DNFUI_DEBUG_TRACE
  const gint64 publish_started_at_us = g_get_monotonic_time();
#endif
  publish_installed_snapshot(std::move(installed), std::move(protected_names));
#ifdef DNFUI_DEBUG_TRACE
  DNFUI_TRACE("Upgradable query done rows=%zu publish_ms=%lld total_ms=%lld",
              rows.size(),
              elapsed_ms_since(publish_started_at_us),
              elapsed_ms_since(started_at_us));
#endif
  return rows;
}

// -----------------------------------------------------------------------------
// Return installed package rows that exactly match one NEVRA.
// Repo relation is added when repository data is available, so pending-action navigation
// can still show local-only or newer-than-repo status when possible.
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
// Return available package rows that exactly match one NEVRA.
// This helper stays repo-only and is used for install-side pending-action navigation and details loading.
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
