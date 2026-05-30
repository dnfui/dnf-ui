// -----------------------------------------------------------------------------
// transaction_service_internal.hpp
// Private transaction service declarations shared by the service implementation.
// This is not a public API.
// It gives the split service source files the same request and runtime state.
// -----------------------------------------------------------------------------
#pragma once

#include "service/transaction_service_dbus.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "transaction_request.hpp"

#include <gio/gio.h>

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

constexpr const char *kServiceName = kTransactionServiceName;
constexpr const char *kManagerObjectPath = kTransactionServiceManagerPath;
constexpr const char *kManagerInterface = kTransactionServiceManagerInterface;
constexpr const char *kTransactionInterface = kTransactionServiceRequestInterface;
constexpr const char *kPreviewActionId = "com.fedora.dnfui.preview-transactions";
constexpr const char *kApplyActionId = "com.fedora.dnfui.apply-transactions";
constexpr size_t kMaxLiveTransactionSessions = 32;
constexpr size_t kMaxLiveTransactionSessionsPerClient = 8;
constexpr unsigned kMaxPreviewWorkers = 2;

// -----------------------------------------------------------------------------
// Transaction service runtime state
// -----------------------------------------------------------------------------
struct TransactionService;

enum class TransactionStage {
  PREVIEW_RUNNING,
  PREVIEW_READY,
  PREVIEW_FAILED,
  APPLY_RUNNING,
  APPLY_SUCCEEDED,
  APPLY_FAILED,
  CANCELLED,
};

struct TransactionSession {
  TransactionService *service = nullptr;
  guint registration_id = 0;
  std::string object_path;
  // Protects preview, stage, success, details, and pending_apply_invocation.
  std::mutex state_mutex;
  TransactionRequest request;
  TransactionPreview preview;
  std::atomic<bool> finished { false };
  std::atomic<bool> cancelled { false };
  std::atomic<bool> release_requested { false };
  TransactionStage stage = TransactionStage::PREVIEW_RUNNING;
  bool success = false;
  std::string details;
  GDBusMethodInvocation *pending_apply_invocation = nullptr;
  std::string owner_name;
  guint owner_watch_id = 0;
};

struct TransactionService {
  GMainLoop *loop = nullptr;
  GMainContext *main_context = nullptr;
  GDBusConnection *connection = nullptr;
  GDBusNodeInfo *manager_node_info = nullptr;
  GDBusNodeInfo *transaction_node_info = nullptr;
  GBusType bus_type = G_BUS_TYPE_SESSION;
  guint owner_id = 0;
  guint manager_registration_id = 0;
  guint next_transaction_id = 1;
  std::atomic<bool> apply_running { false };
  std::atomic<bool> shutting_down { false };
  bool keep_alive_until_exit = false;
  std::map<std::string, std::unique_ptr<TransactionSession>> transactions;
  std::atomic<unsigned> preview_workers { 0 };
};

// -----------------------------------------------------------------------------
// Main loop dispatch payloads
// -----------------------------------------------------------------------------
struct QueuedProgressMessage {
  TransactionSession *session = nullptr;
  std::string line;
};

struct QueuedFinishedResult {
  TransactionSession *session = nullptr;
  TransactionStage stage = TransactionStage::PREVIEW_FAILED;
  bool success = false;
  std::string details;
};

struct QueuedSessionRelease {
  TransactionService *service = nullptr;
  std::string object_path;
};

// -----------------------------------------------------------------------------
// Transaction request limits
// -----------------------------------------------------------------------------
bool service_request_limit_reached(TransactionService *service, const std::string &owner_name, std::string &error_out);

// -----------------------------------------------------------------------------
// Transaction request signals
// -----------------------------------------------------------------------------
void emit_transaction_progress(TransactionSession *session, const std::string &line);
void emit_transaction_finished(TransactionSession *session,
                               TransactionStage stage,
                               bool success,
                               const std::string &details);
void queue_transaction_progress(TransactionSession *session, const std::string &line);
void queue_transaction_finished(TransactionSession *session,
                                TransactionStage stage,
                                bool success,
                                const std::string &details);
void queue_transaction_release(TransactionSession *session);

// -----------------------------------------------------------------------------
// Transaction reply formatting
// -----------------------------------------------------------------------------
void append_transaction_preview_array(GVariantBuilder &builder, const std::vector<std::string> &items);
const char *transaction_stage_name(TransactionStage stage);
void set_transaction_running(TransactionSession *session, TransactionStage stage);

// -----------------------------------------------------------------------------
// Transaction authorization
// -----------------------------------------------------------------------------
bool authorize_preview_start(TransactionService *service, const char *sender, std::string &error_out);
bool
start_authorize_apply_request(TransactionSession *session, GDBusMethodInvocation *invocation, std::string &error_out);
void complete_apply_request(TransactionSession *session, GDBusMethodInvocation *invocation);

// -----------------------------------------------------------------------------
// Transaction worker entrypoints
// -----------------------------------------------------------------------------
gboolean start_transaction_preview(gpointer user_data);
gboolean start_transaction_apply(gpointer user_data);
bool validate_transaction_request_for_service(const TransactionRequest &request, std::string &error_out);

// -----------------------------------------------------------------------------
// Transaction request objects
// -----------------------------------------------------------------------------
bool get_invocation_sender(GDBusMethodInvocation *invocation, std::string &sender_out, std::string &error_out);
TransactionSession *create_transaction_session(TransactionService *service,
                                               const TransactionRequest &request,
                                               const std::string &owner_name,
                                               std::string &error_out);

extern const GDBusInterfaceVTable kManagerVTable;
