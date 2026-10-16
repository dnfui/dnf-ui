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
#include "i18n.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fnmatch.h>
#include <gio/gio.h>

#include <libdnf5/base/base.hpp>
#include <libdnf5/conf/const.hpp>
#include <libdnf5/repo/repo_query.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <libdnf5/utils/patterns.hpp>

namespace dnf_backend_internal {

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
// Keep provider metadata separate from the Base used by ordinary package queries.
// -----------------------------------------------------------------------------
static TemporaryBaseRead
acquire_interruptible_provider_read(GCancellable *cancellable)
{
  BaseCancelToken cancel_token(cancellable);
  return BaseManager::instance().acquire_provider_read(cancel_token.token());
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

// -----------------------------------------------------------------------------
// Return each package name once.
// Repo annotation only needs candidates for names present in the selected rows.
// -----------------------------------------------------------------------------
static std::vector<std::string>
package_names_from_rows(const std::vector<PackageRow> &package_rows)
{
  std::set<std::string> names;
  for (const auto &row : package_rows) {
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
                       const DnfBackendSearchOptions &search_options,
                       const std::set<std::string> *provider_nevras)
{
  if (search_options.find_provider) {
    return provider_nevras && provider_nevras->count(pkg.get_nevra()) > 0;
  }

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
// Resolve provider matches once, before building merged installed and repository rows.
// -----------------------------------------------------------------------------
std::set<std::string>
collect_provider_matching_nevras(libdnf5::Base &base, const std::string &pattern, GCancellable *cancellable)
{
  // Requesting filelists does not guarantee that each repository supplies them.
  libdnf5::repo::RepoQuery repositories(base);
  repositories.filter_enabled(true);
  repositories.filter_type(libdnf5::repo::Repo::Type::AVAILABLE);
  for (auto repo : repositories) {
    if (package_query_cancelled(cancellable)) {
      return {};
    }
    if (!repo->get_metadata_path(libdnf5::METADATA_TYPE_FILELISTS).empty()) {
      continue;
    }

    libdnf5::rpm::PackageQuery packages(base);
    packages.filter_repo_id(std::vector<std::string> { repo->get_id() });
    if (!packages.empty()) {
      throw std::runtime_error(
          _("Provides search needs file lists that are unavailable in the cached repository metadata."));
    }
  }

  std::set<std::string> matches;
  const auto cmp =
      libdnf5::utils::is_glob_pattern(pattern.c_str()) ? libdnf5::sack::QueryCmp::GLOB : libdnf5::sack::QueryCmp::EQ;
  auto collect = [&](libdnf5::rpm::PackageQuery &query) {
    for (auto pkg : query) {
      if (package_query_cancelled(cancellable)) {
        return;
      }
      matches.insert(pkg.get_nevra());
    }
  };

  libdnf5::rpm::PackageQuery provides(base);
  provides.filter_provides(std::vector<std::string> { pattern }, cmp);
  collect(provides);
  if (package_query_cancelled(cancellable)) {
    return {};
  }

  // Also match file names without requiring callers to know their directories.
  // Absolute paths retain their exact or wildcard semantics.
  if (!pattern.empty()) {
    const bool absolute_path = pattern.front() == '/';
    const std::string file_pattern = absolute_path ? pattern : "*/" + pattern;
    libdnf5::rpm::PackageQuery files(base);
    files.filter_file(std::vector<std::string> { file_pattern }, absolute_path ? cmp : libdnf5::sack::QueryCmp::GLOB);
    collect(files);
  }

  return package_query_cancelled(cancellable) ? std::set<std::string> {} : matches;
}

// -----------------------------------------------------------------------------
// Return exact package IDs matching DNF's installonly package configuration.
// -----------------------------------------------------------------------------
static std::set<std::string>
collect_installonly_nevras(libdnf5::Base &base, GCancellable *cancellable)
{
  std::set<std::string> installonly_nevras;

  libdnf5::rpm::PackageQuery query(base);
  query.filter_installonly();

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      installonly_nevras.clear();
      return installonly_nevras;
    }

    installonly_nevras.insert(pkg.get_nevra());
  }

  return installonly_nevras;
}

// -----------------------------------------------------------------------------
// Convert one libdnf package and attach installonly state from the current Base snapshot.
// -----------------------------------------------------------------------------
static PackageRow
make_package_row_with_installonly(const libdnf5::rpm::Package &pkg,
                                  PackageRepoCandidateRelation repo_candidate_relation,
                                  const std::set<std::string> &installonly_nevras)
{
  PackageRow row = make_package_row(pkg, repo_candidate_relation);
  row.installonly = installonly_nevras.count(row.nevra) > 0;
  return row;
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
                                    const std::string *pattern,
                                    const std::set<std::string> *provider_nevras)
{
  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();
  query.filter_latest_evr();

  const std::string pattern_lower = pattern ? utf8_casefold_copy(*pattern) : "";
  const std::set<std::string> installonly_nevras = collect_installonly_nevras(base, cancellable);
  if (package_query_cancelled(cancellable)) {
    return {};
  }
  std::map<std::string, PackageRow> rows_by_name_arch;

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      rows_by_name_arch.clear();
      return rows_by_name_arch;
    }

    // Use the same case-folded search check as installed rows.
    // Otherwise search casing can change whether an installed row finds its repository candidate.
    if (pattern && !package_matches_search(pkg, pattern_lower, search_options, provider_nevras)) {
      continue;
    }

    // The visible table shows one row per package name and architecture.
    // Repositories can contain more than one EVR, so keep only the newest row before installed rows are merged in.
    // The repo relation is UNKNOWN until compared against the installed set.
    // The merge or annotation helpers resolve it when installed rows are available.
    PackageRow row = make_package_row_with_installonly(pkg, PackageRepoCandidateRelation::UNKNOWN, installonly_nevras);
    row.is_newest_available = true;
    remember_newest_row(rows_by_name_arch, row);
  }

