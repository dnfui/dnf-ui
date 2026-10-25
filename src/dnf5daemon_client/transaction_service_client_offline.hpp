// -----------------------------------------------------------------------------
// transaction_service_client_offline.hpp
// Internal offline status and preflight helpers for the daemon client
// -----------------------------------------------------------------------------
#pragma once

#include <gio/gio.h>

#include <string>

namespace dnfui_offline {

// -----------------------------------------------------------------------------
// A scheduled transaction has not yet changed installed packages.
// -----------------------------------------------------------------------------
struct OfflineTransactionStatus {
  bool scheduled = false;
  bool boot_trigger_present = false;
  std::string state;

  bool has_transaction() const
  {
    return scheduled || boot_trigger_present || !state.empty();
  }
};

bool get_offline_status(GDBusConnection *connection,
                        const std::string &session_path,
                        OfflineTransactionStatus &status_out,
                        std::string &error_out,
                        GCancellable *cancellable);

bool verify_no_offline_transaction(GDBusConnection *connection,
                                   const std::string &session_path,
                                   std::string &error_out,
                                   GCancellable *cancellable);

} // namespace dnfui_offline

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
