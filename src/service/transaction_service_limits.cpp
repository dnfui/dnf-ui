// -----------------------------------------------------------------------------
// transaction_service_limits.cpp
// Enforces limits for live transaction request objects.
// Keeps one client from leaving too many request objects owned by the service.
// -----------------------------------------------------------------------------
#include "transaction_service_internal.hpp"

#include "i18n.hpp"

// -----------------------------------------------------------------------------
// Return true when request object limits are already reached.
// -----------------------------------------------------------------------------
bool
service_request_limit_reached(TransactionService *service, const std::string &owner_name, std::string &error_out)
{
  if (!service) {
    error_out = _("Transaction service is not ready.");
    return true;
  }

  if (service->transactions.size() >= kMaxLiveTransactionSessions) {
    error_out = _("The transaction service has too many active requests.");
    return true;
  }

  size_t owner_count = 0;
  for (const auto &[path, session] : service->transactions) {
    (void)path;
    if (session && session->owner_name == owner_name) {
      owner_count++;
    }
  }

  if (owner_count >= kMaxLiveTransactionSessionsPerClient) {
    error_out = _("This client has too many active transaction requests.");
    return true;
  }

  return false;
}