  return rows_by_name_arch;
}

// -----------------------------------------------------------------------------
// Add one available row to an exact-version package view.
// -----------------------------------------------------------------------------
void
add_available_view_row(AvailableViewRows &rows, PackageRow row)
{
  if (rows.row_index_by_nevra.count(row.nevra) > 0) {
    return;
  }

  auto newest_it = rows.newest_available_by_name_arch.find(row.name_arch_key());
  row.is_newest_available =
      newest_it != rows.newest_available_by_name_arch.end() && newest_it->second.nevra == row.nevra;

  remember_newest_row(rows.newest_visible_by_name_arch, row);
  const size_t row_index = rows.rows.size();
  rows.rows.push_back(row);
  rows.row_index_by_nevra.emplace(rows.rows[row_index].nevra, row_index);
}

// -----------------------------------------------------------------------------
// Collect the newest available repo candidate for every package name and architecture tuple.
// This is calculated before search filtering so an older matching version cannot
// become the actionable upgrade target merely because the newest version did not match the search text.
// -----------------------------------------------------------------------------
static std::map<std::string, PackageRow>
collect_newest_available_rows_by_name_arch(libdnf5::Base &base, GCancellable *cancellable)
{
  std::map<std::string, PackageRow> rows_by_name_arch;
  const std::set<std::string> installonly_nevras = collect_installonly_nevras(base, cancellable);
  if (package_query_cancelled(cancellable)) {
    return {};
  }

  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();
  query.filter_latest_evr();

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      rows_by_name_arch.clear();
      return rows_by_name_arch;
    }

    PackageRow row = make_package_row_with_installonly(pkg, PackageRepoCandidateRelation::UNKNOWN, installonly_nevras);
    row.is_newest_available = true;
    remember_newest_row(rows_by_name_arch, row);
  }

  return rows_by_name_arch;
}

