// -----------------------------------------------------------------------------
// dnf_transaction_callbacks.cpp
// libdnf callback adapters used while applying a transaction.
//
// libdnf reports download and rpm progress through callback objects.
// This file turns the useful callback events into the short text lines shown by the transaction progress window.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_transaction_internal.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <libdnf5/rpm/nevra.hpp>

namespace dnf_backend_transaction_internal {

namespace {

  // -----------------------------------------------------------------------------
  // Convert one transaction action to the verb used when rpm starts package work.
  // -----------------------------------------------------------------------------
  static std::string transaction_action_running_label(libdnf5::base::TransactionPackage::Action action)
  {
    using Action = libdnf5::base::TransactionPackage::Action;

    switch (action) {
    case Action::INSTALL:
      return "Installing";
    case Action::UPGRADE:
      return "Upgrading";
    case Action::DOWNGRADE:
      return "Downgrading";
    case Action::REINSTALL:
      return "Reinstalling";
    case Action::REMOVE:
      return "Removing";
    case Action::REPLACED:
      return "Replacing";
    case Action::REASON_CHANGE:
      return "Changing install reason for";
    default:
      return "Processing";
    }
  }

  // -----------------------------------------------------------------------------
  // Format an rpm callback NEVRA.
  // Script callbacks may refer to packages that are not direct transaction items.
  // -----------------------------------------------------------------------------
  static std::string rpm_nevra_label(const libdnf5::rpm::Nevra &nevra)
  {
    return libdnf5::rpm::to_nevra_string(nevra);
  }

  // -----------------------------------------------------------------------------
  // Format the package action line shown while rpm is applying the transaction.
  // libdnf item indexes start at zero.
  // -----------------------------------------------------------------------------
  static std::string
  format_transaction_item_progress(const libdnf5::base::TransactionPackage &item, uint64_t amount, uint64_t total)
  {
    std::string line = transaction_action_running_label(item.get_action());
    if (total == 0) {
      return line + ": " + transaction_package_label(item);
    }

    uint64_t current = std::min<uint64_t>(amount + 1, total);
    return line + " " + std::to_string(current) + "/" + std::to_string(total) + ": " + transaction_package_label(item);
  }

  // libdnf calls this object during transaction downloads.
  // Each callback creates a plain text progress line and sends it through progress_cb.
  class StreamingDownloadCallbacks final : public libdnf5::repo::DownloadCallbacks {
public:
    explicit StreamingDownloadCallbacks(TransactionProgressCallback progress_cb)
        : progress_cb(std::move(progress_cb))
    {
    }

    // One package download has started. libdnf keeps the returned DownloadState
    // pointer and passes it back to progress and end for the same package.
    void *add_new_download(void *, const char *description, double) override
    {
      auto *state = new DownloadState;
      state->description = description ? description : "package";
      emit_progress_line(progress_cb, "Downloading: " + state->description);
      return state;
    }

    // libdnf may call this often.
    // Report only ten percent steps so the progress window gets useful updates without being flooded.
    int progress(void *user_cb_data, double total_to_download, double downloaded) override
    {
      auto *state = static_cast<DownloadState *>(user_cb_data);
      if (!state || total_to_download <= 0.0) {
        return OK;
      }

      int percent = static_cast<int>((downloaded * 100.0) / total_to_download);
      percent = std::clamp(percent, 0, 100);
      int bucket = percent / 10;

      if (bucket > state->last_reported_bucket) {
        state->last_reported_bucket = bucket;
        emit_progress_line(progress_cb,
                           "Download progress: " + state->description + " (" + std::to_string(percent) + "%)");
      }

      return OK;
    }

    // One package download has finished.
    // The unique_ptr frees the DownloadState allocated in add_new_download.
    int end(void *user_cb_data, TransferStatus status, const char *msg) override
    {
      std::unique_ptr<DownloadState> state(static_cast<DownloadState *>(user_cb_data));
      std::string description = state ? state->description : "package";

      switch (status) {
      case TransferStatus::SUCCESSFUL:
      case TransferStatus::ALREADYEXISTS:
        emit_progress_line(progress_cb, "Download ready: " + description);
        break;
      case TransferStatus::ERROR:
        if (msg && *msg) {
          emit_progress_line(progress_cb, "Download failed: " + description + " (" + std::string(msg) + ")");
        } else {
          emit_progress_line(progress_cb, "Download failed: " + description);
        }
        break;
      }

      return OK;
    }

