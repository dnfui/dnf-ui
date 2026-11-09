// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_common.cpp
// Shared libdnf5 backend model conversion helpers
//
// This file contains small conversions that are used across the backend implementation.
// Keeping them separate avoids making query, details, and transaction files depend on each other's private helpers.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_internal.hpp"

#include "i18n.hpp"

#include <string>

#include <gio/gio.h>

#include <libdnf5/rpm/nevra.hpp>
#include <libdnf5/transaction/transaction_item_reason.hpp>

namespace dnf_backend_internal {

// -----------------------------------------------------------------------------
// Translate libdnf5 install reasons into the backend-owned row model.
// -----------------------------------------------------------------------------
static PackageInstallReason
package_install_reason_from_libdnf(libdnf5::transaction::TransactionItemReason reason)
{
  switch (reason) {
  case libdnf5::transaction::TransactionItemReason::DEPENDENCY:
    return PackageInstallReason::DEPENDENCY;
  case libdnf5::transaction::TransactionItemReason::USER:
    return PackageInstallReason::USER;
  case libdnf5::transaction::TransactionItemReason::CLEAN:
    return PackageInstallReason::CLEAN;
  case libdnf5::transaction::TransactionItemReason::WEAK_DEPENDENCY:
    return PackageInstallReason::WEAK_DEPENDENCY;
  case libdnf5::transaction::TransactionItemReason::GROUP:
    return PackageInstallReason::GROUP;
  case libdnf5::transaction::TransactionItemReason::EXTERNAL_USER:
    return PackageInstallReason::EXTERNAL;
  case libdnf5::transaction::TransactionItemReason::NONE:
  default:
    return PackageInstallReason::UNKNOWN;
  }
}

// -----------------------------------------------------------------------------
// Convert a libdnf5 package object into the backend-owned presentation row used
// by controllers and GTK views. This is the only place where libdnf install
// reasons are translated into PackageInstallReason values.
// -----------------------------------------------------------------------------
PackageRow
make_package_row(const libdnf5::rpm::Package &pkg, PackageRepoCandidateRelation repo_candidate_relation)
{
  PackageRow row;
  row.nevra = pkg.get_nevra();
  row.name = pkg.get_name();
  row.epoch = pkg.get_epoch();
  row.version = pkg.get_version();
  row.release = pkg.get_release();
  row.arch = pkg.get_arch();
  row.repo = pkg.get_repo_id();
  if (pkg.is_installed()) {
    row.installed_from_repo = pkg.get_from_repo_id();
  }
  row.summary = pkg.get_summary();
  row.install_size = pkg.get_install_size();
  if (pkg.is_installed()) {
    row.install_reason = package_install_reason_from_libdnf(pkg.get_reason());
  }
  row.repo_candidate_relation = repo_candidate_relation;

  if (row.summary.empty()) {
    row.summary = _("(no summary)");
  }

  return row;
}

// -----------------------------------------------------------------------------
// Return true when a package query task has been cancelled by the UI worker that owns the provided cancellable.
// -----------------------------------------------------------------------------
bool
package_query_cancelled(GCancellable *cancellable)
{
  return cancellable && g_cancellable_is_cancelled(cancellable);
}

} // namespace dnf_backend_internal

namespace {

struct TextEvr {
  const std::string &epoch;
  const std::string &version;
  const std::string &release;

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
};

} // namespace

// -----------------------------------------------------------------------------
// Convert one backend-owned install reason to stable UI text.
// -----------------------------------------------------------------------------
std::string
dnf_backend_install_reason_to_string(PackageInstallReason reason)
{
  switch (reason) {
  case PackageInstallReason::DEPENDENCY:
    return _("Dependency");
  case PackageInstallReason::USER:
    return _("User");
  case PackageInstallReason::CLEAN:
    return _("Clean");
  case PackageInstallReason::WEAK_DEPENDENCY:
    return _("Weak dependency");
  case PackageInstallReason::GROUP:
    return _("Group");
  case PackageInstallReason::EXTERNAL:
    return _("External");
  case PackageInstallReason::UNKNOWN:
  default:
    return _("Unknown");
  }
}

// -----------------------------------------------------------------------------
// Compare two RPM epoch and version groups using the same rules as DNF.
// -----------------------------------------------------------------------------
int
dnf_backend_compare_epoch_version_text(const std::string &left_epoch,
                                       const std::string &left_version,
                                       const std::string &right_epoch,
                                       const std::string &right_version)
{
  static const std::string empty_release;
  TextEvr left { left_epoch, left_version, empty_release };
  TextEvr right { right_epoch, right_version, empty_release };
  return libdnf5::rpm::evrcmp(left, right);
}

// -----------------------------------------------------------------------------
// Compare two RPM version strings using the same segment rules as DNF.
// -----------------------------------------------------------------------------
int
dnf_backend_compare_rpm_version_text(const std::string &left, const std::string &right)
{
  return libdnf5::rpm::rpmvercmp(left.c_str(), right.c_str());
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
