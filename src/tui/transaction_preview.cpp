// -----------------------------------------------------------------------------
// src/tui/transaction_preview.cpp
// -----------------------------------------------------------------------------
#include "transaction_preview.hpp"

#include "dnf5daemon_client/transaction_service_client.hpp"
#include "dnf_backend/base_manager.hpp"
#include "ui/transaction/pending_transaction_request.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace tui {
namespace {

  bool contains_mark(const std::vector<TransactionPreviewPackage> &packages, const PendingAction &action)
  {
    return std::any_of(packages.begin(), packages.end(), [&](const TransactionPreviewPackage &package) {
      return package.label == action.nevra && package.name_arch_key() == action.package_key;
    });
  }

  void
  append_section(std::ostringstream &out, const char *title, const std::vector<TransactionPreviewPackage> &packages)
  {
    if (packages.empty()) {
      return;
    }
    out << "\n" << title << " (" << packages.size() << ")\n";
    for (const auto &package : packages) {
      out << "  " << package.label;
      if (package.additional_removal) {
        out << " [additional removal]";
      }
      out << "\n";
    }
  }

} // namespace

std::vector<std::string>
preview_mark_issues(const TransactionPreview &preview,
                    const std::vector<PendingMark> &marks,
                    const std::set<std::string> &installonly_nevras)
{
  std::vector<std::string> issues;
  issues.reserve(marks.size());
  for (const auto &mark : marks) {
    const auto &action = mark.action;
    bool found = false;
    switch (action.type) {
    case PendingAction::INSTALL:
      found = contains_mark(preview.install, action);
      break;
    case PendingAction::UPGRADE:
      // Installonly upgrades can be represented as installation of the new version.
      found = contains_mark(preview.upgrade, action) ||
          (installonly_nevras.count(action.nevra) > 0 && contains_mark(preview.install, action));
      break;
    case PendingAction::DOWNGRADE:
      found = contains_mark(preview.downgrade, action);
      break;
    case PendingAction::REMOVE:
      found = contains_mark(preview.remove, action);
      break;
    case PendingAction::REINSTALL:
      found = contains_mark(preview.reinstall, action);
      break;
    }
    if (found) {
      issues.emplace_back();
    } else {
      issues.push_back(std::string("Preview mismatch: resolved transaction does not include the marked ") +
                       pending_action_name(action.type) + ": " + action.nevra);
    }
  }
  return issues;
}

namespace {

  // Verify installonly classification only when an upgrade resolves as an install.
  // A daemon target alone does not carry libdnf5's installonly classification.
  std::set<std::string> verified_installonly_nevras(const TransactionPreview &preview,
                                                    const std::vector<PendingMark> &marks,
                                                    GCancellable *cancel)
  {
    std::vector<std::string> nevras;
    for (const auto &mark : marks) {
      if (mark.action.type == PendingAction::UPGRADE && !contains_mark(preview.upgrade, mark.action) &&
          contains_mark(preview.install, mark.action)) {
        nevras.push_back(mark.action.nevra);
      }
    }
    std::set<std::string> verified;
    if (!nevras.empty()) {
      const auto packages = dnf_backend_get_available_package_metadata_by_nevras_interruptible(nevras, cancel);
      if (BaseManager::instance().current_repo_state() == BaseRepoState::LIVE_METADATA) {
        for (const auto &package : packages) {
          if (package.installonly) {
            verified.insert(package.nevra);
          }
        }
      }
    }
    return verified;
  }

