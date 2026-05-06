// src/dnf_backend/dnf_backend.hpp
// Public libdnf5 backend facade
//
// This header is the app-facing contract for the libdnf5 integration. It keeps
// libdnf5 types out of the GTK controller layer by exposing small value models
// and string-based transaction specs, while the implementation owns Base access,
// rpmdb and repository queries, EVR comparison, cache publication, and transaction
// resolution.
//
// Callers should depend only on the types and functions declared here. Helpers
// under the internal backend header are private implementation details for
// the backend translation units and may change whenever the backend internals
// are reorganized.
#pragma once

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <gio/gio.h>

// -----------------------------------------------------------------------------
// libdnf5 backend helpers
// -----------------------------------------------------------------------------
// Structured package metadata used by the GTK presentation layer.
// Keeps the full NEVRA for internal selection and transactions while exposing
// friendlier fields for list and column-based views. The repo candidate relation
// describes how the installed row compares to the newest visible repo-backed
// candidate for the same name and architecture tuple:
//   UNKNOWN: repo provenance was not checked or could not be resolved
//   NONE: no visible repo candidate exists for that name and architecture tuple
//   SAME: installed and visible repo candidate resolve to the same EVR
//   NEWER: the visible repo candidate is newer than the installed row
//   OLDER: the installed row is newer than the visible repo candidate
// -----------------------------------------------------------------------------
enum class PackageRepoCandidateRelation {
  UNKNOWN,
  NONE,
  SAME,
  NEWER,
  OLDER,
};

// -----------------------------------------------------------------------------
// Backend-owned install reason for installed packages.
// This keeps package provenance visible to the UI without exposing libdnf5
// enums directly through the presentation model. Available-only rows keep
// UNKNOWN because the install reason is meaningful only for installed packages.
// -----------------------------------------------------------------------------
enum class PackageInstallReason {
  UNKNOWN,
  DEPENDENCY,
  USER,
  CLEAN,
  WEAK_DEPENDENCY,
  GROUP,
  EXTERNAL,
};

struct PackageRow {
  std::string nevra;
  std::string name;
  std::string epoch;
  std::string version;
  std::string release;
  std::string arch;
  std::string repo;
  std::string summary;
  PackageInstallReason install_reason = PackageInstallReason::UNKNOWN;
  PackageRepoCandidateRelation repo_candidate_relation = PackageRepoCandidateRelation::UNKNOWN;
  // Available package ID for the newest repo candidate matching this installed row.
  std::string repo_candidate_nevra;

  // -----------------------------------------------------------------------------
  // Return the package epoch field.
  // -----------------------------------------------------------------------------
  const std::string &get_epoch() const
  {
    return epoch;
  }
  // -----------------------------------------------------------------------------
  // Return the package version field.
  // -----------------------------------------------------------------------------
  const std::string &get_version() const
  {
    return version;
  }
  // -----------------------------------------------------------------------------
  // Return the package release field.
  // -----------------------------------------------------------------------------
  const std::string &get_release() const
  {
    return release;
  }
  // -----------------------------------------------------------------------------
  // Return the package identity key used for name and architecture lookups.
  // -----------------------------------------------------------------------------
  std::string name_arch_key() const
  {
    return name + "\n" + arch;
  }

  // -----------------------------------------------------------------------------
  // Return the user-visible version and release string.
  // -----------------------------------------------------------------------------
  std::string display_version() const
  {
    if (version.empty()) {
      return release;
    }
    if (release.empty()) {
      return version;
    }
    return version + "-" + release;
  }
};

// -----------------------------------------------------------------------------
// Backend-owned install state so the UI can reason about package actions
// without depending on libdnf5 headers or EVR comparison details.
// These values are presentation-oriented and may depend on repo provenance
// being known for the visible row.
// -----------------------------------------------------------------------------
enum class PackageInstallState {
  AVAILABLE,
  UPGRADEABLE,
  INSTALLED,
  LOCAL_ONLY,
  INSTALLED_NEWER_THAN_REPO,
};

