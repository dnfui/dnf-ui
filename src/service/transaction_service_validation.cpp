// -----------------------------------------------------------------------------
// transaction_service_validation.cpp
// Service-side checks for transaction requests
//
// Keeps request checks that need current installed-package state out of the worker thread implementation.
// -----------------------------------------------------------------------------
#include "transaction_service_internal.hpp"

#include "base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"

#include <exception>

namespace {

struct BackendBaseDropGuard {
  ~BackendBaseDropGuard()
  {
    BaseManager::instance().drop_cached_base();
  }
};

} // namespace

// -----------------------------------------------------------------------------
// Reject requests that passed shared validation but are unsafe for the service to run.
// -----------------------------------------------------------------------------
bool
validate_transaction_request_for_service(const TransactionRequest &request, std::string &error_out)
{
  error_out.clear();

  if (request.install.empty() && request.remove.empty() && request.reinstall.empty()) {
    return true;
  }

  BackendBaseDropGuard base_drop_guard;

  try {
    dnf_backend_refresh_installed_nevras();
  } catch (const std::exception &e) {
    error_out = std::string(_("Unable to validate protected installed packages: ")) + e.what();
    return false;
  }

  for (const auto &spec : request.install) {
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot upgrade the package that owns the running application while it is running.");
      return false;
    }
  }

  for (const auto &spec : request.remove) {
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot remove the package that owns the running application.");
      return false;
    }
  }

  for (const auto &spec : request.reinstall) {
    if (dnf_backend_is_self_protected_transaction_spec(spec)) {
      error_out = _("DNF UI cannot reinstall the package that owns the running application while it is running.");
      return false;
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
