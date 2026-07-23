// src/dnf_backend/dnf_backend.hpp
// Public libdnf5 backend facade
//
// This header is the app-facing contract for the libdnf5 integration.
// It keeps libdnf5 types out of the GTK controller layer by exposing value models.
// The implementation owns Base access, rpmdb and repo queries, EVR comparison, cache updates, and package details.
//
// Callers should depend only on the types and functions declared here.
// Helpers under the internal backend header are private implementation details for backend files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
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
//   UNKNOWN: repo relation was not checked or could not be resolved
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
// This keeps package origin visible to the UI without exposing libdnf5
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
  std::string installed_from_repo;
  std::string summary;
  PackageInstallReason install_reason = PackageInstallReason::UNKNOWN;
  PackageRepoCandidateRelation repo_candidate_relation = PackageRepoCandidateRelation::UNKNOWN;
  // These fields are set only when an installed row has a visible repository candidate.
  std::string repo_candidate_nevra;
  std::string repo_candidate_version;
  std::string repo_candidate_release;
  std::string repo_candidate_repo;

  // libdnf EVR comparison adapter.
  const std::string &get_epoch() const
  {
    return epoch;
  }
  const std::string &get_version() const
  {
    return version;
  }
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
// These values are presentation-oriented and may depend on repo relation
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
// The model must fully describe every resolved transaction action.
// Callers must never receive a partial preview when the backend cannot represent the whole
// resolved transaction.
struct TransactionPreview {
  // Human-readable warnings returned with a successful daemon resolve.
  std::string resolve_warnings;
  std::vector<std::string> install;
  std::vector<std::string> upgrade;
  std::vector<std::string> downgrade;
  std::vector<std::string> reinstall;
  std::vector<std::string> remove;
  std::vector<std::string> replaced;
  long long disk_space_delta = 0;

  // -----------------------------------------------------------------------------
  // Return true when the preview contains no resolved package actions.
  // -----------------------------------------------------------------------------
  bool empty() const
  {
    return install.empty() && upgrade.empty() && downgrade.empty() && reinstall.empty() && remove.empty() &&
        replaced.empty();
  }
};

// -----------------------------------------------------------------------------
// Read-only transaction history model.
// The UI uses this to inspect past package changes without depending on
// libdnf5 transaction classes.
// -----------------------------------------------------------------------------
enum class TransactionHistoryAction {
  INSTALL,
  UPGRADE,
  DOWNGRADE,
  REINSTALL,
  REMOVE,
  REPLACED,
  REASON_CHANGE,
  OTHER,
};

struct TransactionHistoryPackageRow {
  int64_t transaction_id = 0;
  int64_t started_at = 0;
  bool succeeded = false;
  TransactionHistoryAction action = TransactionHistoryAction::OTHER;
  std::string package_id;
  std::string name;
  std::string repo;
  std::string description;
};

struct TransactionHistoryCursor {
  size_t row_offset = 0;

  // -----------------------------------------------------------------------------
  // Return the cursor for one one-based page number.
  // -----------------------------------------------------------------------------
  static TransactionHistoryCursor for_page(size_t page, size_t rows_per_page)
  {
    TransactionHistoryCursor cursor;
    if (page > 1 && rows_per_page > 0) {
      cursor.row_offset = (page - 1) * rows_per_page;
    }
    return cursor;
  }

  // -----------------------------------------------------------------------------
  // Return the one-based page number for this cursor.
  // -----------------------------------------------------------------------------
  size_t page(size_t rows_per_page) const
  {
    if (rows_per_page == 0) {
      return 1;
    }
    return (row_offset / rows_per_page) + 1;
  }
};

enum class TransactionHistoryResultFilter {
  ALL,
  OK,
  FAILED,
};

struct TransactionHistoryFilter {
  std::string package_text;
  std::string detail_text;
  int64_t from = 0;
  int64_t to = std::numeric_limits<int64_t>::max();
  bool action_filter_enabled = false;
  std::set<TransactionHistoryAction> actions;
  TransactionHistoryResultFilter result = TransactionHistoryResultFilter::ALL;
};

struct TransactionHistoryPage {
  std::vector<TransactionHistoryPackageRow> rows;
  TransactionHistoryCursor next_cursor;
  bool has_more = false;
};

