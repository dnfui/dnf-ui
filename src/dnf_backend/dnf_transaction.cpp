// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_transaction.cpp
// Transaction preview and apply helpers
//
// Owns libdnf5 Goal resolution, transaction preview model generation, download
// progress callbacks, and execution. Authorization remains the responsibility
// of the transaction service before it calls into this backend layer.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_backend.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <libdnf5/base/base.hpp>
#include <libdnf5/base/goal.hpp>
#include <libdnf5/base/transaction.hpp>
#include <libdnf5/base/transaction_package.hpp>
#include <libdnf5/repo/download_callbacks.hpp>
#include <libdnf5/rpm/nevra.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <libdnf5/rpm/transaction_callbacks.hpp>

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Test-only hook that makes the upgrade-all resolver reach the empty transaction
// path without depending on live repository update state.
// -----------------------------------------------------------------------------
static bool
test_skip_upgrade_all_goal_job_requested()
{
  const char *skip_upgrade_job = std::getenv("DNFUI_TEST_SKIP_UPGRADE_ALL_GOAL_JOB");
  return skip_upgrade_job && std::string(skip_upgrade_job) == "1";
}
#else
// -----------------------------------------------------------------------------
// Do not change upgrade-all goal construction in production builds.
// -----------------------------------------------------------------------------
static bool
test_skip_upgrade_all_goal_job_requested()
{
  return false;
}
#endif

// -----------------------------------------------------------------------------
// Format a short, bounded summary of package specs for diagnostic error paths.
//
// Output format:
//   - "<count>" if empty
//   - "<count> (spec1, spec2, ...)" with a bounded preview if non-empty
//
// The output is intentionally truncated so failure details stay readable even
// when the UI sends a large transaction request.
// -----------------------------------------------------------------------------
static std::string
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
// Send one progress line to the caller. In normal UI use the caller is the
// transaction service, which turns this line into a D-Bus Progress signal.
// -----------------------------------------------------------------------------
static void
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
static void
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
static std::string
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
// Convert one transaction action to the verb used when rpm starts package work.
// -----------------------------------------------------------------------------
static std::string
transaction_action_running_label(libdnf5::base::TransactionPackage::Action action)
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
// Produce the stable package label used by transaction previews and progress
// logs. Full NEVRA is used so dependency-driven actions remain unambiguous.
// -----------------------------------------------------------------------------
static std::string
transaction_package_label(const libdnf5::base::TransactionPackage &item)
{
  return item.get_package().get_nevra();
}

// -----------------------------------------------------------------------------
// Format an rpm callback NEVRA. Script callbacks may refer to packages that are
// not direct transaction items.
// -----------------------------------------------------------------------------
static std::string
rpm_nevra_label(const libdnf5::rpm::Nevra &nevra)
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

// libdnf calls this object during transaction downloads. Each callback creates
// a plain text progress line and sends it through progress_cb.
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

  // libdnf may call this often. Report only ten percent steps so the progress
  // window gets useful updates without being flooded.
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

  // One package download has finished. The unique_ptr frees the DownloadState
  // allocated in add_new_download.
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

// libdnf calls this object while rpm applies the transaction. Keep this log
// short because the progress window appends each line permanently.
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
                       "Script failed: " + std::string(script_type_to_string(type)) + " for " + rpm_nevra_label(nevra) +
                           " returned " + std::to_string(return_code));
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

// Reset Base download callbacks when leaving transaction apply scope.
class DownloadCallbacksReset {
  public:
  // -----------------------------------------------------------------------------
  // Remember which Base needs its download callbacks cleared.
  // -----------------------------------------------------------------------------
  explicit DownloadCallbacksReset(libdnf5::Base &base)
      : base(base)
  {
  }

  // -----------------------------------------------------------------------------
  // Clear download callbacks when transaction apply finishes.
  // -----------------------------------------------------------------------------
  ~DownloadCallbacksReset()
  {
    base.set_download_callbacks(std::unique_ptr<libdnf5::repo::DownloadCallbacks>());
  }

