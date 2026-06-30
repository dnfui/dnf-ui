// -----------------------------------------------------------------------------
// src/dnf5daemon_client/transaction_service_client.hpp
// GUI client helpers for the dnf5daemon transaction path
// Declares the small client API used by the GTK frontend to prepare, apply, and
// release transaction requests.
// -----------------------------------------------------------------------------
#pragma once

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
// Ask dnf5daemon for package keys from the resolved Upgrade All preview.
// -----------------------------------------------------------------------------
bool transaction_service_client_list_upgrade_keys(std::vector<std::string> &keys_out,
                                                  std::string &error_out,
                                                  GCancellable *cancellable = nullptr);

// -----------------------------------------------------------------------------
// Refresh dnf5daemon repository metadata for the manual Refresh Repositories action.
// -----------------------------------------------------------------------------
bool transaction_service_client_refresh_repositories(std::string &error_out, GCancellable *cancellable = nullptr);

// -----------------------------------------------------------------------------
// Apply one previously prepared transaction request and forward its progress.
// -----------------------------------------------------------------------------
bool transaction_service_client_apply_started_request(const std::string &transaction_path,
                                                      const std::function<void(const std::string &)> &progress_callback,
                                                      const TransactionKeyImportCallback &key_import_callback,
                                                      std::string &error_out,
                                                      GCancellable *cancellable = nullptr);

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
// Check the resolved-preview self-protection rule used by daemon previews.
// -----------------------------------------------------------------------------
bool transaction_service_client_testonly_verify_preview_keeps_running_app_package(const TransactionPreview &preview,
                                                                                  std::string &error_out);
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