// Resolved transaction preview used by the confirmation dialog before apply.
struct TransactionPreview {
  std::vector<std::string> install;
  std::vector<std::string> upgrade;
  std::vector<std::string> downgrade;
  std::vector<std::string> reinstall;
  std::vector<std::string> remove;
  long long disk_space_delta = 0;

  // -----------------------------------------------------------------------------
  // Return true when the preview contains no resolved package actions.
  // -----------------------------------------------------------------------------
  bool empty() const
  {
    return install.empty() && upgrade.empty() && downgrade.empty() && reinstall.empty() && remove.empty();
  }
};

using TransactionProgressCallback = std::function<void(const std::string &)>;

// -----------------------------------------------------------------------------
// Search flags used by backend search queries. The UI can update them from the
// search controls, and each backend worker copies a snapshot before scanning so
// one query remains internally consistent.
// -----------------------------------------------------------------------------
struct DnfBackendSearchOptions {
  bool search_in_description = false;
  bool exact_match = false;
};

// -----------------------------------------------------------------------------
// Publish the backend-owned search option snapshot.
// -----------------------------------------------------------------------------
void dnf_backend_set_search_options(const DnfBackendSearchOptions &options);
// -----------------------------------------------------------------------------
// Return the current backend-owned search option snapshot.
// -----------------------------------------------------------------------------
DnfBackendSearchOptions dnf_backend_get_search_options();

// -----------------------------------------------------------------------------
// Return true when the installed-package snapshot contains the exact NEVRA.
// -----------------------------------------------------------------------------
bool dnf_backend_installed_snapshot_contains(const std::string &nevra);
// -----------------------------------------------------------------------------
// Return the number of exact NEVRA entries in the installed snapshot.
// -----------------------------------------------------------------------------
size_t dnf_backend_installed_snapshot_size();

// -----------------------------------------------------------------------------
// Refresh the installed-package snapshot used by the UI for exact-installed
// checks and upgrade-state classification.
// -----------------------------------------------------------------------------
void dnf_backend_refresh_installed_nevras();

// -----------------------------------------------------------------------------
// Classify one visible package row for UI status badges and action gating.
// -----------------------------------------------------------------------------
PackageInstallState dnf_backend_get_package_install_state(const PackageRow &row);

// -----------------------------------------------------------------------------
// Return the default package-table sort priority for one package state.
// Lower values sort first and keep installed rows ahead of repo-only rows.
// -----------------------------------------------------------------------------
int dnf_backend_get_install_state_sort_rank(PackageInstallState state);

// -----------------------------------------------------------------------------
// Convert one backend-owned install reason to user-facing text.
// -----------------------------------------------------------------------------
std::string dnf_backend_install_reason_to_string(PackageInstallReason reason);

// -----------------------------------------------------------------------------
// Return true only when this exact NEVRA is installed on the current system.
// -----------------------------------------------------------------------------
bool dnf_backend_is_package_installed_exact(const PackageRow &row);

// -----------------------------------------------------------------------------
// Return the installed row with the same package name and architecture as one
// visible row. This lets the UI act on the installed package when the selected
// row is an available upgrade candidate.
// -----------------------------------------------------------------------------
bool dnf_backend_get_installed_package_row_by_name_arch(const PackageRow &row, PackageRow &installed_out);

// -----------------------------------------------------------------------------
// Return true when the exact installed NEVRA can be reinstalled from currently
// available package sources. Local-only packages therefore return false.
// -----------------------------------------------------------------------------
bool dnf_backend_can_reinstall_package(const PackageRow &row);

// -----------------------------------------------------------------------------
// Return true when this installed package owns the running GUI executable and
// therefore must not be removed or reinstalled from within the app itself.
// -----------------------------------------------------------------------------
bool dnf_backend_is_package_self_protected(const PackageRow &row);

// -----------------------------------------------------------------------------
// Return true when one installed remove or reinstall spec targets the running GUI
// package and must be rejected before the transaction is previewed or applied.
// -----------------------------------------------------------------------------
bool dnf_backend_is_self_protected_transaction_spec(const std::string &spec);