// -----------------------------------------------------------------------------
// Collect one visible row per exact available NEVRA.
// Repository copies of the same NEVRA are hidden, but distinct versions remain visible.
// -----------------------------------------------------------------------------
static AvailableViewRows
collect_available_view_rows(libdnf5::Base &base,
                            GCancellable *cancellable,
                            const DnfBackendSearchOptions &search_options,
                            const std::string *pattern,
                            const std::set<std::string> *provider_nevras)
{
  AvailableViewRows result;
  result.newest_available_by_name_arch = collect_newest_available_rows_by_name_arch(base, cancellable);
  if (package_query_cancelled(cancellable)) {
    result = AvailableViewRows {};
    return result;
  }

  const std::string pattern_lower = pattern ? utf8_casefold_copy(*pattern) : "";
  const std::set<std::string> installonly_nevras = collect_installonly_nevras(base, cancellable);
  if (package_query_cancelled(cancellable)) {
    result = AvailableViewRows {};
    return result;
  }

  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      result = AvailableViewRows {};
      return result;
    }

    if (pattern && !package_matches_search(pkg, pattern_lower, search_options, provider_nevras)) {
      continue;
    }

    add_available_view_row(
        result, make_package_row_with_installonly(pkg, PackageRepoCandidateRelation::UNKNOWN, installonly_nevras));
  }

  return result;
}

// -----------------------------------------------------------------------------
// Collect newest repo candidates only for package names present in the selected rows.
// This keeps annotation and exact-package lookup from scanning unrelated repo packages.
// -----------------------------------------------------------------------------
static std::map<std::string, PackageRow>
collect_newest_available_rows_for_package_names(libdnf5::Base &base,
                                                GCancellable *cancellable,
                                                const std::vector<PackageRow> &package_rows)
{
  std::map<std::string, PackageRow> rows_by_name_arch;
  std::vector<std::string> names = package_names_from_rows(package_rows);
  if (names.empty()) {
    return rows_by_name_arch;
  }
  const std::set<std::string> installonly_nevras = collect_installonly_nevras(base, cancellable);
  if (package_query_cancelled(cancellable)) {
    return {};
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

    PackageRow row = make_package_row_with_installonly(pkg, PackageRepoCandidateRelation::UNKNOWN, installonly_nevras);
    row.is_newest_available = true;
    remember_newest_row(rows_by_name_arch, row);
  }

  return rows_by_name_arch;
}

// -----------------------------------------------------------------------------
// Collect exact available package IDs for package names present in the selected rows.
// -----------------------------------------------------------------------------
static std::set<std::string>
collect_repo_nevras_for_package_names(libdnf5::Base &base,
                                      GCancellable *cancellable,
                                      const std::vector<PackageRow> &package_rows)
{
  std::set<std::string> repo_nevras;
  std::vector<std::string> names = package_names_from_rows(package_rows);
  if (names.empty()) {
    return repo_nevras;
  }

  libdnf5::rpm::PackageQuery query(base);
  query.filter_name(names, libdnf5::sack::QueryCmp::EQ);
  // Reinstall needs a repository copy of the installed NEVRA. libdnf's available filter removes installed packages.
  query.filter_repo_id("@System", libdnf5::sack::QueryCmp::NEQ);

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      repo_nevras.clear();
      return repo_nevras;
    }

    repo_nevras.insert(pkg.get_nevra());
  }

  return repo_nevras;
}

// -----------------------------------------------------------------------------
// Mark installed rows whose exact NEVRA is still available from repositories.
// -----------------------------------------------------------------------------
static void
annotate_installed_rows_with_exact_available_nevras(std::vector<PackageRow> &installed_rows,
                                                    const std::set<std::string> &available_nevras)
{
  for (auto &row : installed_rows) {
    row.repo_candidate_exact_available = available_nevras.count(row.nevra) > 0;
  }
}

