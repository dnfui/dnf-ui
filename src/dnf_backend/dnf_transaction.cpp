// -----------------------------------------------------------------------------
// dnf_transaction.cpp
// Transaction preview and apply
//
// Owns libdnf5 Goal resolution, transaction preview model generation, and
// transaction execution. Progress callback adapters live in their own file so
// this file can stay focused on what is resolved and when it is applied.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_backend.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend/dnf_transaction_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <libdnf5/base/base.hpp>
#include <libdnf5/base/goal.hpp>
#include <libdnf5/base/transaction.hpp>
#include <libdnf5/base/transaction_package.hpp>
#include <libdnf5/rpm/package_query.hpp>

namespace tx = dnf_backend_transaction_internal;

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

  tx::emit_progress_line(progress_cb, "Resolving dependency changes...");

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
    tx::emit_progress_block(progress_cb, error_out);
    return false;
  }

  if (transaction.get_transaction_packages().empty()) {
    if (upgrade_all) {
      tx::emit_progress_line(progress_cb, "No package updates are available.");
      transaction_out = std::make_unique<libdnf5::base::Transaction>(std::move(transaction));
      return true;
    }

    std::ostringstream oss;
    oss << "No packages in transaction (nothing to do).\n"
        << "Install specs: " << tx::format_specs(install_nevras) << "\n"
        << "Remove specs: " << tx::format_specs(remove_nevras) << "\n"
        << "Reinstall specs: " << tx::format_specs(reinstall_nevras) << "\n"
        << "Upgrade all: " << (upgrade_all ? "yes" : "no") << "\n";
    error_out = oss.str();
    tx::emit_progress_block(progress_cb, error_out);
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

  const std::string label = tx::transaction_package_label(item);
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
  // libdnf may return the same resolved packages in a different order.
  // Compare sorted copies so Apply only rejects real package changes.
  auto sorted = [](std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    return values;
  };

  return sorted(left.install) == sorted(right.install) && sorted(left.upgrade) == sorted(right.upgrade) &&
      sorted(left.downgrade) == sorted(right.downgrade) && sorted(left.reinstall) == sorted(right.reinstall) &&
      sorted(left.remove) == sorted(right.remove) && left.disk_space_delta == right.disk_space_delta;
}

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Test-only access to the preview comparison used before apply.
// -----------------------------------------------------------------------------
bool
dnf_backend_testonly_transaction_previews_match(const TransactionPreview &left, const TransactionPreview &right)
{
  return transaction_previews_match(left, right);
}
#endif

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
        tx::emit_progress_line(progress_cb, error_out);
        return false;
      }
    }

    if (transaction->get_transaction_packages().empty()) {
      error_out = "No package updates are available.";
      tx::emit_progress_line(progress_cb, error_out);
      return false;
    }

    tx::emit_progress_line(progress_cb,
                           "Resolved " + std::to_string(transaction->get_transaction_packages_count()) +
                               " package item" + (transaction->get_transaction_packages_count() == 1 ? "." : "s."));

    for (const auto &item : transaction->get_transaction_packages()) {
      tx::emit_progress_line(
          progress_cb, tx::transaction_action_label(item.get_action()) + ": " + tx::transaction_package_label(item));
    }

    // Register download callbacks before transaction download starts. libdnf
    // calls them while packages are downloaded, and they feed progress_cb.
    base.set_download_callbacks(tx::make_streaming_download_callbacks(progress_cb));
    tx::DownloadCallbacksReset download_callbacks_reset(base);
    tx::emit_progress_line(progress_cb, "Starting package downloads...");
    DNFUI_TRACE("Transaction download start");
    transaction->download();
    DNFUI_TRACE("Transaction download done");
    tx::emit_progress_line(progress_cb, "Package downloads finished.");

    transaction->set_callbacks(tx::make_streaming_transaction_callbacks(progress_cb));
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
      tx::emit_progress_block(progress_cb, error_out);
      return false;
    }

    return true;
  } catch (const std::exception &e) {
    error_out = e.what();
    tx::emit_progress_line(progress_cb, error_out);
    return false;
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