// -----------------------------------------------------------------------------
// Query all installed packages. This path remains local-first and should still
// work when repository metadata is unavailable; repo provenance is annotated
// only as an optional extra when possible.
// -----------------------------------------------------------------------------
std::vector<PackageRow> dnf_backend_get_installed_package_rows_interruptible(GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Query the merged browse view shown by "List Packages".
// -----------------------------------------------------------------------------
std::vector<PackageRow> dnf_backend_get_browse_package_rows_interruptible(GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Query available repo packages that are upgrades to installed packages.
// -----------------------------------------------------------------------------
std::vector<PackageRow> dnf_backend_get_upgradeable_package_rows_interruptible(GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Search the merged browse view using the current search flags.
// -----------------------------------------------------------------------------
std::vector<PackageRow> dnf_backend_search_package_rows_interruptible(const std::string &pattern,
                                                                      GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Return installed package rows that exactly match one NEVRA.
// -----------------------------------------------------------------------------
std::vector<PackageRow> dnf_backend_get_installed_package_rows_by_nevra(const std::string &pkg_nevra);
// -----------------------------------------------------------------------------
// Return available package rows that exactly match one NEVRA.
// -----------------------------------------------------------------------------
std::vector<PackageRow> dnf_backend_get_available_package_rows_by_nevra(const std::string &pkg_nevra);
// -----------------------------------------------------------------------------
// Return formatted package details for one NEVRA.
// -----------------------------------------------------------------------------
std::string dnf_backend_get_package_info(const std::string &pkg_nevra);
// -----------------------------------------------------------------------------
// Return the installed file list for one NEVRA.
// -----------------------------------------------------------------------------
std::string dnf_backend_get_installed_package_files(const std::string &pkg_nevra, size_t max_files_for_display = 1500);
// -----------------------------------------------------------------------------
// Return formatted dependency details for one NEVRA.
// -----------------------------------------------------------------------------
std::string dnf_backend_get_package_deps(const std::string &pkg_nevra);
// -----------------------------------------------------------------------------
// Return formatted changelog entries for one NEVRA.
// -----------------------------------------------------------------------------
std::string dnf_backend_get_package_changelog(const std::string &pkg_nevra);
// -----------------------------------------------------------------------------
// Resolve the pending transaction and summarize the final package changes for UI review.
// -----------------------------------------------------------------------------
bool dnf_backend_preview_transaction(const std::vector<std::string> &install_nevras,
                                     const std::vector<std::string> &remove_nevras,
                                     const std::vector<std::string> &reinstall_nevras,
                                     TransactionPreview &preview,
                                     std::string &error_out,
                                     const TransactionProgressCallback &progress_cb = {},
                                     bool upgrade_all = false);
// -----------------------------------------------------------------------------
// Resolve and apply the requested transaction and report progress.
// -----------------------------------------------------------------------------
bool dnf_backend_apply_transaction(const std::vector<std::string> &install_nevras,
                                   const std::vector<std::string> &remove_nevras,
                                   const std::vector<std::string> &reinstall_nevras,
                                   std::string &error_out,
                                   const TransactionProgressCallback &progress_cb = {},
                                   bool upgrade_all = false);

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Test-only hooks for cache-state setup. Production callers should refresh the
// installed snapshot from libdnf instead of mutating it directly.
// -----------------------------------------------------------------------------
void dnf_backend_testonly_clear_installed_snapshot();
// -----------------------------------------------------------------------------
// Replace the installed snapshot with test data.
// -----------------------------------------------------------------------------
void dnf_backend_testonly_replace_installed_snapshot(const std::set<std::string> &nevras);
// -----------------------------------------------------------------------------
// Replace the installed snapshot with full package rows for tests that need
// name and architecture lookups.
// -----------------------------------------------------------------------------
void dnf_backend_testonly_replace_installed_snapshot_rows(const std::vector<PackageRow> &rows);
// -----------------------------------------------------------------------------
// Test-only hook: force the optional repo annotation path to fail and return
// whether all rows kept UNKNOWN repo-candidate relation afterwards.
// -----------------------------------------------------------------------------
bool dnf_backend_testonly_annotation_fallback_leaves_rows_unknown(std::vector<PackageRow> &rows);
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
