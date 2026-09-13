// -----------------------------------------------------------------------------
// src/dnf5daemon_client/transaction_service_client.hpp
// GUI client helpers for the dnf5daemon transaction path
// Declares the small client API used by the GTK frontend to prepare, apply, and release transaction requests.
// -----------------------------------------------------------------------------
#pragma once

#include "upgrade/daemon_upgrade_target.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

typedef struct _GCancellable GCancellable;

struct TransactionRequest;
struct TransactionPreview;

struct TransactionKeyImportRequest {
  std::string key_id;
  std::vector<std::string> user_ids;
  std::string fingerprint;
  std::string key_url;
};

using TransactionKeyImportCallback = std::function<bool(const TransactionKeyImportRequest &)>;

struct TransactionApplyProgress {
  bool determinate = false;
  uint64_t processed = 0;
  uint64_t total = 0;
};

using TransactionApplyProgressCallback = std::function<void(const TransactionApplyProgress &)>;

// -----------------------------------------------------------------------------
// Prepare one transaction through dnf5daemon and return its resolved preview.
// -----------------------------------------------------------------------------
bool transaction_service_client_preview_request(const TransactionRequest &request,
                                                TransactionPreview &preview_out,
                                                std::string &transaction_path_out,
                                                std::string &error_out,
                                                const TransactionKeyImportCallback &key_import_callback = {},
                                                GCancellable *cancellable = nullptr);
// -----------------------------------------------------------------------------
// Prepare an upgrade-all transaction through dnf5daemon.
// -----------------------------------------------------------------------------
bool
transaction_service_client_preview_upgrade_all_request(TransactionPreview &preview_out,
                                                       std::string &transaction_path_out,
                                                       std::string &error_out,
                                                       const TransactionKeyImportCallback &key_import_callback = {},
                                                       GCancellable *cancellable = nullptr);

// -----------------------------------------------------------------------------
// List upgrade targets directly from dnf5daemon's package-list API.
// This is a read-only snapshot of daemon upgrade state, not a transaction preview.
// -----------------------------------------------------------------------------
bool transaction_service_client_list_upgrade_targets(std::vector<DaemonUpgradeTarget> &targets_out,
                                                     std::string &error_out,
                                                     GCancellable *cancellable = nullptr);

// -----------------------------------------------------------------------------
// Refresh dnf5daemon repository metadata for the manual Refresh Repositories action.
// -----------------------------------------------------------------------------
bool transaction_service_client_refresh_repositories(std::string &error_out, GCancellable *cancellable = nullptr);

// -----------------------------------------------------------------------------
// Apply one previously prepared transaction request and forward its progress.
// transaction_started_out is true when daemon progress reported that the RPM transaction began.
// Offline apply only prepares changes for reboot and must match the approved preview.
// -----------------------------------------------------------------------------
bool transaction_service_client_apply_started_request(const std::string &transaction_path,
                                                      const std::function<void(const std::string &)> &progress_callback,
                                                      const TransactionApplyProgressCallback &progress_update_callback,
                                                      const TransactionKeyImportCallback &key_import_callback,
                                                      std::string &error_out,
                                                      bool &transaction_started_out,
                                                      GCancellable *cancellable = nullptr,
                                                      bool offline = false);

// -----------------------------------------------------------------------------
// Read or discard DNF's stored offline transaction without loading repositories.
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

bool transaction_service_client_get_offline_status(OfflineTransactionStatus &status_out,
                                                   std::string &error_out,
                                                   GCancellable *cancellable = nullptr);
bool transaction_service_client_clear_offline_transaction(std::string &error_out, GCancellable *cancellable = nullptr);

// -----------------------------------------------------------------------------
// Release one finished transaction request that is no longer needed.
// -----------------------------------------------------------------------------
void transaction_service_client_release_request(const std::string &transaction_path);
// -----------------------------------------------------------------------------
// Queue request release on a worker thread so GTK cleanup does not wait on D-Bus.
// -----------------------------------------------------------------------------
void transaction_service_client_release_request_async(const std::string &transaction_path);

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Drop the cached D-Bus connection so integration tests can stop their private
// session bus cleanly between runs.
// -----------------------------------------------------------------------------
void transaction_service_client_reset_for_tests();

// -----------------------------------------------------------------------------
// Return true when the cached daemon connection can still see a session path.
// -----------------------------------------------------------------------------
bool transaction_service_client_session_exists_for_tests(const std::string &transaction_path);

// -----------------------------------------------------------------------------
// Feed one daemon transaction item through the preview parser for tests.
// -----------------------------------------------------------------------------
bool transaction_service_client_testonly_build_preview_from_item(const std::string &object_type,
                                                                 const std::string &action,
                                                                 const std::string &name,
                                                                 TransactionPreview &preview,
                                                                 std::string &error_out);
// -----------------------------------------------------------------------------
// Check the completed-preview daemon-server protection rule used by daemon previews.
// -----------------------------------------------------------------------------
bool transaction_service_client_testonly_verify_preview_keeps_required_daemon_server(const TransactionPreview &preview,
                                                                                     std::string &error_out);
// -----------------------------------------------------------------------------
// Feed one daemon upgrade target object through the package-list parser for tests.
// -----------------------------------------------------------------------------
bool transaction_service_client_testonly_build_upgrade_target_from_fields(const std::string &name,
                                                                          const std::string &epoch,
                                                                          const std::string &version,
                                                                          const std::string &release,
                                                                          const std::string &arch,
                                                                          const std::string &repo_id,
                                                                          const std::string &nevra,
                                                                          const std::string &full_nevra,
                                                                          DaemonUpgradeTarget &target_out,
                                                                          std::string &error_out);
// -----------------------------------------------------------------------------
// Check the resolved-preview self-protection rule used by daemon previews.
// -----------------------------------------------------------------------------
bool transaction_service_client_testonly_verify_preview_keeps_running_app_package(const TransactionPreview &preview,
                                                                                  std::string &error_out);
// -----------------------------------------------------------------------------
// Return the key import answer after applying the cancellation rule used before daemon confirmation.
// -----------------------------------------------------------------------------
bool transaction_service_client_testonly_key_import_answer_after_callback(const TransactionKeyImportCallback &callback,
                                                                          GCancellable *cancellable,
                                                                          bool &confirmed_out);
// -----------------------------------------------------------------------------
// Return the cancellable used for the final key confirmation call.
// -----------------------------------------------------------------------------
GCancellable *transaction_service_client_testonly_key_import_confirmation_cancellable(bool confirmed,
                                                                                      GCancellable *cancellable);
// -----------------------------------------------------------------------------
// Return the user-facing preparation error for one daemon D-Bus error.
// -----------------------------------------------------------------------------
std::string transaction_service_client_testonly_prepare_error_message(const std::string &remote_error,
                                                                      const std::string &daemon_message);
// -----------------------------------------------------------------------------
// Return true when one selected request should load available repositories.
// -----------------------------------------------------------------------------
bool transaction_service_client_testonly_request_loads_available_repos(const TransactionRequest &request);
// -----------------------------------------------------------------------------
// Return the user-facing apply error for one daemon D-Bus error.
// -----------------------------------------------------------------------------
std::string transaction_service_client_testonly_apply_error_message(const std::string &remote_error,
                                                                    const std::string &daemon_message);
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
