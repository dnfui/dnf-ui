// -----------------------------------------------------------------------------
// dnf_transaction_internal.hpp
// Internal helpers shared by the transaction backend files.
//
// These declarations are not part of the UI contract.
// They keep transaction resolving, progress text, and libdnf callback handling
// in separate files while still using one backend implementation.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <memory>
#include <string>
#include <vector>

#include <libdnf5/base/base.hpp>
#include <libdnf5/base/transaction_package.hpp>
#include <libdnf5/repo/download_callbacks.hpp>
#include <libdnf5/rpm/transaction_callbacks.hpp>

namespace dnf_backend_transaction_internal {

// -----------------------------------------------------------------------------
// Format a compact package spec list for transaction error details.
// -----------------------------------------------------------------------------
std::string format_specs(const std::vector<std::string> &specs);

// -----------------------------------------------------------------------------
// Send one progress line to the caller if a progress callback was provided.
// -----------------------------------------------------------------------------
void emit_progress_line(const TransactionProgressCallback &progress_cb, const std::string &message);

// -----------------------------------------------------------------------------
// Send a multi-line error message to the caller as separate progress lines.
// -----------------------------------------------------------------------------
void emit_progress_block(const TransactionProgressCallback &progress_cb, const std::string &message);

// -----------------------------------------------------------------------------
// Return the action label used in preview and progress text.
// -----------------------------------------------------------------------------
std::string transaction_action_label(libdnf5::base::TransactionPackage::Action action);

// -----------------------------------------------------------------------------
// Return the package label used in preview and progress text.
// -----------------------------------------------------------------------------
std::string transaction_package_label(const libdnf5::base::TransactionPackage &item);

// -----------------------------------------------------------------------------
// Create callbacks that report package download progress.
// -----------------------------------------------------------------------------
std::unique_ptr<libdnf5::repo::DownloadCallbacks>
make_streaming_download_callbacks(TransactionProgressCallback progress_cb);

// -----------------------------------------------------------------------------
// Create callbacks that report rpm transaction progress.
// -----------------------------------------------------------------------------
std::unique_ptr<libdnf5::rpm::TransactionCallbacks>
make_streaming_transaction_callbacks(TransactionProgressCallback progress_cb);

// -----------------------------------------------------------------------------
// Clears Base download callbacks when transaction apply leaves scope.
// -----------------------------------------------------------------------------
class DownloadCallbacksReset {
  public:
  explicit DownloadCallbacksReset(libdnf5::Base &base);

  ~DownloadCallbacksReset();

  private:
  libdnf5::Base &base;
};

} // namespace dnf_backend_transaction_internal
