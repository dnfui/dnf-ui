// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_internal.hpp
// Internal libdnf5 backend implementation helpers
//
// The public backend contract lives in the backend facade header. This header is
// shared by backend implementation units and backend tests so the app-facing API
// can stay small while query, details, and state-cache code remain in separate files.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gio/gio.h>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>

namespace dnf_backend_internal {

// Collect packages that provide a capability or own a requested path.
std::set<std::string>
collect_provider_matching_nevras(libdnf5::Base &base, const std::string &pattern, GCancellable *cancellable);

// Installed package scan result published into the shared UI cache only after a full uncancelled scan.
// Keeping rows, exact NEVRAs, and name and architecture lookup together avoids partial cache updates.
struct InstalledQueryResult {
  std::vector<PackageRow> rows;
  std::set<std::string> nevras;
  std::map<std::string, PackageRow> rows_by_name_arch;
};

struct AvailableViewRows {
  std::vector<PackageRow> rows;
  std::map<std::string, PackageRow> newest_visible_by_name_arch;
  std::map<std::string, PackageRow> newest_available_by_name_arch;
  std::map<std::string, size_t> row_index_by_nevra;
};

using AvailableRowsProvider = std::function<std::map<std::string, PackageRow>(GCancellable *)>;

// -----------------------------------------------------------------------------
// Convert one libdnf5 package object to the backend-owned presentation row.
// -----------------------------------------------------------------------------
PackageRow
make_package_row(const libdnf5::rpm::Package &pkg,
                 PackageRepoCandidateRelation repo_candidate_relation = PackageRepoCandidateRelation::UNKNOWN);

// -----------------------------------------------------------------------------
// Return true when the active package query task was cancelled by the UI.
// -----------------------------------------------------------------------------
bool package_query_cancelled(GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Collect query rows keyed by package name and architecture.
// The caller supplies the Base so related libdnf5 queries stay under the same Base lock.
// -----------------------------------------------------------------------------
std::map<std::string, PackageRow>
collect_available_rows_by_name_arch(libdnf5::Base &base,
                                    GCancellable *cancellable,
                                    const DnfBackendSearchOptions &search_options,
                                    const std::string *pattern = nullptr,
                                    const std::set<std::string> *provider_nevras = nullptr);
// Collect installed rows and exact NEVRA cache data in one scan.
// -----------------------------------------------------------------------------
InstalledQueryResult collect_installed_rows(libdnf5::Base &base,
                                            GCancellable *cancellable,
                                            const DnfBackendSearchOptions &search_options,
                                            const std::string *pattern = nullptr,
                                            const std::set<std::string> *provider_nevras = nullptr);

// -----------------------------------------------------------------------------
// Repo-candidate annotation and row merge helpers used by query code and tests.
// -----------------------------------------------------------------------------
void annotate_installed_row_with_repo_candidate(PackageRow &installed_row,
                                                const std::map<std::string, PackageRow> &available_rows);
// -----------------------------------------------------------------------------
// Annotate installed rows with repo candidate state when repo data is available.
// -----------------------------------------------------------------------------
void annotate_installed_rows_with_repo_candidates_best_effort(std::vector<PackageRow> &installed_rows,
                                                              GCancellable *cancellable,
                                                              const AvailableRowsProvider &available_rows_provider);
// -----------------------------------------------------------------------------
// Add one available row to an exact-version package view.
// -----------------------------------------------------------------------------
void add_available_view_row(AvailableViewRows &rows, PackageRow row);
// -----------------------------------------------------------------------------
// Merge exact-version available rows with missing exact installed rows.
// -----------------------------------------------------------------------------
std::vector<PackageRow> visible_rows_from_available_view(AvailableViewRows available_rows,
                                                         const InstalledQueryResult &installed);
// -----------------------------------------------------------------------------
// Merge available and installed row maps into the visible package list.
// -----------------------------------------------------------------------------
std::vector<PackageRow> visible_rows_from_maps(std::map<std::string, PackageRow> available_rows,
                                               const std::map<std::string, PackageRow> &installed_rows);

// -----------------------------------------------------------------------------
// State-cache helpers owned by dnf_state.cpp and used by query refresh paths.
// -----------------------------------------------------------------------------
std::set<std::string> collect_self_protected_package_names(libdnf5::Base &base);
// -----------------------------------------------------------------------------
// Publish a completed installed-package scan to shared backend state.
// Returns true when installed identities or self-protected package names changed.
// -----------------------------------------------------------------------------
bool publish_installed_snapshot(InstalledQueryResult installed, std::set<std::string> protected_names);
// -----------------------------------------------------------------------------
// Publish a local rpmdb-only installed scan while preserving repo-derived fields
// that still describe the same exact installed package.
// -----------------------------------------------------------------------------
bool publish_local_installed_snapshot(InstalledQueryResult installed, std::set<std::string> protected_names);

} // namespace dnf_backend_internal

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
