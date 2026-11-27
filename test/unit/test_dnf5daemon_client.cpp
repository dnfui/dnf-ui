// -----------------------------------------------------------------------------
// test_dnf5daemon_client.cpp
// dnf5daemon transaction client tests
// Exercises the same preview client path used by the GTK frontend.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "test_utils.hpp"
#include "dnf_backend/base_manager.hpp"
#include "transaction/transaction_preview.hpp"
#include "transaction/transaction_request.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"
#include "dnf5daemon_client/transaction_service_client_parser.hpp"
#include "tui/upgrade_query.hpp"
#include "tui/pending_actions.hpp"
#include "tui/transaction_preview.hpp"

#include <glib.h>
#include <libdnf5/transaction/offline.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Return the package used for daemon preview tests.
// -----------------------------------------------------------------------------
std::string
dnf5daemon_test_install_spec()
{
  const char *spec = g_getenv("DNFUI_TEST_DNF5DAEMON_INSTALL_SPEC");
  if (spec && *spec) {
    return spec;
  }
  return "cowsay";
}

// -----------------------------------------------------------------------------
// Return the package used for daemon downgrade preview tests.
// -----------------------------------------------------------------------------
std::string
dnf5daemon_test_downgrade_spec()
{
  const char *spec = g_getenv("DNFUI_TEST_DNF5DAEMON_DOWNGRADE_SPEC");
  if (spec && *spec) {
    return spec;
  }
  return "";
}

// -----------------------------------------------------------------------------
// Return true when a preview section contains a package label by name.
// -----------------------------------------------------------------------------
bool
preview_section_contains_name(const std::vector<TransactionPreviewPackage> &items, const std::string &name)
{
  return std::any_of(items.begin(), items.end(), [&](const TransactionPreviewPackage &item) {
    return item.label.rfind(name + "-", 0) == 0;
  });
}

bool
preview_section_contains_label(const std::vector<TransactionPreviewPackage> &items, const std::string &label)
{
  return std::any_of(
      items.begin(), items.end(), [&](const TransactionPreviewPackage &item) { return item.label == label; });
}

TransactionPreviewPackage
preview_package(const std::string &name, const std::string &arch = "x86_64")
{
  return { name + "-1.2.3-1." + arch, name, arch };
}

// -----------------------------------------------------------------------------
// Return true when progress contains the expected message.
// -----------------------------------------------------------------------------
bool
progress_contains(const std::vector<std::string> &progress_lines, const std::string &expected)
{
  return std::any_of(progress_lines.begin(), progress_lines.end(), [&](const std::string &line) {
    return line.find(expected) != std::string::npos;
  });
}

