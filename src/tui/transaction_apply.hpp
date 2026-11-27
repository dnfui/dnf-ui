// -----------------------------------------------------------------------------
// src/tui/transaction_apply.hpp
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

typedef struct _GCancellable GCancellable;

namespace tui {

// Owns exactly one resolved daemon session until apply or dismissal.
class PreparedTransaction {
  public:
  PreparedTransaction() = default;
  ~PreparedTransaction();

  PreparedTransaction(const PreparedTransaction &) = delete;
  PreparedTransaction &operator=(const PreparedTransaction &) = delete;

  std::string &path_for_resolution();
  void set_offline_mode(bool value);
  const std::string &path() const;
  bool requires_offline() const;

  private:
  std::string session_path;
  bool offline = false;
};

class TransactionProgress {
  public:
  void set_message(const std::string &message);
  void set_count(bool determinate, uint64_t processed, uint64_t total);
  std::string text() const;

  private:
  mutable std::mutex mutex;
  std::string message;
  std::string count;
};

enum class OfflineState {
  UNKNOWN,
  NOT_PREPARED,
  PREPARED,
};

// Read status on a private, cancellable connection without opening a transaction session on the shared client.
OfflineState query_offline_state(GCancellable *cancel);
const char *offline_recovery_hint();

struct ApplyResult {
  bool success = false;
  bool offline = false;
  std::string message;
  OfflineState offline_state = OfflineState::UNKNOWN;
};

// Consume the approved session without resolving another transaction.
ApplyResult apply_transaction(std::unique_ptr<PreparedTransaction> prepared, TransactionProgress &progress);

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