    // Report failed mirrors while allowing libdnf to continue with other mirrors.
    int mirror_failure(void *, const char *msg, const char *url, const char *) override
    {
      std::string line = "Download mirror failed";
      if (url && *url) {
        line += ": ";
        line += url;
      }
      if (msg && *msg) {
        line += " (";
        line += msg;
        line += ")";
      }

      emit_progress_line(progress_cb, line);
      return OK;
    }

private:
    struct DownloadState {
      std::string description;
      int last_reported_bucket = -1;
    };

    TransactionProgressCallback progress_cb;
  };

  // libdnf calls this object while rpm applies the transaction.
  // Keep this log short because the progress window appends each line permanently.
  class StreamingTransactionCallbacks final : public libdnf5::rpm::TransactionCallbacks {
public:
    explicit StreamingTransactionCallbacks(TransactionProgressCallback progress_cb)
        : progress_cb(std::move(progress_cb))
    {
    }

    void before_begin(uint64_t total) override
    {
      (void)total;
      emit_progress_line(progress_cb, "Running transaction.");
    }

    void after_complete(bool success) override
    {
      if (!success) {
        emit_progress_line(progress_cb, "RPM transaction failed.");
      }
    }

    void verify_start(uint64_t total) override
    {
      (void)total;
      emit_progress_line(progress_cb, "Verifying package files.");
    }

    void transaction_start(uint64_t total) override
    {
      (void)total;
      emit_progress_line(progress_cb, "Preparing transaction.");
    }

    void elem_progress(const libdnf5::base::TransactionPackage &item, uint64_t amount, uint64_t total) override
    {
      emit_progress_line(progress_cb, format_transaction_item_progress(item, amount, total));
    }

    void script_error(const libdnf5::base::TransactionPackage *,
                      libdnf5::rpm::Nevra nevra,
                      ScriptType type,
                      uint64_t return_code) override
    {
      emit_progress_line(progress_cb,
                         "Script failed: " + std::string(script_type_to_string(type)) + " for " +
                             rpm_nevra_label(nevra) + " returned " + std::to_string(return_code));
    }

    void unpack_error(const libdnf5::base::TransactionPackage &item) override
    {
      emit_progress_line(progress_cb, "Unpack failed: " + transaction_package_label(item));
    }

    void cpio_error(const libdnf5::base::TransactionPackage &item) override
    {
      emit_progress_line(progress_cb, "Archive unpack failed: " + transaction_package_label(item));
    }

private:
    TransactionProgressCallback progress_cb;
  };

} // namespace

// -----------------------------------------------------------------------------
// Create the download callback object used while transaction packages download.
// -----------------------------------------------------------------------------
std::unique_ptr<libdnf5::repo::DownloadCallbacks>
make_streaming_download_callbacks(TransactionProgressCallback progress_cb)
{
  return std::make_unique<StreamingDownloadCallbacks>(std::move(progress_cb));
}

// -----------------------------------------------------------------------------
// Create the rpm callback object used while rpm applies the transaction.
// -----------------------------------------------------------------------------
std::unique_ptr<libdnf5::rpm::TransactionCallbacks>
make_streaming_transaction_callbacks(TransactionProgressCallback progress_cb)
{
  return std::make_unique<StreamingTransactionCallbacks>(std::move(progress_cb));
}

// -----------------------------------------------------------------------------
// Remember which Base needs its download callbacks cleared after apply.
// -----------------------------------------------------------------------------
DownloadCallbacksReset::DownloadCallbacksReset(libdnf5::Base &base)
    : base(base)
{
}

// -----------------------------------------------------------------------------
// Clear download callbacks before returning the shared Base to other backend work.
// -----------------------------------------------------------------------------
DownloadCallbacksReset::~DownloadCallbacksReset()
{
  base.set_download_callbacks(std::unique_ptr<libdnf5::repo::DownloadCallbacks>());
}

} // namespace dnf_backend_transaction_internal