// -----------------------------------------------------------------------------
// Verify that cold-offline repository load failures are shown as user-facing preparation errors.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preparation maps repository load failures to a clear message")
{
  const std::string message =
      transaction_service_client_testonly_prepare_error_message("org.rpm.dnf.v0.Error", "Cannot load repositories.");

  REQUIRE(message.find("Repository data is unavailable") != std::string::npos);
  REQUIRE(message.find("refresh repositories") != std::string::npos);
  REQUIRE(message.find("GDBus.Error") == std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that package download failures are shown without the raw D-Bus prefix.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon apply maps package download failures to a clear message")
{
  const std::string message =
      transaction_service_client_testonly_apply_error_message("org.rpm.dnf.v0.Error", "Failed to download packages.");

  REQUIRE(message.find("Required packages could not be downloaded") != std::string::npos);
  REQUIRE(message.find("network connection") != std::string::npos);
  REQUIRE(message.find("GDBus.Error") == std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that only remove-only selected transactions skip available repository loading.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon selected transaction sessions load repositories by request type")
{
  TransactionRequest request;
  request.remove.push_back("test-package.x86_64");
  REQUIRE_FALSE(transaction_service_client_testonly_request_loads_available_repos(request));

  request.install.push_back("other-package.x86_64");
  REQUIRE(transaction_service_client_testonly_request_loads_available_repos(request));

  request = {};
  request.install.push_back("test-package.x86_64");
  REQUIRE(transaction_service_client_testonly_request_loads_available_repos(request));

  request = {};
  request.upgrade.push_back("test-package.x86_64");
  REQUIRE(transaction_service_client_testonly_request_loads_available_repos(request));

  request = {};
  request.downgrade.push_back("test-package-1.0-1.x86_64");
  REQUIRE(transaction_service_client_testonly_request_loads_available_repos(request));

  request = {};
  request.reinstall.push_back("test-package.x86_64");
  REQUIRE(transaction_service_client_testonly_request_loads_available_repos(request));
}

// -----------------------------------------------------------------------------
// Return progress lines as one string so Catch2 can show them on failure.
// -----------------------------------------------------------------------------
std::string
joined_progress_lines(const std::vector<std::string> &progress_lines)
{
  std::ostringstream out;
  for (const auto &line : progress_lines) {
    if (out.tellp() > 0) {
      out << "\n";
    }
    out << line;
  }
  return out.str();
}

// -----------------------------------------------------------------------------
// Skip daemon tests unless the caller explicitly enabled them.
// -----------------------------------------------------------------------------
void
require_dnf5daemon_test_enabled()
{
  const char *enabled = g_getenv("DNFUI_TEST_DNF5DAEMON");
  if (!enabled || std::string(enabled) != "1") {
    SKIP("Set DNFUI_TEST_DNF5DAEMON=1 to run dnf5daemon client tests.");
  }
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that replaced packages are listed as actions instead of only as disk space changes.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview parser represents replaced package actions")
{
  TransactionPreview preview;
  std::string error;

  bool ok = transaction_service_client_testonly_build_preview_from_item(
      "package", "replaced", "test-package", preview, error);

  REQUIRE(ok);
  REQUIRE(error.empty());
  REQUIRE(preview.replaced.size() == 1);
  REQUIRE(preview.replaced[0].label == "test-package-2.0-3.x86_64");
  REQUIRE(preview.replaced[0].name == "test-package");
  REQUIRE(preview.replaced[0].arch == "x86_64");
  REQUIRE(preview.disk_space_delta == -4096);
  REQUIRE_FALSE(preview.empty());
}

// -----------------------------------------------------------------------------
// Verify that downgrade preview items are represented explicitly.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview parser represents downgrade package actions")
{
  TransactionPreview preview;
  std::string error;

  bool ok = transaction_service_client_testonly_build_preview_from_item(
      "package", "downgrade", "test-package", preview, error);

  REQUIRE(ok);
  REQUIRE(error.empty());
  REQUIRE(preview.downgrade.size() == 1);
  REQUIRE(preview.downgrade[0].label == "test-package-2.0-3.x86_64");
  REQUIRE(preview.downgrade[0].name == "test-package");
  REQUIRE(preview.downgrade[0].arch == "x86_64");
  REQUIRE(preview.disk_space_delta == 4096);
  REQUIRE_FALSE(preview.empty());
}

// -----------------------------------------------------------------------------
// Only removals absent from the exact request are marked as additional.
// -----------------------------------------------------------------------------
TEST_CASE("Transaction preview identifies additional removals by exact package identity")
{
  TransactionPreview preview;
  preview.remove.push_back({ "example-1-1.x86_64", "example", "x86_64" });
  preview.remove.push_back({ "example-2-1.x86_64", "example", "x86_64" });
  preview.remove.push_back({ "other-3:1-1.noarch", "other", "noarch" });
  preview.replaced.push_back({ "old-upgrade-1-1.x86_64", "old-upgrade", "x86_64" });

  preview.identify_additional_removals({ "example-1-1.x86_64", "other-3:1-1.noarch" });
  REQUIRE_FALSE(preview.remove[0].additional_removal);
  REQUIRE(preview.remove[1].additional_removal);
  REQUIRE_FALSE(preview.remove[2].additional_removal);
  REQUIRE_FALSE(preview.replaced[0].additional_removal);

  preview.identify_additional_removals({});
  REQUIRE(preview.remove[0].additional_removal);
  REQUIRE(preview.remove[1].additional_removal);
  REQUIRE(preview.remove[2].additional_removal);
  REQUIRE_FALSE(preview.replaced[0].additional_removal);
}

// -----------------------------------------------------------------------------
// Verify that skipped packages produce warnings without executable actions or disk changes.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview parser represents skipped packages as warnings")
{
  const char *reason_skipped = nullptr;
  std::string expected_warning;

  SECTION("conflict")
  {
    reason_skipped = "conflict";
    expected_warning = "test-package-2.0-3.x86_64 was skipped because of a package conflict.";
  }
  SECTION("broken dependency")
  {
    reason_skipped = "broken_dependency";
    expected_warning = "test-package-2.0-3.x86_64 was skipped because of broken dependencies.";
  }
  SECTION("vendor change")
  {
    reason_skipped = "vendor_change";
    expected_warning = "test-package-2.0-3.x86_64 was skipped because of a vendor change restriction.";
  }
  SECTION("unknown reason")
  {
    reason_skipped = "future_reason";
    expected_warning = "test-package-2.0-3.x86_64 was skipped by dnf5daemon: future_reason";
  }
  SECTION("missing reason")
  {
    expected_warning = "test-package-2.0-3.x86_64 was skipped by dnf5daemon.";
  }
  SECTION("empty reason")
  {
    reason_skipped = "";
    expected_warning = "test-package-2.0-3.x86_64 was skipped by dnf5daemon.";
  }

  TransactionPreview preview;
  std::string error;
  REQUIRE(transaction_service_client_testonly_build_preview_from_item(
      "Skipped", "", "test-package", preview, error, reason_skipped));
  REQUIRE(error.empty());
  REQUIRE(preview.empty());
  REQUIRE(preview.disk_space_delta == 0);
  REQUIRE(preview.resolve_warnings == expected_warning);
}

// -----------------------------------------------------------------------------
// Verify that skipped warnings preserve daemon warnings and surrounding package actions.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview parser preserves mixed transactions with skipped packages")
{
  TransactionPreview preview;
  preview.resolve_warnings = "Existing daemon warning.";
  std::string error;

  REQUIRE(transaction_service_client_testonly_build_preview_from_item(
      "Package", "upgrade", "first-upgrade", preview, error));
  REQUIRE(transaction_service_client_testonly_build_preview_from_item(
      "Skipped", "", "conflicting-package", preview, error, "conflict"));
  REQUIRE(transaction_service_client_testonly_build_preview_from_item(
      "Skipped", "", "broken-package", preview, error, "broken_dependency"));
  REQUIRE(preview.disk_space_delta == 4096);
  REQUIRE(transaction_service_client_testonly_build_preview_from_item(
      "Package", "upgrade", "second-upgrade", preview, error));

  REQUIRE(error.empty());
  REQUIRE_FALSE(preview.empty());
  REQUIRE(preview.upgrade.size() == 2);
  REQUIRE(preview.upgrade[0].name == "first-upgrade");
  REQUIRE(preview.upgrade[1].name == "second-upgrade");
  REQUIRE(preview.install.empty());
  REQUIRE(preview.downgrade.empty());
  REQUIRE(preview.reinstall.empty());
  REQUIRE(preview.remove.empty());
  REQUIRE(preview.replaced.empty());
  REQUIRE(preview.disk_space_delta == 8192);
  REQUIRE(preview.resolve_warnings ==
          "Existing daemon warning.\n"
          "conflicting-package-2.0-3.x86_64 was skipped because of a package conflict.\n"
          "broken-package-2.0-3.x86_64 was skipped because of broken dependencies.");
}

// -----------------------------------------------------------------------------
// Verify that skipped items still require a valid package identity.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview parser rejects malformed skipped packages")
{
  TransactionPreview preview;
  std::string error;

  REQUIRE_FALSE(
      transaction_service_client_testonly_build_preview_from_item("Skipped", "", "", preview, error, "conflict"));
  REQUIRE(error.find("incomplete package item") != std::string::npos);
  REQUIRE(preview.empty());
  REQUIRE(preview.disk_space_delta == 0);
  REQUIRE(preview.resolve_warnings.empty());
}

// -----------------------------------------------------------------------------
// Verify that unsupported daemon item types fail the whole preview.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview parser rejects unsupported item types")
{
  TransactionPreview preview;
  std::string error;

  bool ok =
      transaction_service_client_testonly_build_preview_from_item("group", "install", "test-group", preview, error);

  REQUIRE_FALSE(ok);
  REQUIRE_FALSE(error.empty());
  REQUIRE(preview.empty());
}

// -----------------------------------------------------------------------------
// Verify that unsupported package actions fail the whole preview.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview parser rejects unsupported package actions")
{
  TransactionPreview preview;
  std::string error;

  bool ok = transaction_service_client_testonly_build_preview_from_item(
      "package", "future-action", "test-package", preview, error);

  REQUIRE_FALSE(ok);
  REQUIRE_FALSE(error.empty());
  REQUIRE(preview.empty());
}

// -----------------------------------------------------------------------------
// Verify that successful resolve warnings do not count as package actions.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview warnings do not count as package actions")
{
  TransactionPreview preview;
  preview.resolve_warnings = "Package test-package is already installed.";

  REQUIRE_FALSE(preview.resolve_warnings.empty());
  REQUIRE(preview.empty());
}

// -----------------------------------------------------------------------------
// Verify that cancellation wins before a trusted repository key answer reaches dnf5daemon.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon key import confirmation honors cancellation after user approval")
{
  GCancellable *cancellable = g_cancellable_new();
  bool callback_called = false;

  TransactionKeyImportCallback callback = [&](const TransactionKeyImportRequest &) {
    callback_called = true;
    g_cancellable_cancel(cancellable);
    return true;
  };

  bool confirmed = true;
  REQUIRE(transaction_service_client_testonly_key_import_answer_after_callback(callback, cancellable, confirmed));
  GCancellable *confirmation_cancellable =
      transaction_service_client_testonly_key_import_confirmation_cancellable(confirmed, cancellable);

  g_object_unref(cancellable);

  REQUIRE(callback_called);
  REQUIRE_FALSE(confirmed);
  REQUIRE(confirmation_cancellable == nullptr);
}

// -----------------------------------------------------------------------------
// Verify that the completed preview rejects removing the daemon server DNF UI needs for package changes.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview validation rejects removing dnf5daemon-server")
{
  TransactionPreview preview;
  std::string error;

  preview.remove.push_back(preview_package("dnf5daemon-server"));

  REQUIRE_FALSE(transaction_service_client_testonly_verify_preview_keeps_required_daemon_server(preview, error));
  REQUIRE(error.find("dnf5daemon-server") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that replacing dnf5daemon-server is allowed only when the preview also installs its successor.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview validation handles dnf5daemon-server replacement pairs")
{
  TransactionPreview preview;
  std::string error;

  preview.replaced.push_back(preview_package("dnf5daemon-server"));
  REQUIRE_FALSE(transaction_service_client_testonly_verify_preview_keeps_required_daemon_server(preview, error));
  REQUIRE(error.find("dnf5daemon-server") != std::string::npos);

  error.clear();
  preview.upgrade.push_back(preview_package("dnf5daemon-server"));
  REQUIRE(transaction_service_client_testonly_verify_preview_keeps_required_daemon_server(preview, error));
  REQUIRE(error.empty());
}

// -----------------------------------------------------------------------------
// Verify that self-protection blocks destructive preview actions, not normal upgrades.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview self-protection allows normal upgrades")
{
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", "dnf-ui");

  TransactionPreview preview;
  preview.upgrade.push_back(preview_package("dnf-ui"));
  std::string error;

  REQUIRE(transaction_service_client_testonly_verify_preview_keeps_running_app_package(preview, error));
  REQUIRE(error.empty());
}

// -----------------------------------------------------------------------------
// Verify that self-protection allows the replaced old package that belongs to a normal upgrade.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview self-protection allows normal upgrade replacement pairs")
{
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", "dnf-ui");

  TransactionPreview preview;
  preview.upgrade.push_back(preview_package("dnf-ui"));
  preview.replaced.push_back(preview_package("dnf-ui"));
  std::string error;

  REQUIRE(transaction_service_client_testonly_verify_preview_keeps_running_app_package(preview, error));
  REQUIRE(error.empty());
}

// -----------------------------------------------------------------------------
// Verify that self-protection rejects a replacement without a matching incoming upgrade.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview self-protection rejects replacements")
{
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", "dnf-ui");

  TransactionPreview preview;
  preview.replaced.push_back(preview_package("dnf-ui"));
  std::string error;

  REQUIRE_FALSE(transaction_service_client_testonly_verify_preview_keeps_running_app_package(preview, error));
  REQUIRE(error.find("DNF UI") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that self-protection rejects a resolved downgrade of the running app package.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview self-protection rejects downgrades")
{
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", "dnf-ui");

  TransactionPreview preview;
  preview.downgrade.push_back(preview_package("dnf-ui"));
  std::string error;

  REQUIRE_FALSE(transaction_service_client_testonly_verify_preview_keeps_running_app_package(preview, error));
  REQUIRE(error.find("DNF UI") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that self-protection rejects a resolved reinstall of the running app package.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon preview self-protection rejects reinstalls")
{
  ScopedEnvVar protected_name("DNFUI_TEST_SELF_PROTECTED_PACKAGE_NAME", "dnf-ui");

  TransactionPreview preview;
  preview.reinstall.push_back(preview_package("dnf-ui"));
  std::string error;

  REQUIRE_FALSE(transaction_service_client_testonly_verify_preview_keeps_running_app_package(preview, error));
  REQUIRE(error.find("DNF UI") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade targets keep internal identity and daemon specs separate.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon upgrade target parser exposes stable identity helpers")
{
  DaemonUpgradeTarget target;
  std::string error;

  REQUIRE(transaction_service_client_testonly_build_upgrade_target_from_fields("demo",
                                                                               "0",
                                                                               "1.2.3",
                                                                               "4.fc44",
                                                                               "x86_64",
                                                                               "updates",
                                                                               "demo-1.2.3-4.fc44.x86_64",
                                                                               "demo-0:1.2.3-4.fc44.x86_64",
                                                                               target,
                                                                               error));
  REQUIRE(error.empty());
  REQUIRE(target.name_arch_key() == "demo\nx86_64");
  REQUIRE(target.upgrade_spec() == "demo.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that normal NEVRA stays the application-facing package ID.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon upgrade target parser keeps normal and full NEVRA")
{
  DaemonUpgradeTarget target;
  std::string error;

  REQUIRE(transaction_service_client_testonly_build_upgrade_target_from_fields("demo",
                                                                               "0",
                                                                               "1.2.3",
                                                                               "4.fc44",
                                                                               "x86_64",
                                                                               "updates",
                                                                               "demo-1.2.3-4.fc44.x86_64",
                                                                               "demo-0:1.2.3-4.fc44.x86_64",
                                                                               target,
                                                                               error));
  REQUIRE(target.nevra == "demo-1.2.3-4.fc44.x86_64");
  REQUIRE(target.full_nevra == "demo-0:1.2.3-4.fc44.x86_64");
  REQUIRE(target.repo_id == "updates");
}

// -----------------------------------------------------------------------------
// Verify that daemon summaries are retained for the upgrade table.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon upgrade target parser keeps summary")
{
  GVariantBuilder fields;
  g_variant_builder_init(&fields, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&fields, "{sv}", "name", g_variant_new_string("demo"));
  g_variant_builder_add(&fields, "{sv}", "version", g_variant_new_string("1.2.3"));
  g_variant_builder_add(&fields, "{sv}", "release", g_variant_new_string("4.fc44"));
  g_variant_builder_add(&fields, "{sv}", "arch", g_variant_new_string("x86_64"));
  g_variant_builder_add(&fields, "{sv}", "summary", g_variant_new_string("Package summary"));
  GVariant *object = g_variant_ref_sink(g_variant_builder_end(&fields));

  DaemonUpgradeTarget target;
  std::string error;
  bool parsed = transaction_service_client_upgrade_target_from_daemon_object(object, target, error);
  g_variant_unref(object);

  REQUIRE(parsed);
  REQUIRE(error.empty());
  REQUIRE(target.summary == "Package summary");
}

// -----------------------------------------------------------------------------
// Verify that full NEVRA alone is still kept while normal NEVRA is reconstructed.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon upgrade target parser builds normal NEVRA when missing")
{
  DaemonUpgradeTarget target;
  std::string error;

  REQUIRE(transaction_service_client_testonly_build_upgrade_target_from_fields(
      "demo", "0", "1.2.3", "4.fc44", "x86_64", "updates", "", "demo-0:1.2.3-4.fc44.x86_64", target, error));
  REQUIRE(target.nevra == "demo-1.2.3-4.fc44.x86_64");
  REQUIRE(target.full_nevra == "demo-0:1.2.3-4.fc44.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that a missing full NEVRA still keeps the epoch-zero full form.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon upgrade target parser builds epoch-zero full NEVRA")
{
  DaemonUpgradeTarget target;
  std::string error;

  REQUIRE(transaction_service_client_testonly_build_upgrade_target_from_fields(
      "demo", "0", "1.2.3", "4.fc44", "x86_64", "updates", "", "", target, error));
  REQUIRE(target.nevra == "demo-1.2.3-4.fc44.x86_64");
  REQUIRE(target.full_nevra == "demo-0:1.2.3-4.fc44.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that nonzero epochs are kept in the reconstructed NEVRA.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon upgrade target parser builds nonzero epoch NEVRA")
{
  DaemonUpgradeTarget target;
  std::string error;

  REQUIRE(transaction_service_client_testonly_build_upgrade_target_from_fields(
      "demo", "2", "1.2.3", "4.fc44", "x86_64", "updates", "", "", target, error));
  REQUIRE(target.nevra == "demo-2:1.2.3-4.fc44.x86_64");
  REQUIRE(target.full_nevra == target.nevra);
}

// -----------------------------------------------------------------------------
// Verify that incomplete daemon upgrade targets fail instead of entering the snapshot.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon upgrade target parser rejects missing required fields")
{
  DaemonUpgradeTarget target;
  std::string error;

  REQUIRE_FALSE(transaction_service_client_testonly_build_upgrade_target_from_fields(
      "", "0", "1.2.3", "4.fc44", "x86_64", "updates", "", "", target, error));
  REQUIRE(error.find("incomplete upgrade item") != std::string::npos);

  REQUIRE_FALSE(transaction_service_client_testonly_build_upgrade_target_from_fields(
      "demo", "0", "", "4.fc44", "x86_64", "updates", "", "", target, error));
  REQUIRE(error.find("incomplete upgrade item") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that the client can ask dnf5daemon for an install preview.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client previews install requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest request;
  request.install.push_back(install_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());

  bool preview_contains_package = preview_section_contains_name(preview.install, install_spec);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
}

// -----------------------------------------------------------------------------
// Verify that the client can ask dnf5daemon for an upgrade-all preview.
// A fully updated test container may return an empty preview, which is still a successful preview result.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client previews upgrade-all requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_upgrade_all_request(preview, transaction_path, error));

  if (!transaction_path.empty()) {
    transaction_service_client_release_request(transaction_path);
  }
  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that the client can preview an explicitly selected upgrade.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client previews selected upgrade requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const char *upgrade_spec = g_getenv("DNFUI_TEST_DNF5DAEMON_UPGRADE_SPEC");
  const char *expected_nevra = g_getenv("DNFUI_TEST_DNF5DAEMON_UPGRADE_EXPECTED_NEVRA");
  if (!upgrade_spec || !*upgrade_spec || !expected_nevra || !*expected_nevra) {
    SKIP("Set the upgrade spec and expected NEVRA to run the selected upgrade preview test.");
  }

  TransactionRequest request;
  request.upgrade.push_back(upgrade_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  bool previewed = transaction_service_client_preview_request(request, preview, transaction_path, error);
  INFO(error);
  REQUIRE(previewed);
  REQUIRE_FALSE(transaction_path.empty());

  bool preview_contains_package = preview_section_contains_label(preview.upgrade, expected_nevra);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
}

// -----------------------------------------------------------------------------
// Verify that the client can ask dnf5daemon for a downgrade preview.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client previews downgrade requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string downgrade_spec = dnf5daemon_test_downgrade_spec();
  if (downgrade_spec.empty()) {
    SKIP("Set DNFUI_TEST_DNF5DAEMON_DOWNGRADE_SPEC to run the daemon downgrade preview test.");
  }

  TransactionRequest request;
  request.downgrade.push_back(downgrade_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());

  bool preview_contains_package = preview_section_contains_name(preview.downgrade, downgrade_spec) ||
      preview_section_contains_label(preview.downgrade, downgrade_spec);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
}

// -----------------------------------------------------------------------------
// Verify that the client can list daemon upgrade targets without resolving an Upgrade All preview.
// A fully updated test system may return an empty list.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client lists upgrade targets", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  std::vector<DaemonUpgradeTarget> targets;
  std::string error;

  bool listed = transaction_service_client_list_upgrade_targets(targets, error);

  transaction_service_client_reset_for_tests();

  INFO(error);
  REQUIRE(listed);
  for (const auto &target : targets) {
    REQUIRE_FALSE(target.name.empty());
    REQUIRE_FALSE(target.arch.empty());
    REQUIRE_FALSE(target.version.empty());
    REQUIRE_FALSE(target.release.empty());
    REQUIRE_FALSE(target.nevra.empty());
    REQUIRE(target.name_arch_key() == target.name + "\n" + target.arch);
    REQUIRE(target.upgrade_spec() == target.name + "." + target.arch);
  }
}

// -----------------------------------------------------------------------------
// Verify that the TUI retains the daemon target and its snapshot generation.
// The Docker fixture provides a known upgrade from a local repository.
// -----------------------------------------------------------------------------
TEST_CASE("TUI lists current daemon upgrade targets after refresh", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  const char *expected_nevra = g_getenv("DNFUI_TEST_DNF5DAEMON_UPGRADE_EXPECTED_NEVRA");
  if (!expected_nevra || !*expected_nevra) {
    SKIP("Set the expected NEVRA to run the TUI upgrade target test.");
  }
  transaction_service_client_reset_for_tests();
  auto &state = DaemonUpgradeState::instance();
  state.reset_for_tests();

  const auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
  const auto generation_before = BaseManager::instance().current_generation();
  const auto result = tui::query_upgrades(true, nullptr, cancel_flag);
  const auto snapshot = state.snapshot();

  REQUIRE(BaseManager::instance().current_generation() > generation_before);
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(result.rows.size() == snapshot.targets_by_name_arch.size());
  std::vector<tui::PendingMark> marks;
  for (const auto &row : result.rows) {
    REQUIRE(row.upgrade != nullptr);
    REQUIRE_FALSE(row.installed);
    REQUIRE(state.is_current_target(row.upgrade->target, row.upgrade->generation));
    REQUIRE(row.package.nevra == row.upgrade->target.nevra);
    if (row.package.nevra == expected_nevra) {
      REQUIRE(tui::mark_pending_action(marks, row, PendingAction::UPGRADE));
    }
  }
  REQUIRE(marks.size() == 1);
  const auto issues = tui::validate_pending_marks(marks, nullptr, cancel_flag);
  REQUIRE(issues.size() == marks.size());
  INFO(issues.front());
  REQUIRE(issues.front().empty());
  REQUIRE(state.snapshot().generation > snapshot.generation);
  REQUIRE(marks.front().source.upgrade->generation == snapshot.generation);
  REQUIRE(marks.front().action.nevra == expected_nevra);

  const auto installed_before = package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr));
  auto preview = tui::preview_pending_marks(marks, nullptr, cancel_flag);
  INFO(preview.error);
  REQUIRE(preview.error.empty());
  REQUIRE(preview_section_contains_label(preview.preview.upgrade, expected_nevra));
  REQUIRE(preview.preview_issues.size() == marks.size());
  REQUIRE(preview.preview_issues.front().empty());
  REQUIRE(preview.prepared != nullptr);
  REQUIRE_FALSE(preview.session_path_for_tests.empty());
  REQUIRE(transaction_service_client_session_exists_for_tests(preview.session_path_for_tests));
  preview.prepared.reset();
  REQUIRE_FALSE(transaction_service_client_session_exists_for_tests(preview.session_path_for_tests));

  // Resolve the real upgrade but compare it with a deliberately different marked target.
  TransactionRequest request;
  request.upgrade.push_back(marks.front().action.transaction_spec);
  auto mismatched_marks = marks;
  mismatched_marks.front().action.nevra = "dnfui-nonexistent-version-0-1.noarch";
  const auto rejected = tui::resolve_preview_for_tests(request, mismatched_marks);
  REQUIRE_FALSE(rejected.error.empty());
  REQUIRE(rejected.preview.empty());
  REQUIRE(rejected.prepared == nullptr);
  REQUIRE(rejected.preview_issues.size() == 1);
  REQUIRE_FALSE(rejected.preview_issues.front().empty());
  REQUIRE_FALSE(rejected.session_path_for_tests.empty());
  REQUIRE_FALSE(transaction_service_client_session_exists_for_tests(rejected.session_path_for_tests));

  BaseManager::instance().drop_cached_base();
  REQUIRE(package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr)) == installed_before);

  state.reset_for_tests();
  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Apply the exact session prepared by the terminal frontend. Also exercise a
// failed submission after the session was closed, without resolving it again.
// -----------------------------------------------------------------------------
TEST_CASE("TUI applies the approved upgrade session", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  const char *expected_nevra = g_getenv("DNFUI_TEST_DNF5DAEMON_UPGRADE_EXPECTED_NEVRA");
  if (!expected_nevra || !*expected_nevra) {
    SKIP("Set the expected NEVRA to run the TUI apply test.");
  }

  transaction_service_client_reset_for_tests();
  auto &state = DaemonUpgradeState::instance();
  state.reset_for_tests();
  const auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
  const auto rows = tui::query_upgrades(false, nullptr, cancel_flag);
  std::vector<tui::PendingMark> marks;
  for (const auto &row : rows.rows) {
    if (row.package.nevra == expected_nevra) {
      REQUIRE(tui::mark_pending_action(marks, row, PendingAction::UPGRADE));
    }
  }
  REQUIRE(marks.size() == 1);

  auto preview = tui::preview_pending_marks(marks, nullptr, cancel_flag);
  INFO(preview.error);
  REQUIRE(preview.prepared != nullptr);
  const auto installed_before = package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr));
  const auto path = preview.prepared->path();
  transaction_service_client_release_request(path);
  tui::TransactionProgress progress;
  const auto failed = tui::apply_transaction(std::move(preview.prepared), progress);
  REQUIRE_FALSE(failed.success);
  REQUIRE_FALSE(failed.message.empty());
  REQUIRE_FALSE(transaction_service_client_session_exists_for_tests(path));
  REQUIRE(package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr)) == installed_before);

  preview = tui::preview_pending_marks(marks, nullptr, cancel_flag);
  INFO(preview.error);
  REQUIRE(preview.prepared != nullptr);
  REQUIRE_FALSE(preview.prepared->requires_offline());
  const auto approved_path = preview.prepared->path();
  REQUIRE(transaction_service_client_session_exists_for_tests(approved_path));
  const auto applied = tui::apply_transaction(std::move(preview.prepared), progress);
  INFO(applied.message);
  REQUIRE(applied.success);
  REQUIRE_FALSE(applied.offline);
  REQUIRE_FALSE(transaction_service_client_session_exists_for_tests(approved_path));
  const auto installed = package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr));
  REQUIRE(std::find(installed.begin(), installed.end(), expected_nevra) != installed.end());

  state.reset_for_tests();
  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// A cancelled request must not publish a failed or successful daemon snapshot.
// -----------------------------------------------------------------------------
TEST_CASE("TUI skips cancelled daemon upgrade queries")
{
  auto &state = DaemonUpgradeState::instance();
  state.reset_for_tests();

  std::unique_ptr<GCancellable, decltype(&g_object_unref)> cancel { g_cancellable_new(), g_object_unref };
  g_cancellable_cancel(cancel.get());
  const auto cancel_flag = std::make_shared<std::atomic<bool>>(true);
  const auto result = tui::query_upgrades(true, cancel.get(), cancel_flag);

  REQUIRE(result.rows.empty());
  REQUIRE(state.snapshot().status == DaemonUpgradeSnapshotStatus::NOT_LOADED);
}

// -----------------------------------------------------------------------------
// Verify that cancelling a passive status check does not wait for the daemon reply.
// -----------------------------------------------------------------------------
TEST_CASE("TUI cancels a delayed offline status session", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  std::unique_ptr<GTestDBus, decltype(&g_object_unref)> bus(g_test_dbus_new(G_TEST_DBUS_NONE), g_object_unref);
  g_test_dbus_up(bus.get());
  ScopedEnvVar system_bus("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus.get()));

  GError *error = nullptr;
  std::unique_ptr<GDBusConnection, decltype(&g_object_unref)> service(
      g_dbus_connection_new_for_address_sync(
          g_test_dbus_get_bus_address(bus.get()),
          static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
          nullptr,
          nullptr,
          &error),
      g_object_unref);
  INFO(error ? error->message : "");
  g_clear_error(&error);
  REQUIRE(service != nullptr);

  std::unique_ptr<GVariant, decltype(&g_variant_unref)> name_reply(
      g_dbus_connection_call_sync(service.get(),
                                  "org.freedesktop.DBus",
                                  "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus",
                                  "RequestName",
                                  g_variant_new("(su)", "org.rpm.dnf.v0", 0U),
                                  G_VARIANT_TYPE("(u)"),
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1,
                                  nullptr,
                                  &error),
      g_variant_unref);
  INFO(error ? error->message : "");
  g_clear_error(&error);
  REQUIRE(name_reply != nullptr);
  guint32 name_result = 0;
  g_variant_get(name_reply.get(), "(u)", &name_result);
  REQUIRE(name_result == 1U);

  // Drop the open_session call without replying so the client must cancel it.
  auto received = std::make_unique<std::promise<void>>();
  auto request = received->get_future();
  const guint filter = g_dbus_connection_add_filter(
      service.get(),
      [](GDBusConnection *, GDBusMessage *message, gboolean incoming, gpointer data) -> GDBusMessage * {
        if (incoming && g_dbus_message_get_message_type(message) == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
            g_strcmp0(g_dbus_message_get_interface(message), "org.rpm.dnf.v0.SessionManager") == 0 &&
            g_strcmp0(g_dbus_message_get_member(message), "open_session") == 0) {
          static_cast<std::promise<void> *>(data)->set_value();
          g_object_unref(message);
          return nullptr;
        }
        return message;
      },
      received.release(),
      [](gpointer data) { delete static_cast<std::promise<void> *>(data); });

  std::unique_ptr<GCancellable, decltype(&g_object_unref)> cancel(g_cancellable_new(), g_object_unref);
  auto status = std::async(std::launch::async, [token = cancel.get()]() { return tui::query_offline_state(token); });
  const auto request_state = request.wait_for(std::chrono::seconds(5));
  g_cancellable_cancel(cancel.get());
  const auto cancel_state = status.wait_for(std::chrono::seconds(2));
  const auto result = status.get();

  g_dbus_connection_remove_filter(service.get(), filter);
  g_dbus_connection_close_sync(service.get(), nullptr, nullptr);
  REQUIRE(request_state == std::future_status::ready);
  REQUIRE(cancel_state == std::future_status::ready);
  REQUIRE(result == tui::OfflineState::UNKNOWN);
}

// -----------------------------------------------------------------------------
// Verify that an unavailable daemon is an error, not an empty upgrade list.
// -----------------------------------------------------------------------------
TEST_CASE("TUI reports failed daemon upgrade queries", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();
  auto &state = DaemonUpgradeState::instance();
  state.reset_for_tests();

  ScopedEnvVar missing_system_bus("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/tmp/dnfui-missing-system-bus");
  const auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
  REQUIRE_THROWS_AS(tui::query_upgrades(false, nullptr, cancel_flag), std::runtime_error);
  REQUIRE(state.snapshot().status == DaemonUpgradeSnapshotStatus::ERROR);
  REQUIRE(state.snapshot().targets_by_name_arch.empty());

  state.reset_for_tests();
  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that the client can ask dnf5daemon to refresh repository metadata.
// This is the same daemon path used by the Refresh Repositories button.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client refreshes repositories", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  std::string error;

  bool refreshed = transaction_service_client_refresh_repositories(error);

  transaction_service_client_reset_for_tests();

  INFO(error);
  REQUIRE(refreshed);
}

// -----------------------------------------------------------------------------
// Verify that releasing a preview session really closes it in dnf5daemon.
// This is the same path used when the user closes the preview dialog.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client releases preview sessions", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest request;
  request.install.push_back(install_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());

  transaction_service_client_release_request(transaction_path);

  std::vector<std::string> progress_lines;
  bool transaction_started = false;
  bool applied_after_release = transaction_service_client_apply_started_request(
      transaction_path,
      [&](const std::string &line) { progress_lines.push_back(line); },
      {},
      {},
      error,
      transaction_started);

  transaction_service_client_reset_for_tests();

  REQUIRE_FALSE(applied_after_release);
  REQUIRE_FALSE(transaction_started);
  REQUIRE_FALSE(error.empty());
}

// -----------------------------------------------------------------------------
// Verify that the client can apply an install transaction through dnf5daemon.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client applies install requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest request;
  request.install.push_back(install_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());
  REQUIRE(transaction_service_client_session_exists_for_tests(transaction_path));

  bool preview_contains_package = preview_section_contains_name(preview.install, install_spec);

  std::vector<std::string> progress_lines;
  std::vector<TransactionApplyProgress> progress_updates;
  bool transaction_started = false;
  bool applied = transaction_service_client_apply_started_request(
      transaction_path,
      [&](const std::string &line) { progress_lines.push_back(line); },
      [&](const TransactionApplyProgress &progress) { progress_updates.push_back(progress); },
      {},
      error,
      transaction_started);

  transaction_service_client_release_request(transaction_path);
  bool session_exists_after_release = transaction_service_client_session_exists_for_tests(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
  INFO(error);
  INFO(joined_progress_lines(progress_lines));
  REQUIRE(applied);
  REQUIRE(transaction_started);
  REQUIRE(progress_contains(progress_lines, "Transaction applied successfully."));
  bool has_determinate_progress = false;
  for (const auto &progress : progress_updates) {
    if (!progress.determinate) {
      continue;
    }
    REQUIRE(progress.total > 0);
    REQUIRE(progress.processed <= progress.total);
    has_determinate_progress = true;
  }
  REQUIRE(has_determinate_progress);
  REQUIRE_FALSE(session_exists_after_release);
}

// -----------------------------------------------------------------------------
// Verify that release still closes the daemon session after apply fails.
// A second daemon session changes the package state after the first preview.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client releases sessions after failed apply", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest remove_request;
  remove_request.remove.push_back(install_spec);

  TransactionPreview first_preview;
  std::string first_transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(remove_request, first_preview, first_transaction_path, error));
  REQUIRE_FALSE(first_transaction_path.empty());
  REQUIRE(transaction_service_client_session_exists_for_tests(first_transaction_path));

  TransactionPreview second_preview;
  std::string second_transaction_path;
  REQUIRE(transaction_service_client_preview_request(remove_request, second_preview, second_transaction_path, error));
  REQUIRE_FALSE(second_transaction_path.empty());

  std::vector<std::string> progress_lines;
  bool second_transaction_started = false;
  REQUIRE(transaction_service_client_apply_started_request(
      second_transaction_path,
      [&](const std::string &line) { progress_lines.push_back(line); },
      {},
      {},
      error,
      second_transaction_started));
  transaction_service_client_release_request(second_transaction_path);

  bool first_transaction_started = false;
  bool failed_apply = transaction_service_client_apply_started_request(
      first_transaction_path,
      [&](const std::string &line) { progress_lines.push_back(line); },
      {},
      {},
      error,
      first_transaction_started);
  transaction_service_client_release_request(first_transaction_path);
  bool session_exists_after_release = transaction_service_client_session_exists_for_tests(first_transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE_FALSE(failed_apply);
  REQUIRE(second_transaction_started);
  INFO(error);
  INFO(joined_progress_lines(progress_lines));
  REQUIRE_FALSE(error.empty());
  REQUIRE(error.find("The approved transaction did not complete.") != std::string::npos);
  REQUIRE_FALSE(session_exists_after_release);
}

// -----------------------------------------------------------------------------
// Verify that the client can preview removing an installed package.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client previews remove requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest request;
  request.remove.push_back(install_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());

  bool preview_contains_package = preview_section_contains_name(preview.remove, install_spec);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
}

// -----------------------------------------------------------------------------
// Verify that the client can preview reinstalling an installed package.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client previews reinstall requests", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  const std::string install_spec = dnf5daemon_test_install_spec();
  TransactionRequest request;
  request.reinstall.push_back(install_spec);

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE_FALSE(transaction_path.empty());

  bool preview_contains_package = preview_section_contains_name(preview.reinstall, install_spec);

  transaction_service_client_release_request(transaction_path);
  transaction_service_client_reset_for_tests();

  REQUIRE(preview_contains_package);
}

// -----------------------------------------------------------------------------
// Verify that DNF UI refuses a transaction that would remove its transaction backend.
// Removing dnf5daemon-server would leave the app unable to apply later package changes.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client rejects removing dnf5daemon-server", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  TransactionRequest request;
  request.remove.push_back("dnf5daemon-server");

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE_FALSE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE(transaction_path.empty());
  REQUIRE_FALSE(error.empty());
  REQUIRE(error.find("dnf5daemon-server") != std::string::npos);
  REQUIRE(preview.empty());

  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that daemon resolver failures are returned as normal client errors.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client reports resolve failure", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  TransactionRequest request;
  request.install.push_back("dnf-ui-definitely-not-a-real-package-name");

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE_FALSE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE(transaction_path.empty());
  REQUIRE_FALSE(error.empty());
  REQUIRE(preview.empty());

  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that connection failure is reported before any daemon session is opened.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client reports unavailable daemon", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  transaction_service_client_reset_for_tests();

  ScopedEnvVar missing_system_bus("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/tmp/dnfui-missing-system-bus");

  TransactionRequest request;
  request.install.push_back(dnf5daemon_test_install_spec());

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;

  REQUIRE_FALSE(transaction_service_client_preview_request(request, preview, transaction_path, error));
  REQUIRE(transaction_path.empty());
  REQUIRE_FALSE(error.empty());
  REQUIRE(preview.empty());

  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that the terminal frontend preserves the offline mode of its preview.
// -----------------------------------------------------------------------------
TEST_CASE("TUI prepares approved daemon changes for reboot", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  const char *enabled = g_getenv("DNFUI_TEST_DNF5DAEMON_OFFLINE");
  if (!enabled || std::string(enabled) != "1") {
    SKIP("Set DNFUI_TEST_DNF5DAEMON_OFFLINE=1 only in a disposable test system.");
  }

  transaction_service_client_reset_for_tests();
  BaseManager::instance().drop_cached_base();
  const auto installed = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  const auto installed_before = package_row_nevras(installed);
  std::vector<tui::PendingMark> marks;
  for (const auto &row : installed) {
    if (row.name == "dnf5daemon-server") {
      REQUIRE(tui::mark_pending_action(marks, { row, true }, PendingAction::REINSTALL));
    }
  }
  REQUIRE(marks.size() == 1);

  const auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
  auto preview = tui::preview_pending_marks(marks, nullptr, cancel_flag);
  INFO(preview.error);
  REQUIRE(preview.prepared != nullptr);
  REQUIRE(preview.preview.requires_offline);
  REQUIRE(preview.prepared->requires_offline());
  const auto path = preview.prepared->path();

  tui::TransactionProgress progress;
  const auto applied = tui::apply_transaction(std::move(preview.prepared), progress);
  INFO(applied.message);
  REQUIRE(applied.success);
  REQUIRE(applied.offline);
  REQUIRE(applied.offline_state == tui::OfflineState::PREPARED);
  REQUIRE_FALSE(transaction_service_client_session_exists_for_tests(path));
  REQUIRE(package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr)) == installed_before);

  // A new connection must detect an already prepared transaction.
  transaction_service_client_reset_for_tests();
  REQUIRE(tui::query_offline_state(nullptr) == tui::OfflineState::PREPARED);

  std::string error;
  bool prepared = false;
  REQUIRE(transaction_service_client_has_prepared_offline_updates(prepared, error));
  REQUIRE(prepared);
  REQUIRE(transaction_service_client_discard_offline_transaction(error));
  REQUIRE(transaction_service_client_has_prepared_offline_updates(prepared, error));
  REQUIRE_FALSE(prepared);
  REQUIRE(tui::query_offline_state(nullptr) == tui::OfflineState::NOT_PREPARED);
  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Exercise offline preparation only in an explicitly disposable system.
// The daemon itself is included, but no package scriptlets should execute here.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client prepares daemon changes for reboot", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  const char *enabled = g_getenv("DNFUI_TEST_DNF5DAEMON_OFFLINE");
  if (!enabled || std::string(enabled) != "1") {
    SKIP("Set DNFUI_TEST_DNF5DAEMON_OFFLINE=1 only in a disposable test system.");
  }
  transaction_service_client_reset_for_tests();
  std::string error;
  const auto installed_before = package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr));

  TransactionRequest request;
  request.reinstall.push_back("dnf5daemon-server");
  request.install.push_back(dnf5daemon_test_install_spec());
  TransactionPreview preview;
  std::string path;
  bool previewed = transaction_service_client_preview_request(request, preview, path, error);
  INFO(error);
  REQUIRE(previewed);
  REQUIRE(preview.requires_offline);
  REQUIRE_FALSE(preview.install.empty());
  bool started = false;
  REQUIRE_FALSE(transaction_service_client_apply_started_request(path, {}, {}, {}, error, started));
  REQUIRE_FALSE(started);

  std::vector<std::string> progress;
  bool prepared = transaction_service_client_apply_started_request(
      path, [&](const std::string &line) { progress.push_back(line); }, {}, {}, error, started, nullptr, true);
  INFO(error);
  REQUIRE(prepared);
  REQUIRE_FALSE(started);
  REQUIRE(progress_contains(progress, "prepared for the next reboot"));
  REQUIRE_FALSE(progress_contains(progress, "applied successfully"));
  transaction_service_client_release_request(path);
  transaction_service_client_reset_for_tests();

  BaseManager::instance().drop_cached_base();
  REQUIRE(package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr)) == installed_before);

  REQUIRE_FALSE(transaction_service_client_preview_request(request, preview, path, error));
  REQUIRE(path.empty());
  REQUIRE(error.find("already prepared for reboot") != std::string::npos);
  REQUIRE(error.find("Discard Prepared Updates") != std::string::npos);
  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify the user-facing discard operation in the disposable container after preparation.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client discards prepared updates", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  const char *enabled = g_getenv("DNFUI_TEST_DNF5DAEMON_OFFLINE");
  if (!enabled || std::string(enabled) != "1") {
    SKIP("Set DNFUI_TEST_DNF5DAEMON_OFFLINE=1 only in a disposable test system.");
  }
  transaction_service_client_reset_for_tests();
  const auto installed_before = package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr));
  TransactionRequest request;
  request.install.push_back(dnf5daemon_test_install_spec());
  TransactionPreview preview;
  std::string path;
  std::string error;
  REQUIRE_FALSE(transaction_service_client_preview_request(request, preview, path, error));
  REQUIRE(error.find("already prepared for reboot") != std::string::npos);
  REQUIRE(error.find("Discard Prepared Updates") != std::string::npos);

  bool prepared = false;
  REQUIRE(transaction_service_client_has_prepared_offline_updates(prepared, error));
  REQUIRE(prepared);

  bool discarded = transaction_service_client_discard_offline_transaction(error);
  INFO(error);
  REQUIRE(discarded);
  REQUIRE(transaction_service_client_has_prepared_offline_updates(prepared, error));
  REQUIRE_FALSE(prepared);
  BaseManager::instance().drop_cached_base();
  REQUIRE(package_row_nevras(dnf_backend_get_installed_package_rows_interruptible(nullptr)) == installed_before);
  REQUIRE(transaction_service_client_preview_request(request, preview, path, error));
  REQUIRE_FALSE(preview.requires_offline);
  transaction_service_client_release_request(path);
  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// Verify that unreadable offline state still reaches the daemon cleanup method.
