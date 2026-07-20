// -----------------------------------------------------------------------------
// transaction_service_client_internal.hpp
// Private helpers for the GUI-side transaction client.
// This is not a public API.
// It keeps raw D-Bus calls and wait handling out of the high-level preview and apply flow.
// -----------------------------------------------------------------------------
#pragma once

#include <gio/gio.h>

#include "transaction_service_client.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

struct TransactionPreview;
struct TransactionRequest;

struct TransactionServiceProgressForwarder {
  const std::function<void(const std::string &)> *progress_callback = nullptr;
  const TransactionKeyImportCallback *key_import_callback = nullptr;
  std::string transaction_path;
  bool downloads_started = false;
  bool transaction_started = false;
  bool verify_started = false;
  bool prepare_started = false;
  std::string key_confirm_error;
  std::map<std::string, int> download_bucket_by_id;
  std::map<std::string, std::string> download_description_by_id;
};

// -----------------------------------------------------------------------------
// Low-level D-Bus calls.
// -----------------------------------------------------------------------------
GDBusConnection *transaction_service_client_connect(std::string &error_out);

bool transaction_service_client_start_transaction_request(GDBusConnection *connection,
                                                          const TransactionRequest &request,
                                                          std::string &transaction_path_out,
                                                          std::string &error_out);

bool transaction_service_client_start_upgrade_all_transaction_request(GDBusConnection *connection,
                                                                      std::string &transaction_path_out,
                                                                      std::string &error_out);

bool transaction_service_client_get_transaction_preview(GDBusConnection *connection,
                                                        const std::string &transaction_path,
                                                        TransactionServiceProgressForwarder *progress_forwarder,
                                                        GCancellable *cancellable,
                                                        TransactionPreview &preview_out,
                                                        std::string &error_out);

bool transaction_service_client_start_apply_request(GDBusConnection *connection,
                                                    const std::string &transaction_path,
                                                    TransactionServiceProgressForwarder *progress_forwarder,
                                                    GCancellable *cancellable,
                                                    std::string &error_out);

bool transaction_service_client_release_transaction_request(GDBusConnection *connection,
                                                            const std::string &transaction_path,
                                                            std::string &error_out);

bool transaction_service_client_confirm_key(GDBusConnection *connection,
                                            const std::string &transaction_path,
                                            const std::string &key_id,
                                            bool confirmed,
                                            std::string &error_out);

bool transaction_service_client_list_daemon_upgrade_targets(GDBusConnection *connection,
                                                            GCancellable *cancellable,
                                                            std::vector<TransactionServiceUpgradeTarget> &targets_out,
                                                            std::string &error_out);

guint transaction_service_client_subscribe_progress(GDBusConnection *connection,
                                                    const std::string &transaction_path,
                                                    TransactionServiceProgressForwarder *progress_forwarder);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
