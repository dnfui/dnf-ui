// -----------------------------------------------------------------------------
// dnf_transaction_format.cpp
// Shared text helpers for transaction previews, progress, and errors.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_transaction_internal.hpp"

#include <algorithm>
#include <sstream>

namespace dnf_backend_transaction_internal {

// -----------------------------------------------------------------------------
// Format a short summary of package specs for diagnostic error paths.
//
// Output format:
//   - "<count>" if empty
//   - "<count> (spec1, spec2, ...)" when specs are present
//
// The output shows only the first few specs so failure details stay readable even
// when the UI sends a large transaction request.
// -----------------------------------------------------------------------------
std::string
format_specs(const std::vector<std::string> &specs)
{
  std::ostringstream out;
  out << specs.size();

  if (!specs.empty()) {
    out << " (";

    const size_t limit = std::min<size_t>(specs.size(), 3);
    for (size_t i = 0; i < limit; ++i) {
      if (i > 0) {
        out << ", ";
      }
      out << specs[i];
    }

    if (specs.size() > limit) {
      out << ", ...";
    }

    out << ")";
  }

  return out.str();
}

// -----------------------------------------------------------------------------
// Send one progress line to the caller.
// In normal UI use the caller is the transaction service, which turns this line into a D-Bus Progress signal.
// -----------------------------------------------------------------------------
void
emit_progress_line(const TransactionProgressCallback &progress_cb, const std::string &message)
{
  if (!progress_cb || message.empty()) {
    return;
  }

  progress_cb(message);
}

// -----------------------------------------------------------------------------
// Split a multi-line error into separate progress lines. The progress window
// appends one line at a time.
// -----------------------------------------------------------------------------
void
emit_progress_block(const TransactionProgressCallback &progress_cb, const std::string &message)
{
  if (!progress_cb || message.empty()) {
    return;
  }

  std::istringstream stream(message);
  std::string line;

  while (std::getline(stream, line)) {
    if (!line.empty()) {
      progress_cb(line);
    }
  }
}

// -----------------------------------------------------------------------------
// Convert one libdnf transaction action to the verb used in progress output.
// -----------------------------------------------------------------------------
std::string
transaction_action_label(libdnf5::base::TransactionPackage::Action action)
{
  using Action = libdnf5::base::TransactionPackage::Action;

  switch (action) {
  case Action::INSTALL:
    return "Install";
  case Action::UPGRADE:
    return "Upgrade";
  case Action::DOWNGRADE:
    return "Downgrade";
  case Action::REINSTALL:
    return "Reinstall";
  case Action::REMOVE:
    return "Remove";
  case Action::REPLACED:
    return "Replace";
  case Action::REASON_CHANGE:
    return "Reason change";
  default:
    return "Process";
  }
}

// -----------------------------------------------------------------------------
// Produce the stable package label used by transaction previews and progress
// logs. Full NEVRA is used so dependency-driven actions remain unambiguous.
// -----------------------------------------------------------------------------
std::string
transaction_package_label(const libdnf5::base::TransactionPackage &item)
{
  return item.get_package().get_nevra();
}

} // namespace dnf_backend_transaction_internal