// -----------------------------------------------------------------------------
// Collect installed package rows and the corresponding exact NEVRA and name and architecture caches in one pass.
// When a search term is provided, filter the installed list with the same search semantics used for repo-backed rows.
// -----------------------------------------------------------------------------
InstalledQueryResult
collect_installed_rows(libdnf5::Base &base,
                       GCancellable *cancellable,
                       const DnfBackendSearchOptions &search_options,
                       const std::string *pattern,
                       const std::set<std::string> *provider_nevras)
{
  InstalledQueryResult result;
  const std::string pattern_lower = pattern ? utf8_casefold_copy(*pattern) : "";
  const std::set<std::string> installonly_nevras = collect_installonly_nevras(base, cancellable);
  if (package_query_cancelled(cancellable)) {
    return result;
  }

  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();

  for (auto pkg : query) {
    if (package_query_cancelled(cancellable)) {
      result.rows.clear();
      result.nevras.clear();
      result.rows_by_name_arch.clear();
      return result;
    }

    if (pattern && !package_matches_search(pkg, pattern_lower, search_options, provider_nevras)) {
      continue;
    }

    PackageRow row = make_package_row_with_installonly(pkg, PackageRepoCandidateRelation::UNKNOWN, installonly_nevras);
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
// Mark exact reinstall availability and update the installed row lookup.
// -----------------------------------------------------------------------------
static void
annotate_installed_result_with_exact_available_nevras(InstalledQueryResult &installed,
                                                      const std::set<std::string> &available_nevras)
{
  annotate_installed_rows_with_exact_available_nevras(installed.rows, available_nevras);
  refresh_installed_row_lookup(installed);
}

// -----------------------------------------------------------------------------
// Add exact reinstall availability when repo metadata is available.
// Installed queries must keep working from the local rpmdb when this fails.
// -----------------------------------------------------------------------------
static void
annotate_installed_result_with_exact_available_nevras_best_effort(libdnf5::Base &base,
                                                                  InstalledQueryResult &installed,
                                                                  GCancellable *cancellable)
{
  try {
    std::set<std::string> available_nevras = collect_repo_nevras_for_package_names(base, cancellable, installed.rows);
    if (package_query_cancelled(cancellable)) {
      return;
    }

    annotate_installed_result_with_exact_available_nevras(installed, available_nevras);
  } catch (const std::exception &e) {
    DNFUI_TRACE("Installed row exact availability annotation skipped: %s", e.what());
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
    installed_row.repo_candidate_epoch.clear();
    installed_row.repo_candidate_version.clear();
    installed_row.repo_candidate_release.clear();
    installed_row.repo_candidate_repo.clear();
    installed_row.repo_candidate_is_newest_available = false;
    return;
  }

  installed_row.repo_candidate_nevra = it->second.nevra;
  installed_row.repo_candidate_epoch = it->second.epoch;
  installed_row.repo_candidate_version = it->second.version;
  installed_row.repo_candidate_release = it->second.release;
  installed_row.repo_candidate_repo = it->second.repo;
  installed_row.repo_candidate_is_newest_available = it->second.is_newest_available;
  if (it->second.nevra == installed_row.nevra) {
    installed_row.repo_candidate_exact_available = true;
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
//   - dnf_backend_resolve_installed_package handles UNKNOWN on available rows
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
// Build an exact-version package view.
// Available rows remain one row per exact NEVRA. Installed NEVRAs that are not already visible are added separately.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
visible_rows_from_available_view(AvailableViewRows available_rows, const InstalledQueryResult &installed)
{
  for (const auto &stored_installed_row : installed.rows) {
    auto visible_it = available_rows.row_index_by_nevra.find(stored_installed_row.nevra);
    if (visible_it != available_rows.row_index_by_nevra.end()) {
      PackageRow &visible_row = available_rows.rows[visible_it->second];
      annotate_installed_row_with_repo_candidate(visible_row, available_rows.newest_visible_by_name_arch);
      visible_row.repo_candidate_exact_available = true;
      continue;
    }

    PackageRow installed_row = stored_installed_row;
    annotate_installed_row_with_repo_candidate(installed_row, available_rows.newest_visible_by_name_arch);
    const size_t row_index = available_rows.rows.size();
    available_rows.rows.push_back(installed_row);
    available_rows.row_index_by_nevra.emplace(available_rows.rows[row_index].nevra, row_index);
  }

  return std::move(available_rows.rows);
}

} // namespace dnf_backend_internal

using namespace dnf_backend_internal;

// -----------------------------------------------------------------------------
// Search merged repo and installed-only package rows and stop early when the task cancellable is set.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_search_package_rows_interruptible(const std::string &pattern,
                                              const DnfBackendSearchOptions &search_options,
                                              GCancellable *cancellable)
{
  std::vector<PackageRow> rows;
  InstalledQueryResult installed_snapshot;
  std::set<std::string> protected_names;
  {
    try {
      std::optional<TemporaryBaseRead> provider_read;
      std::optional<BaseRead> normal_read;
      if (search_options.find_provider) {
        provider_read.emplace(acquire_interruptible_provider_read(cancellable));
      } else {
        normal_read.emplace(acquire_interruptible_base_read(cancellable));
      }
      libdnf5::Base &base = provider_read ? *provider_read->base : normal_read->base;
      // The shared installed snapshot must contain every installed package, not
      // only the rows that matched this search.
      // The UI uses it later for package status, action buttons, and pending action handling.
      const DnfBackendSearchOptions snapshot_search_options {};
      installed_snapshot = collect_installed_rows(base, cancellable, snapshot_search_options);
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      std::set<std::string> available_nevras =
          collect_repo_nevras_for_package_names(base, cancellable, installed_snapshot.rows);
      if (package_query_cancelled(cancellable)) {
        return {};
      }
      annotate_installed_result_with_exact_available_nevras(installed_snapshot, available_nevras);

      protected_names = collect_self_protected_package_names(base);
      std::set<std::string> provider_matches;
      if (search_options.find_provider) {
        provider_matches = collect_provider_matching_nevras(base, pattern, cancellable);
      }
      if (package_query_cancelled(cancellable)) {
        return {};
      }
      const std::set<std::string> *provider_nevras = search_options.find_provider ? &provider_matches : nullptr;
      if (search_options.latest_only) {
        auto available_rows =
            collect_available_rows_by_name_arch(base, cancellable, search_options, &pattern, provider_nevras);
        if (package_query_cancelled(cancellable)) {
          return {};
        }

        // This scan is only for installed rows that match the search term.
        // Those rows can still appear in the visible search result when no repo candidate
        // is shown for the same package name and architecture.
        InstalledQueryResult filtered_installed =
            collect_installed_rows(base, cancellable, search_options, &pattern, provider_nevras);
        if (package_query_cancelled(cancellable)) {
          return {};
        }
        annotate_installed_result_with_exact_available_nevras(filtered_installed, available_nevras);

        rows = visible_rows_from_maps(std::move(available_rows), filtered_installed.rows_by_name_arch);
      } else {
        AvailableViewRows available_rows =
            collect_available_view_rows(base, cancellable, search_options, &pattern, provider_nevras);
        if (package_query_cancelled(cancellable)) {
          return {};
        }

        InstalledQueryResult filtered_installed =
            collect_installed_rows(base, cancellable, search_options, &pattern, provider_nevras);
        if (package_query_cancelled(cancellable)) {
          return {};
        }
        annotate_installed_result_with_exact_available_nevras(filtered_installed, available_nevras);

        rows = visible_rows_from_available_view(std::move(available_rows), filtered_installed);
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
      auto [base, guard] = acquire_interruptible_base_read(cancellable);
      const DnfBackendSearchOptions search_options {};
      installed = collect_installed_rows(base, cancellable, search_options);
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      annotate_installed_result_with_exact_available_nevras_best_effort(base, installed, cancellable);

      // Installed listing is allowed to work from the local rpmdb alone.
      // Repo annotation adds upgrade and local-only status when repo metadata
      // is available, but it must not make the installed list fail.
      annotate_installed_rows_with_repo_candidates_best_effort(
          installed.rows, cancellable, [&base, &installed](GCancellable *annotation_cancellable) {
            return collect_newest_available_rows_for_package_names(base, annotation_cancellable, installed.rows);
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
dnf_backend_get_browse_package_rows_interruptible(const DnfBackendSearchOptions &search_options,
                                                  GCancellable *cancellable)
{
  std::vector<PackageRow> rows;
  InstalledQueryResult installed;
  std::set<std::string> protected_names;
  {
    try {
      auto [base, guard] = acquire_interruptible_base_read(cancellable);

      // Browse uses the full installed set because installed-only packages must
      // still be visible even when no enabled repo contains them.
      installed = collect_installed_rows(base, cancellable, search_options);
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      std::set<std::string> available_nevras = collect_repo_nevras_for_package_names(base, cancellable, installed.rows);
      if (package_query_cancelled(cancellable)) {
        return {};
      }
      annotate_installed_result_with_exact_available_nevras(installed, available_nevras);

      protected_names = collect_self_protected_package_names(base);
      if (search_options.latest_only) {
        auto available_rows = collect_available_rows_by_name_arch(base, cancellable, search_options);
        if (package_query_cancelled(cancellable)) {
          return {};
        }
        rows = visible_rows_from_maps(std::move(available_rows), installed.rows_by_name_arch);
      } else {
        AvailableViewRows available_rows =
            collect_available_view_rows(base, cancellable, search_options, nullptr, nullptr);
        if (package_query_cancelled(cancellable)) {
          return {};
        }
        rows = visible_rows_from_available_view(std::move(available_rows), installed);
      }
    } catch (const BaseOperationCancelled &) {
      return {};
    }
  }

  publish_installed_snapshot(std::move(installed), std::move(protected_names));
  return rows;
}

// -----------------------------------------------------------------------------
// Return available package metadata for exact daemon-selected NEVRAs.
// The caller has already decided which packages are upgrade targets.
// This helper only adds metadata for those exact package IDs.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_available_package_metadata_by_nevras_interruptible(const std::vector<std::string> &nevras,
                                                                   GCancellable *cancellable)
{
  std::vector<PackageRow> rows;
  std::set<std::string> requested_nevras(nevras.begin(), nevras.end());
  std::set<std::string> returned_nevras;

  if (requested_nevras.empty()) {
    return rows;
  }

  try {
    auto [base, guard] = acquire_interruptible_base_read(cancellable);
    const std::set<std::string> installonly_nevras = collect_installonly_nevras(base, cancellable);
    if (package_query_cancelled(cancellable)) {
      return {};
    }

    libdnf5::rpm::PackageQuery query(base);
    query.filter_available();

    for (auto pkg : query) {
      if (package_query_cancelled(cancellable)) {
        return {};
      }

      PackageRow row =
          make_package_row_with_installonly(pkg, PackageRepoCandidateRelation::UNKNOWN, installonly_nevras);
      if (requested_nevras.count(row.nevra) == 0 || returned_nevras.count(row.nevra) > 0) {
        continue;
      }

      rows.push_back(row);
      returned_nevras.insert(row.nevra);
    }
  } catch (const BaseOperationCancelled &) {
    return {};
  }

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

  auto [base, guard] = BaseManager::instance().acquire_read();
  const std::set<std::string> installonly_nevras = collect_installonly_nevras(base, nullptr);
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(pkg_nevra);
  query.filter_installed();

  for (auto pkg : query) {
    packages.push_back(
        make_package_row_with_installonly(pkg, PackageRepoCandidateRelation::UNKNOWN, installonly_nevras));
  }

  annotate_installed_rows_with_repo_candidates_best_effort(
      packages, nullptr, [&base, &packages](GCancellable *annotation_cancellable) {
        return collect_newest_available_rows_for_package_names(base, annotation_cancellable, packages);
      });
  InstalledQueryResult installed;
  installed.rows = packages;
  annotate_installed_result_with_exact_available_nevras_best_effort(base, installed, nullptr);
  packages = std::move(installed.rows);

  return packages;
}

// -----------------------------------------------------------------------------
// Return available package rows that exactly match one NEVRA.
// This helper stays repo-only and is used for install-side pending-action navigation and details loading.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_available_package_rows_by_nevra(const std::string &pkg_nevra)
{
  AvailableViewRows visible_rows;

  auto [base, guard] = BaseManager::instance().acquire_read();
  const std::set<std::string> installonly_nevras = collect_installonly_nevras(base, nullptr);
  {
    libdnf5::rpm::PackageQuery query(base);
    query.filter_nevra(pkg_nevra);
    query.filter_available();

    for (auto pkg : query) {
      add_available_view_row(
          visible_rows,
          make_package_row_with_installonly(pkg, PackageRepoCandidateRelation::UNKNOWN, installonly_nevras));
    }
  }

  visible_rows.newest_available_by_name_arch =
      collect_newest_available_rows_for_package_names(base, nullptr, visible_rows.rows);
  for (auto &row : visible_rows.rows) {
    auto newest_it = visible_rows.newest_available_by_name_arch.find(row.name_arch_key());
    row.is_newest_available =
        newest_it != visible_rows.newest_available_by_name_arch.end() && newest_it->second.nevra == row.nevra;
  }

  return visible_rows.rows;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