  private:
  libdnf5::Base &base;
};

// -----------------------------------------------------------------------------
// Prefer removing exact installed packages by rpmdb object when the UI passes
// a full NEVRA. This keeps local-only RPM removals working even when libdnf5
// does not re-resolve the same string spec back to the installed package.
// -----------------------------------------------------------------------------
static void
add_remove_request(libdnf5::Base &base, libdnf5::Goal &goal, const std::string &spec)
{
  libdnf5::rpm::PackageQuery installed_query(base);
  installed_query.filter_installed();
  installed_query.filter_nevra(spec);

  if (!installed_query.empty()) {
    goal.add_rpm_remove(installed_query);
    return;
  }

  goal.add_rpm_remove(spec);
}

// -----------------------------------------------------------------------------
// Resolve the transaction through one shared code path so preview and apply
// use identical resolution logic.
// -----------------------------------------------------------------------------
static bool
resolve_transaction_plan(libdnf5::Base &base,
                         const std::vector<std::string> &install_nevras,
                         const std::vector<std::string> &remove_nevras,
                         const std::vector<std::string> &reinstall_nevras,
                         std::string &error_out,
                         const TransactionProgressCallback &progress_cb,
                         std::unique_ptr<libdnf5::base::Transaction> &transaction_out,
                         bool upgrade_all)
{
  transaction_out.reset();

  if (!upgrade_all && install_nevras.empty() && remove_nevras.empty() && reinstall_nevras.empty()) {
    error_out = "No packages specified in transaction.";
    return false;
  }

  if (upgrade_all && (!install_nevras.empty() || !remove_nevras.empty() || !reinstall_nevras.empty())) {
    error_out = "Upgrade all cannot be combined with other package actions.";
    return false;
  }

  libdnf5::Goal goal(base);

  emit_progress_line(progress_cb, "Resolving dependency changes...");

  // Let package removal also remove installed packages that depend on the
  // selected package, matching DNF's normal transaction behavior.
  if (!remove_nevras.empty()) {
    goal.set_allow_erasing(true);
  }

  // The UI currently passes package specs as NEVRA strings from the package list.
  for (const auto &spec : install_nevras) {
    goal.add_rpm_install(spec);
  }

  for (const auto &spec : remove_nevras) {
    add_remove_request(base, goal, spec);
  }

  for (const auto &spec : reinstall_nevras) {
    goal.add_rpm_reinstall(spec);
  }

  if (upgrade_all && !test_skip_upgrade_all_goal_job_requested()) {
    goal.add_rpm_upgrade();
  }

  auto transaction = goal.resolve();

  auto goal_problem = transaction.get_problems();
  if (goal_problem != libdnf5::GoalProblem::NO_PROBLEM) {
    std::ostringstream oss;
    oss << "Unable to resolve transaction.\n";

    for (const auto &log : transaction.get_resolve_logs_as_strings()) {
      oss << "  " << log << "\n";
    }

    error_out = oss.str();
    emit_progress_block(progress_cb, error_out);
    return false;
  }

  if (transaction.get_transaction_packages().empty()) {
    if (upgrade_all) {
      emit_progress_line(progress_cb, "No package updates are available.");
      transaction_out = std::make_unique<libdnf5::base::Transaction>(std::move(transaction));
      return true;
    }

    std::ostringstream oss;
    oss << "No packages in transaction (nothing to do).\n"
        << "Install specs: " << format_specs(install_nevras) << "\n"
        << "Remove specs: " << format_specs(remove_nevras) << "\n"
        << "Reinstall specs: " << format_specs(reinstall_nevras) << "\n"
        << "Upgrade all: " << (upgrade_all ? "yes" : "no") << "\n";
    error_out = oss.str();
    emit_progress_block(progress_cb, error_out);
    return false;
  }

  transaction_out = std::make_unique<libdnf5::base::Transaction>(std::move(transaction));
  return true;
}

