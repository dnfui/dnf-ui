// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_internal.hpp
// Internal libdnf5 backend implementation helpers
//
// The public backend contract lives in the backend facade header. This header is
// shared only by the backend implementation units so the
// app-facing API can stay small while query, details, state-cache, and
// transaction code remain in separate files.
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

// Installed package scan result published into the shared UI cache only after a
// full uncancelled scan. Keeping rows, exact NEVRAs, and name and architecture lookup
// together avoids partial cache updates.
struct InstalledQueryResult {
  std::vector<PackageRow> rows;
  std::set<std::string> nevras;
  std::map<std::string, PackageRow> rows_by_name_arch;
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
// Collect query rows keyed by package name and architecture. These helpers intentionally
// require a caller-supplied Base reference so the caller controls the Base lock
// while related libdnf5 queries run.
// -----------------------------------------------------------------------------
std::map<std::string, PackageRow> collect_available_rows_by_name_arch(libdnf5::Base &base,
                                                                      GCancellable *cancellable,
                                                                      const DnfBackendSearchOptions &search_options,
                                                                      const std::string *pattern = nullptr);
// -----------------------------------------------------------------------------
// Collect installed rows and exact NEVRA cache data in one scan.
// -----------------------------------------------------------------------------
InstalledQueryResult collect_installed_rows(libdnf5::Base &base,
                                            GCancellable *cancellable,
                                            const DnfBackendSearchOptions &search_options,
                                            const std::string *pattern = nullptr);

// -----------------------------------------------------------------------------
// Repo-candidate annotation and browse and search merge helpers shared by query and
// test-only fallback paths.
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
// -----------------------------------------------------------------------------
void publish_installed_snapshot(InstalledQueryResult installed, std::set<std::string> protected_names);

} // namespace dnf_backend_internal

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