  PreviewResult resolve_pending_preview(const TransactionRequest &request,
                                        const std::vector<PendingMark> &marks,
                                        GCancellable *cancel,
                                        PreviewResult result)
  {
    auto session = std::make_unique<PreparedTransaction>();
    TransactionPreview preview;
    bool key_requested = false;
    const bool resolved = transaction_service_client_preview_request(
        request,
        preview,
        session->path_for_resolution(),
        result.error,
        [&key_requested](const TransactionKeyImportRequest &) {
          key_requested = true;
          return false;
        },
        cancel);
#ifdef DNFUI_BUILD_TESTS
    result.session_path_for_tests = session->path();
#endif
    if (key_requested) {
      result.error =
          "A repository signing key requires approval. Key import is not supported in the terminal frontend.";
      return result;
    }
    if (!resolved) {
      if (result.error.empty()) {
        result.error = "Could not resolve the transaction.";
      }
      return result;
    }
    if (g_cancellable_is_cancelled(cancel)) {
      result.error = "Transaction preview cancelled.";
      return result;
    }
    const auto installonly_nevras = verified_installonly_nevras(preview, marks, cancel);
    if (g_cancellable_is_cancelled(cancel)) {
      result.error = "Transaction preview cancelled.";
      return result;
    }
    result.pending_issues = preview_mark_issues(preview, marks, installonly_nevras);
    for (const auto &issue : result.pending_issues) {
      if (!issue.empty() && result.error.empty()) {
        result.error = issue;
      }
    }
    if (!result.error.empty()) {
      return result;
    }
    if (preview.empty()) {
      result.error = "The resolved transaction contains no package actions.";
      return result;
    }

    if (session->path().empty()) {
      result.error = "dnf5daemon returned no prepared session.";
      return result;
    }
    preview.identify_additional_removals(request.remove);
    result.preview = std::move(preview);
    session->set_offline_mode(result.preview.requires_offline);
    result.prepared = std::move(session);
    return result;
  }

} // namespace

PreviewResult
preview_pending_marks(const std::vector<PendingMark> &marks,
                      GCancellable *cancel,
                      const std::shared_ptr<std::atomic<bool>> &cancel_flag)
{
  PreviewResult result;
  if (marks.empty()) {
    result.error = "No pending actions.";
    return result;
  }

  result.pending_issues = validate_pending_marks(marks, cancel, cancel_flag);
  if (g_cancellable_is_cancelled(cancel) || result.pending_issues.size() != marks.size()) {
    result.error = "Pending action validation did not finish.";
    return result;
  }
  for (size_t i = 0; i < marks.size(); ++i) {
    if (!result.pending_issues[i].empty()) {
      result.error = marks[i].action.nevra + ": " + result.pending_issues[i];
      return result;
    }
  }

  std::vector<PendingAction> actions;
  actions.reserve(marks.size());
  for (const auto &mark : marks) {
    actions.push_back(mark.action);
  }

  TransactionRequest request;
  if (!pending_transaction_build_request(actions, request, result.error) || !request.validate(result.error) ||
      !pending_transaction_validate_request(request, result.error)) {
    return result;
  }
  if (g_cancellable_is_cancelled(cancel)) {
    result.error = "Transaction preview cancelled.";
    return result;
  }

  return resolve_pending_preview(request, marks, cancel, std::move(result));
}

#ifdef DNFUI_BUILD_TESTS
PreviewResult
resolve_preview_for_tests(const TransactionRequest &request, const std::vector<PendingMark> &marks)
{
  return resolve_pending_preview(request, marks, nullptr, {});
}
#endif

std::string
format_preview(const TransactionPreview &preview)
{
  std::ostringstream out;
  out << "No packages have been changed.\n";
  if (preview.requires_offline) {
    out << "\nThese changes would require preparation for the next reboot.\n";
  }
  if (!preview.resolve_warnings.empty()) {
    out << "\nWarnings\n" << preview.resolve_warnings << "\n";
  }
  append_section(out, "Install", preview.install);
  append_section(out, "Upgrade", preview.upgrade);
  append_section(out, "Downgrade", preview.downgrade);
  append_section(out, "Reinstall", preview.reinstall);
  append_section(out, "Remove", preview.remove);
  append_section(out, "Replace", preview.replaced);

  const auto delta = preview.disk_space_delta;
  const auto bytes = delta < 0 ? static_cast<guint64>(-(delta + 1)) + 1 : static_cast<guint64>(delta);
  gchar *size = g_format_size(bytes);
  out << "\nDisk space: " << size << (delta < 0 ? " freed" : " additional") << "\n";
  g_free(size);
  return out.str();
}

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
