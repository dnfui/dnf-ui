// -----------------------------------------------------------------------------
// transaction_service_client_internal.hpp
// Private helpers for the GUI-side transaction service client.
// This is not a public API.
// It keeps raw D-Bus calls and wait handling out of the high-level preview and apply flow.
// -----------------------------------------------------------------------------
#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>

struct TransactionPreview;
struct TransactionRequest;

// -----------------------------------------------------------------------------
// Transaction service result state returned by GetResult.
// -----------------------------------------------------------------------------
struct TransactionServiceResult {
  std::string stage;
  bool finished = false;
  bool success = false;
  std::string details;
};

struct TransactionServiceProgressForwarder {
  const std::function<void(const std::string &)> *progress_callback = nullptr;
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

bool transaction_service_client_get_transaction_result(GDBusConnection *connection,
                                                       const std::string &transaction_path,
                                                       TransactionServiceResult &result_out,
                                                       std::string &error_out);

bool transaction_service_client_get_transaction_preview(GDBusConnection *connection,
                                                        const std::string &transaction_path,
                                                        TransactionPreview &preview_out,
                                                        std::string &error_out);

bool transaction_service_client_start_apply_request(GDBusConnection *connection,
                                                    const std::string &transaction_path,
                                                    std::string &error_out);

bool transaction_service_client_release_transaction_request(GDBusConnection *connection,
                                                            const std::string &transaction_path,
                                                            std::string &error_out);

// -----------------------------------------------------------------------------
// Wait and progress signal handling.
// -----------------------------------------------------------------------------
bool transaction_service_client_wait_for_transaction_stage(GDBusConnection *connection,
                                                           const std::string &transaction_path,
                                                           const char *running_stage,
                                                           GMainContext *context,
                                                           TransactionServiceResult &result_out,
                                                           std::string &error_out);

bool transaction_service_client_wait_for_started_transaction_preview(GDBusConnection *connection,
                                                                     const std::string &transaction_path,
                                                                     TransactionPreview &preview_out,
                                                                     std::string &error_out);

guint transaction_service_client_subscribe_progress(GDBusConnection *connection,
                                                    const std::string &transaction_path,
                                                    TransactionServiceProgressForwarder *progress_forwarder);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
