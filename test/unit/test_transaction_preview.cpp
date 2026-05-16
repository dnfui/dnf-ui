// -----------------------------------------------------------------------------
// test/unit/test_transaction_preview.cpp
// Backend transaction preview tests
// Covers the public preview API that prepares transaction summaries before the
// service or GUI proceeds to the apply step.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Return true when one progress line contains the expected text
// -----------------------------------------------------------------------------
bool
progress_contains(const std::vector<std::string> &lines, const std::string &needle)
{
  for (const auto &line : lines) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }

  return false;
}

} // namespace

// -----------------------------------------------------------------------------
// Transaction preview request validation tests
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that empty preview requests fail and clear stale preview output.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction preview rejects an empty request")
{
  reset_backend_globals();

  TransactionPreview preview;
  preview.install.push_back("stale");
  preview.disk_space_delta = 123;
  std::string error;

  bool ok = dnf_backend_preview_transaction({}, {}, {}, preview, error);

  REQUIRE_FALSE(ok);
  REQUIRE(error == "No packages specified in transaction.");
  REQUIRE(preview.install.empty());
  REQUIRE(preview.upgrade.empty());
  REQUIRE(preview.downgrade.empty());
  REQUIRE(preview.reinstall.empty());
  REQUIRE(preview.remove.empty());
  REQUIRE(preview.disk_space_delta == 0);
}

// -----------------------------------------------------------------------------
// Verify that impossible install specs return a user-facing resolve error.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction preview reports a friendly resolve error for an impossible package")
{
  reset_backend_globals();

  TransactionPreview preview;
  std::string error;
  std::vector<std::string> progress_lines;

  // clang-format off
  bool ok = dnf_backend_preview_transaction({"___definitely_not_a_real_package_246810___"},
                                            {},
                                            {},
                                            preview,
                                            error,
                                            [&](const std::string &line) { progress_lines.push_back(line); });
  // clang-format on

  REQUIRE_FALSE(ok);
  REQUIRE(error.find("Unable to resolve transaction.") != std::string::npos);
  REQUIRE(progress_contains(progress_lines, "Resolving dependency changes..."));
}

// -----------------------------------------------------------------------------
// Verify that upgrade all preview treats no available updates as success.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction preview accepts empty upgrade-all results")
{
  reset_backend_globals();
  ScopedEnvVar force_empty_upgrade_all("DNFUI_TEST_SKIP_UPGRADE_ALL_GOAL_JOB", "1");

  TransactionPreview preview;
  std::string error = "stale";
  std::vector<std::string> progress_lines;

  bool ok = dnf_backend_preview_transaction(
      {}, {}, {}, preview, error, [&](const std::string &line) { progress_lines.push_back(line); }, true);

  REQUIRE(ok);
  REQUIRE(error.empty());
  REQUIRE(preview.empty());
  REQUIRE(progress_contains(progress_lines, "No package updates are available."));
}

// -----------------------------------------------------------------------------
// Verify that preview rejects upgrade all mixed with explicit package specs.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction preview rejects mixed upgrade-all requests")
{
  reset_backend_globals();

  TransactionPreview preview;
  std::string error;

  bool ok = dnf_backend_preview_transaction({ "example-install-spec" }, {}, {}, preview, error, {}, true);

  REQUIRE_FALSE(ok);
  REQUIRE(error == "Upgrade all cannot be combined with other package actions.");
  REQUIRE(preview.empty());
}

// -----------------------------------------------------------------------------
// Verify that apply refuses an empty upgrade all transaction.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction apply rejects empty upgrade-all results")
{
  reset_backend_globals();
  ScopedEnvVar force_empty_upgrade_all("DNFUI_TEST_SKIP_UPGRADE_ALL_GOAL_JOB", "1");

  std::string error;
  std::vector<std::string> progress_lines;

  bool ok = dnf_backend_apply_transaction(
      {}, {}, {}, error, [&](const std::string &line) { progress_lines.push_back(line); }, true);

  REQUIRE_FALSE(ok);
  REQUIRE(error == "No package updates are available.");
  REQUIRE(progress_contains(progress_lines, "No package updates are available."));
}