// -----------------------------------------------------------------------------
TEST_CASE("dnf5daemon client discards unreadable offline updates", "[dnf5daemon]")
{
  require_dnf5daemon_test_enabled();
  const char *enabled = g_getenv("DNFUI_TEST_DNF5DAEMON_OFFLINE");
  if (!enabled || std::string(enabled) != "1") {
    SKIP("Set DNFUI_TEST_DNF5DAEMON_OFFLINE=1 only in a disposable test system.");
  }
  transaction_service_client_reset_for_tests();
  const auto &data_dir = libdnf5::offline::DEFAULT_DATADIR;
  std::filesystem::create_directories(data_dir);
  REQUIRE(std::filesystem::is_empty(data_dir));

  // NOTE: DNF 5.2 stores offline packages under the data directory.
  const auto package_dir = data_dir / "packages";
  std::filesystem::create_directories(package_dir);
  const auto state_path = data_dir / libdnf5::offline::TRANSACTION_STATE_FILENAME;
  const auto package_path = package_dir / "dnfui-cleanup-test.rpm";
  {
    std::ofstream state_file(state_path);
    state_file << "invalid state data\n";
    REQUIRE(state_file.good());
    std::ofstream package_file(package_path);
    package_file << "placeholder package data\n";
    REQUIRE(package_file.good());
  }

  std::string error;
  bool discarded = transaction_service_client_discard_offline_transaction(error);
  INFO(error);
  REQUIRE(discarded);
  REQUIRE(std::filesystem::is_empty(data_dir));
  transaction_service_client_reset_for_tests();
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
