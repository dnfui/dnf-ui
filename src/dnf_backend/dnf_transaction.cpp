// -----------------------------------------------------------------------------
// dnf_transaction.cpp
// Transaction preview and apply
//
// Owns libdnf5 Goal resolution, transaction preview model generation, and transaction execution.
// Progress callback adapters live in their own file so this file can stay focused
// on what is resolved and when it is applied.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_backend.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend/dnf_internal.hpp"
#include "dnf_backend/dnf_transaction_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
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

// -----------------------------------------------------------------------------
// Test-only hook that injects one unsupported preview action after normal
// transaction items so preview failure can be exercised through the public API.
// -----------------------------------------------------------------------------
static bool
test_preview_injection_requested()
{
  const char *inject_preview_action = std::getenv("DNFUI_TEST_INJECT_UNSUPPORTED_PREVIEW_ACTION");
  return inject_preview_action && std::string(inject_preview_action) == "1";
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

// -----------------------------------------------------------------------------
// Do not inject preview actions in production builds.
// -----------------------------------------------------------------------------
static bool
test_preview_injection_requested()
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
// Remove packages with protected names from one package query.
// -----------------------------------------------------------------------------
static void
remove_packages_by_name(libdnf5::Base &base,
                        libdnf5::rpm::PackageQuery &query,
                        const std::set<std::string> &protected_names)
{
  if (protected_names.empty()) {
    return;
  }

  std::vector<std::string> names { protected_names.begin(), protected_names.end() };
  libdnf5::rpm::PackageQuery protected_packages(base);
  protected_packages.filter_name(names);
  query.difference(protected_packages);
}

// -----------------------------------------------------------------------------
// Add an upgrade-all request without upgrading the running application package.
// -----------------------------------------------------------------------------
static void
add_upgrade_all_request(libdnf5::Base &base, libdnf5::Goal &goal, const TransactionProgressCallback &progress_cb)
{
  std::set<std::string> protected_names = dnf_backend_internal::collect_self_protected_package_names(base);
  if (protected_names.empty()) {
    // If the running app package cannot be identified, keep libdnf's normal Upgrade All behavior.
    goal.add_rpm_upgrade();
    return;
  }

  libdnf5::rpm::PackageQuery protected_upgrades(base);
  protected_upgrades.filter_available();
  protected_upgrades.filter_upgrades();
  protected_upgrades.filter_name(std::vector<std::string> { protected_names.begin(), protected_names.end() });
  if (protected_upgrades.empty()) {
    // If the app itself is not upgradeable, there is nothing to exclude from Upgrade All.
    goal.add_rpm_upgrade();
    return;
  }

  // The app has an upgrade, so resolve all other upgrade candidates.
  libdnf5::rpm::PackageQuery upgrades(base);
  upgrades.filter_available();
  upgrades.filter_upgrades();
  upgrades.filter_latest_evr();
  remove_packages_by_name(base, upgrades, protected_names);

  if (!upgrades.empty()) {
    goal.add_rpm_upgrade(upgrades);
  }
  tx::emit_progress_line(progress_cb, "Skipping DNF UI package while it is running.");
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

  // Let package removal also remove installed packages that depend on the selected package.
  // This matches DNF's normal transaction behavior.
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
    add_upgrade_all_request(base, goal, progress_cb);
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
// Reject unknown actions so the preview cannot silently omit part of the resolved transaction.
// -----------------------------------------------------------------------------
static bool
append_preview_action(TransactionPreview &preview,
                      libdnf5::base::TransactionPackage::Action action,
                      const std::string &label,
                      long long install_size,
                      std::string &error_out)
{
  using Action = libdnf5::base::TransactionPackage::Action;

  switch (action) {
  case Action::INSTALL:
    preview.install.push_back(label);
    preview.disk_space_delta += install_size;
    return true;
  case Action::UPGRADE:
    preview.upgrade.push_back(label);
    preview.disk_space_delta += install_size;
    return true;
  case Action::DOWNGRADE:
    preview.downgrade.push_back(label);
    preview.disk_space_delta += install_size;
    return true;
  case Action::REINSTALL:
    preview.reinstall.push_back(label);
    return true;
  case Action::REMOVE:
    preview.remove.push_back(label);
    preview.disk_space_delta -= install_size;
    return true;
  case Action::REPLACED:
    preview.disk_space_delta -= install_size;
    return true;
  default:
    error_out = "Unsupported transaction action in preview: " + tx::transaction_action_label(action) + ".";
    return false;
  }
}

// -----------------------------------------------------------------------------
// Add one resolved transaction item to the confirmation preview model.
// -----------------------------------------------------------------------------
static bool
append_preview_item(TransactionPreview &preview, const libdnf5::base::TransactionPackage &item, std::string &error_out)
{
  return append_preview_action(preview,
                               item.get_action(),
                               tx::transaction_package_label(item),
                               static_cast<long long>(item.get_package().get_install_size()),
                               error_out);
}

// -----------------------------------------------------------------------------
// Build the confirmation preview model from one resolved transaction.
// Publish the result only after every action has been represented so callers
// never observe a partial preview on failure.
// -----------------------------------------------------------------------------
static bool
build_transaction_preview(const libdnf5::base::Transaction &transaction,
                          TransactionPreview &preview,
                          std::string &error_out,
                          const TransactionProgressCallback &progress_cb)
{
  TransactionPreview built_preview;

  for (const auto &item : transaction.get_transaction_packages()) {
    if (!append_preview_item(built_preview, item, error_out)) {
      tx::emit_progress_line(progress_cb, error_out);
      return false;
    }
  }

  if (test_preview_injection_requested() &&
      !append_preview_action(built_preview,
                             libdnf5::base::TransactionPackage::Action::REASON_CHANGE,
                             "test-injected-preview-action",
                             0,
                             error_out)) {
    tx::emit_progress_line(progress_cb, error_out);
    return false;
  }

  preview = std::move(built_preview);
  return true;
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
// Test-only access to the package-name exclusion used by upgrade-all self-protection.
// -----------------------------------------------------------------------------
bool
dnf_backend_testonly_query_excludes_package_name(const std::string &package_name)
{
  auto [base, guard] = BaseManager::instance().acquire_write();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_name(package_name);
  if (query.empty()) {
    return false;
  }

  remove_packages_by_name(base, query, { package_name });
  return query.empty();
}

// -----------------------------------------------------------------------------
// Test-only access to the preview comparison used before apply.
// -----------------------------------------------------------------------------
bool
dnf_backend_testonly_transaction_previews_match(const TransactionPreview &left, const TransactionPreview &right)
{
  return transaction_previews_match(left, right);
}

// -----------------------------------------------------------------------------
// Test-only hook for preview-builder regression tests.
// -----------------------------------------------------------------------------
bool
dnf_backend_testonly_build_preview_from_actions(const std::vector<int> &action_codes,
                                                TransactionPreview &preview,
                                                std::string &error_out)
{
  TransactionPreview built_preview;

  for (size_t i = 0; i < action_codes.size(); ++i) {
    if (!append_preview_action(built_preview,
                               static_cast<libdnf5::base::TransactionPackage::Action>(action_codes[i]),
                               "test-item-" + std::to_string(i + 1),
                               4096,
                               error_out)) {
      return false;
    }
  }

  preview = std::move(built_preview);
  return true;
}
#endif

// -----------------------------------------------------------------------------
// Resolve the final transaction and group the resulting package actions for the confirmation dialog.
// This deliberately shares resolve_transaction_plan with apply so preview and apply use the same dependency logic.
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

    if (!build_transaction_preview(*transaction, preview, error_out, progress_cb)) {
      DNFUI_TRACE("Transaction preview resolve produced unsupported action: %s", error_out.c_str());
      return false;
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
// Apply a resolved package transaction through libdnf.
// This helper does not perform authorization. The dnf5daemon client path handles
// privileged apply outside this backend.
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
      if (!build_transaction_preview(*transaction, resolved_preview, error_out, progress_cb)) {
        DNFUI_TRACE("Transaction apply rejected because preview build failed: %s", error_out.c_str());
        return false;
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