// -----------------------------------------------------------------------------
// Verify that apply refuses to run when the resolved transaction no longer
// matches the preview that was shown to the user.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction apply rejects changed approved preview")
{
  reset_backend_globals();
  ScopedEnvVar force_empty_upgrade_all("DNFUI_TEST_SKIP_UPGRADE_ALL_GOAL_JOB", "1");

  TransactionPreview approved_preview;
  approved_preview.install.push_back("package-from-old-preview");

  std::string error;
  std::vector<std::string> progress_lines;

  bool ok = dnf_backend_apply_transaction(
      {}, {}, {}, error, [&](const std::string &line) { progress_lines.push_back(line); }, true, &approved_preview);

  REQUIRE_FALSE(ok);
  REQUIRE(error ==
          "Package state changed after the preview was prepared. Review the transaction again before applying.");
  REQUIRE(progress_contains(progress_lines, "Package state changed after the preview was prepared."));
}

// -----------------------------------------------------------------------------
// Verify that the check before apply compares action sets, not resolver order.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction preview comparison accepts matching package actions in different order")
{
  TransactionPreview approved_preview;
  approved_preview.install = { "b-package-1-1.x86_64", "a-package-1-1.x86_64" };
  approved_preview.upgrade = { "z-package-2-1.x86_64", "c-package-2-1.x86_64" };
  approved_preview.remove = { "old-package-1-1.x86_64" };
  approved_preview.disk_space_delta = 4096;

  TransactionPreview resolved_preview;
  resolved_preview.install = { "a-package-1-1.x86_64", "b-package-1-1.x86_64" };
  resolved_preview.upgrade = { "c-package-2-1.x86_64", "z-package-2-1.x86_64" };
  resolved_preview.remove = { "old-package-1-1.x86_64" };
  resolved_preview.disk_space_delta = 4096;

  REQUIRE(dnf_backend_testonly_transaction_previews_match(approved_preview, resolved_preview));
}

// -----------------------------------------------------------------------------
// Verify that the check before apply still rejects changed package actions.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction preview comparison rejects changed package actions")
{
  TransactionPreview approved_preview;
  approved_preview.install = { "a-package-1-1.x86_64" };
  approved_preview.disk_space_delta = 4096;

  TransactionPreview resolved_preview;
  resolved_preview.install = { "different-package-1-1.x86_64" };
  resolved_preview.disk_space_delta = 4096;

  REQUIRE_FALSE(dnf_backend_testonly_transaction_previews_match(approved_preview, resolved_preview));
}

// -----------------------------------------------------------------------------
// Verify that the check before apply still rejects changed disk usage.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction preview comparison rejects changed disk space")
{
  TransactionPreview approved_preview;
  approved_preview.install = { "a-package-1-1.x86_64" };
  approved_preview.disk_space_delta = 4096;

  TransactionPreview resolved_preview;
  resolved_preview.install = { "a-package-1-1.x86_64" };
  resolved_preview.disk_space_delta = 8192;

  REQUIRE_FALSE(dnf_backend_testonly_transaction_previews_match(approved_preview, resolved_preview));
}

// -----------------------------------------------------------------------------
// Transaction preview success path tests
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that preview can resolve reinstall for an installed package row.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction preview resolves a reinstall request for an installed package")
{
  reset_backend_globals();

  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE_FALSE(installed_rows.empty());
  const PackageRow &installed_row = installed_rows.front();

  TransactionPreview preview;
  std::string error;
  std::vector<std::string> progress_lines;

  // clang-format off
  bool ok = dnf_backend_preview_transaction({},
                                            {},
                                            {installed_row.nevra},
                                            preview,
                                            error,
                                            [&](const std::string &line) { progress_lines.push_back(line); });
  // clang-format on

  INFO(error);
  REQUIRE(ok);
  REQUIRE(error.empty());
  REQUIRE(progress_contains(progress_lines, "Resolving dependency changes..."));
  REQUIRE_FALSE(preview.reinstall.empty());
  REQUIRE(std::any_of(preview.reinstall.begin(), preview.reinstall.end(), [&](const std::string &label) {
    return label.find(installed_row.name + "-") != std::string::npos;
  }));
}