// -----------------------------------------------------------------------------
// Add one resolved transaction item to the confirmation preview model.
// -----------------------------------------------------------------------------
static void
append_preview_item(TransactionPreview &preview, const libdnf5::base::TransactionPackage &item)
{
  using Action = libdnf5::base::TransactionPackage::Action;

  const std::string label = transaction_package_label(item);
  const long long install_size = static_cast<long long>(item.get_package().get_install_size());

  switch (item.get_action()) {
  case Action::INSTALL:
    preview.install.push_back(label);
    preview.disk_space_delta += install_size;
    break;
  case Action::UPGRADE:
    preview.upgrade.push_back(label);
    preview.disk_space_delta += install_size;
    break;
  case Action::DOWNGRADE:
    preview.downgrade.push_back(label);
    preview.disk_space_delta += install_size;
    break;
  case Action::REINSTALL:
    preview.reinstall.push_back(label);
    break;
  case Action::REMOVE:
    preview.remove.push_back(label);
    preview.disk_space_delta -= install_size;
    break;
  case Action::REPLACED:
    preview.disk_space_delta -= install_size;
    break;
  default:
    break;
  }
}

// -----------------------------------------------------------------------------
// Compare the package actions the user approved with the actions resolved at apply time.
// Any difference means the package state changed after preview.
// -----------------------------------------------------------------------------
static bool
transaction_previews_match(const TransactionPreview &left, const TransactionPreview &right)
{
  return left.install == right.install && left.upgrade == right.upgrade && left.downgrade == right.downgrade &&
      left.reinstall == right.reinstall && left.remove == right.remove &&
      left.disk_space_delta == right.disk_space_delta;
}

// -----------------------------------------------------------------------------
// Resolve the final transaction and group the resulting package actions for
// the confirmation dialog. This deliberately shares resolve_transaction_plan
// with apply so the preview and actual transaction use identical dependency
// resolution logic.
// -----------------------------------------------------------------------------
bool
dnf_backend_preview_transaction(const std::vector<std::string> &install_nevras,
                                const std::vector<std::string> &remove_nevras,
                                const std::vector<std::string> &reinstall_nevras,
                                TransactionPreview &preview,
                                std::string &error_out,
                                const TransactionProgressCallback &progress_cb,
                                bool upgrade_all)
{
  error_out.clear();
  preview = TransactionPreview();

  try {
    DNFUI_TRACE("Transaction preview start install=%zu remove=%zu reinstall=%zu upgrade_all=%d",
                install_nevras.size(),
                remove_nevras.size(),
                reinstall_nevras.size(),
                upgrade_all ? 1 : 0);
    auto [base, guard] = BaseManager::instance().acquire_write();
    std::unique_ptr<libdnf5::base::Transaction> transaction;

    if (!resolve_transaction_plan(
            base, install_nevras, remove_nevras, reinstall_nevras, error_out, progress_cb, transaction, upgrade_all)) {
      DNFUI_TRACE("Transaction preview resolve failed: %s", error_out.c_str());
      return false;
    }

    for (const auto &item : transaction->get_transaction_packages()) {
      append_preview_item(preview, item);
    }

    DNFUI_TRACE("Transaction preview done items=%zu", transaction->get_transaction_packages_count());
    return true;
  } catch (const std::exception &e) {
    error_out = e.what();
    DNFUI_TRACE("Transaction preview failed: %s", e.what());
    return false;
  }
}