// -----------------------------------------------------------------------------
// Convert one transaction history action to user-facing text.
// -----------------------------------------------------------------------------
std::string dnf_backend_transaction_history_action_to_string(TransactionHistoryAction action);

// -----------------------------------------------------------------------------
// Return one page of package changes from the libdnf5 transaction history database.
// The cursor stores the first matching row offset for the requested page.
// The backend stops after the requested page and one extra matching row.
// -----------------------------------------------------------------------------
TransactionHistoryPage dnf_backend_list_transaction_history_page(TransactionHistoryCursor cursor,
                                                                 const TransactionHistoryFilter &filter,
                                                                 size_t max_package_rows,
                                                                 GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Search flags used by backend search queries.
// Each search request carries its own options so worker tasks do not depend on
// mutable global checkbox state.
// -----------------------------------------------------------------------------
struct DnfBackendSearchOptions {
  bool search_in_description = false;
  bool exact_match = false;
};

// -----------------------------------------------------------------------------
// Refresh the installed-package snapshot used by the UI for exact-installed
// checks and upgrade-state classification. Returns true when installed NEVRAs
// or self-protected package names changed.
// -----------------------------------------------------------------------------
bool dnf_backend_refresh_installed_nevras();

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
// Return the installed row with the same package name and architecture as one visible row.
// This lets the UI act on the installed package when the selected
// row is an available upgrade candidate.
// -----------------------------------------------------------------------------
bool dnf_backend_get_installed_package_row_by_name_arch(const PackageRow &row, PackageRow &installed_out);

// -----------------------------------------------------------------------------
// Return true when this installed package owns the running GUI executable.
// That package must not be modified from within the app itself.
// -----------------------------------------------------------------------------
bool dnf_backend_is_package_self_protected(const PackageRow &row);

// -----------------------------------------------------------------------------
// Return true when resolved daemon package labels name the running GUI package.
// Uses the current installed snapshot. Callers refresh that snapshot when needed.
// -----------------------------------------------------------------------------
bool dnf_backend_any_self_protected_package_label(const std::vector<std::string> &labels);

// -----------------------------------------------------------------------------
// Return true when one transaction spec targets the running GUI package
// and must be rejected before the transaction is previewed or applied.
// -----------------------------------------------------------------------------
bool dnf_backend_is_self_protected_transaction_spec(const std::string &spec);

// -----------------------------------------------------------------------------
// Query all installed packages. This path remains local-first and should still work when repository metadata is
// unavailable. Repo relation is added only as an optional extra when possible.
// -----------------------------------------------------------------------------
std::vector<PackageRow> dnf_backend_get_installed_package_rows_interruptible(GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Query the merged browse view shown by "List Packages".
// -----------------------------------------------------------------------------
std::vector<PackageRow> dnf_backend_get_browse_package_rows_interruptible(GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Return available package metadata for exact daemon-selected NEVRAs.
// This does not decide which packages are upgrades.
// -----------------------------------------------------------------------------
std::vector<PackageRow>
dnf_backend_get_available_package_metadata_by_nevras_interruptible(const std::vector<std::string> &nevras,
                                                                   GCancellable *cancellable);

// -----------------------------------------------------------------------------
// Search the merged browse view using one request-local option snapshot.
// -----------------------------------------------------------------------------
std::vector<PackageRow> dnf_backend_search_package_rows_interruptible(const std::string &pattern,
                                                                      const DnfBackendSearchOptions &search_options,
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
std::string dnf_backend_get_package_info(const std::string &pkg_nevra, const PackageRow *upgrade_row_override);
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

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Return true when the installed-package snapshot contains the exact NEVRA.
// -----------------------------------------------------------------------------
bool dnf_backend_installed_snapshot_contains_for_tests(const std::string &nevra);
// -----------------------------------------------------------------------------
// Return the number of exact NEVRA entries in the installed snapshot.
// -----------------------------------------------------------------------------
size_t dnf_backend_installed_snapshot_size_for_tests();
// -----------------------------------------------------------------------------
// Test-only hooks for cache-state setup.
// Production callers should refresh the installed snapshot from libdnf instead of mutating it directly.
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
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