// -----------------------------------------------------------------------------
// Apply a resolved package transaction after the caller has completed any
// required authorization. The transaction service enforces Polkit before
// invoking this function; direct backend callers are expected to run in tests or
// another already-authorized context.
// -----------------------------------------------------------------------------
bool
dnf_backend_apply_transaction(const std::vector<std::string> &install_nevras,
                              const std::vector<std::string> &remove_nevras,
                              const std::vector<std::string> &reinstall_nevras,
                              std::string &error_out,
                              const TransactionProgressCallback &progress_cb,
                              bool upgrade_all,
                              const TransactionPreview *approved_preview)
{
  error_out.clear();

  try {
    DNFUI_TRACE("Transaction apply start install=%zu remove=%zu reinstall=%zu upgrade_all=%d",
                install_nevras.size(),
                remove_nevras.size(),
                reinstall_nevras.size(),
                upgrade_all ? 1 : 0);
    // Exclusive access to shared libdnf Base for transactional changes.
    auto [base, guard] = BaseManager::instance().acquire_write();
    std::unique_ptr<libdnf5::base::Transaction> transaction;

    if (!resolve_transaction_plan(
            base, install_nevras, remove_nevras, reinstall_nevras, error_out, progress_cb, transaction, upgrade_all)) {
      DNFUI_TRACE("Transaction apply resolve failed: %s", error_out.c_str());
      return false;
    }

    // The user approved the preview shown by the service.
    // If the package state changed before Apply, stop before downloads or RPM transaction work starts.
    if (approved_preview) {
      TransactionPreview resolved_preview;
      for (const auto &item : transaction->get_transaction_packages()) {
        append_preview_item(resolved_preview, item);
      }

      if (!transaction_previews_match(*approved_preview, resolved_preview)) {
        error_out =
            "Package state changed after the preview was prepared. Review the transaction again before applying.";
        DNFUI_TRACE("Transaction apply rejected because resolved preview changed");
        emit_progress_line(progress_cb, error_out);
        return false;
      }
    }

    if (transaction->get_transaction_packages().empty()) {
      error_out = "No package updates are available.";
      emit_progress_line(progress_cb, error_out);
      return false;
    }

    emit_progress_line(progress_cb,
                       "Resolved " + std::to_string(transaction->get_transaction_packages_count()) + " package item" +
                           (transaction->get_transaction_packages_count() == 1 ? "." : "s."));

    for (const auto &item : transaction->get_transaction_packages()) {
      emit_progress_line(progress_cb,
                         transaction_action_label(item.get_action()) + ": " + transaction_package_label(item));
    }

    // Register download callbacks before transaction download starts. libdnf
    // calls them while packages are downloaded, and they feed progress_cb.
    base.set_download_callbacks(std::make_unique<StreamingDownloadCallbacks>(progress_cb));
    DownloadCallbacksReset download_callbacks_reset(base);
    emit_progress_line(progress_cb, "Starting package downloads...");
    DNFUI_TRACE("Transaction download start");
    transaction->download();
    DNFUI_TRACE("Transaction download done");
    emit_progress_line(progress_cb, "Package downloads finished.");

    transaction->set_callbacks(std::make_unique<StreamingTransactionCallbacks>(progress_cb));
    DNFUI_TRACE("Transaction run start");
    auto run_result = transaction->run();
    DNFUI_TRACE("Transaction run done result=%d", static_cast<int>(run_result));
    if (run_result != libdnf5::base::Transaction::TransactionRunResult::SUCCESS) {
      std::ostringstream oss;
      oss << "Transaction failed: " << libdnf5::base::Transaction::transaction_result_to_string(run_result) << " (code "
          << static_cast<int>(run_result) << ").\n";

      for (const auto &msg : transaction->get_transaction_problems()) {
        oss << "  " << msg << "\n";
      }

      auto rpm_messages = transaction->get_rpm_messages();
      if (!rpm_messages.empty()) {
        oss << "RPM messages:\n";
        for (const auto &msg : rpm_messages) {
          oss << "  " << msg << "\n";
        }
      }

      std::string script_output = transaction->get_last_script_output();
      if (!script_output.empty()) {
        oss << "Last script output:\n" << script_output << "\n";
      }

      error_out = oss.str();
      emit_progress_block(progress_cb, error_out);
      return false;
    }

    return true;
  } catch (const std::exception &e) {
    error_out = e.what();
    emit_progress_line(progress_cb, error_out);
    return false;
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
