// -----------------------------------------------------------------------------
// test_package_details.cpp
// Package information, dependencies, and file-list tests
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Find a real available update candidate from installed-row repository annotation.
// -----------------------------------------------------------------------------
bool
find_update_pair_from_installed_annotation(PackageRow &installed_out, PackageRow &update_out)
{
  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  for (const auto &installed_row : installed_rows) {
    if (installed_row.repo_candidate_relation != PackageRepoCandidateRelation::NEWER ||
        installed_row.repo_candidate_nevra.empty()) {
      continue;
    }

    auto candidates = dnf_backend_get_available_package_rows_by_nevra(installed_row.repo_candidate_nevra);
    if (candidates.empty()) {
      continue;
    }

    update_out = candidates.front();
    InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(update_out);
    if (resolution.exact_installed || !resolution.has_installed_row ||
        resolution.state != PackageInstallState::UPGRADEABLE) {
      continue;
    }
    installed_out = resolution.installed_row;
    return true;
  }

  return false;
}

int
compare_backend_test_row_evr(const PackageRow &left, const PackageRow &right)
{
  int result = dnf_backend_compare_epoch_version_text(left.epoch, left.version, right.epoch, right.version);
  if (result != 0) {
    return result;
  }

  return dnf_backend_compare_rpm_version_text(left.release, right.release);
}

bool
find_parallel_installed_versions(PackageRow &older_out, PackageRow &newer_out)
{
  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);

  for (size_t i = 0; i < installed_rows.size(); ++i) {
    for (size_t j = i + 1; j < installed_rows.size(); ++j) {
      if (installed_rows[i].name_arch_key() != installed_rows[j].name_arch_key() ||
          installed_rows[i].nevra == installed_rows[j].nevra) {
        continue;
      }

      if (compare_backend_test_row_evr(installed_rows[i], installed_rows[j]) <= 0) {
        older_out = installed_rows[i];
        newer_out = installed_rows[j];
      } else {
        older_out = installed_rows[j];
        newer_out = installed_rows[i];
      }
      return true;
    }
  }

  return false;
}

} // namespace

// -----------------------------------------------------------------------------
// Package info tests (read-only)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that invalid package identifiers return a friendly details message.
// -----------------------------------------------------------------------------
TEST_CASE("Invalid NEVRA returns friendly message")
{
  auto info = dnf_backend_get_package_info("invalid-0-0.x86_64");

  REQUIRE(info.find("No details found") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that package details include the expected user-visible fields.
// -----------------------------------------------------------------------------
TEST_CASE("Package info formatting contains expected fields")
{
  reset_backend_globals();

  auto results = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE(!results.empty());

  auto info = dnf_backend_get_package_info(results.front().nevra);

  REQUIRE(info.find("Name: ") != std::string::npos);
  REQUIRE(info.find("Package ID: ") != std::string::npos);
  REQUIRE(info.find("Version: ") != std::string::npos);
  REQUIRE(info.find("Release: ") != std::string::npos);
  REQUIRE(info.find("Arch: ") != std::string::npos);
  REQUIRE(info.find("Installed From: ") != std::string::npos);
  REQUIRE(info.find("Install Size: ") != std::string::npos);
  REQUIRE(info.find("Install Reason: ") != std::string::npos);
  REQUIRE(info.find("Summary:") != std::string::npos);
  REQUIRE(info.find("Description:") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that package details can display an explicit daemon update target.
// -----------------------------------------------------------------------------
TEST_CASE("Package info formatting can use explicit upgrade details")
{
  reset_backend_globals();

  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE(!installed_rows.empty());

  PackageRow installed_row = installed_rows.front();
  PackageRow daemon_target = installed_row;
  daemon_target.version = "999.0";
  daemon_target.release = "1.test";
  daemon_target.nevra = daemon_target.name + "-999.0-1.test." + daemon_target.arch;
  daemon_target.repo = "daemon-test";

  auto info = dnf_backend_get_package_info(installed_row.nevra, &daemon_target);

  REQUIRE(info.find("Package ID: " + installed_row.nevra) != std::string::npos);
  REQUIRE(info.find("Installed Version: " + installed_row.display_version()) != std::string::npos);
  REQUIRE(info.find("Upgradable Version: " + daemon_target.display_version()) != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that selected-version details describe the exact selected NEVRA.
// -----------------------------------------------------------------------------
TEST_CASE("Package info selected-version mode uses selected package metadata")
{
  reset_backend_globals();

  PackageRow installed_row;
  PackageRow update_row;
  if (!find_update_pair_from_installed_annotation(installed_row, update_row)) {
    SUCCEED("No installed package with a newer repo candidate in the test environment.");
    return;
  }

  auto info = dnf_backend_get_package_info(update_row.nevra, PackageDetailsContext::SELECTED_VERSION);

  REQUIRE(info.find("Package ID: " + update_row.nevra) != std::string::npos);
  REQUIRE(info.find("Package ID: " + installed_row.nevra) == std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that installed context keeps exact installed package metadata.
// -----------------------------------------------------------------------------
TEST_CASE("Package info installed context keeps exact installed version")
{
  reset_backend_globals();

  PackageRow older_row;
  PackageRow newer_row;
  if (!find_parallel_installed_versions(older_row, newer_row)) {
    SUCCEED("No parallel installed package versions in the test environment.");
    return;
  }

  auto info = dnf_backend_get_package_info(older_row.nevra);

  REQUIRE(info.find("Package ID: " + older_row.nevra) != std::string::npos);
  REQUIRE(info.find("Package ID: " + newer_row.nevra) == std::string::npos);
  REQUIRE(info.find("Upgradable Version: ") == std::string::npos);
}

// -----------------------------------------------------------------------------
// Dependency and file list tests (read-only)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Verify that dependency details keep all expected section headings.
// -----------------------------------------------------------------------------
TEST_CASE("Dependency info contains expected section headers")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false), nullptr);
  REQUIRE(!results.empty());

  auto deps = dnf_backend_get_package_deps(results.front().nevra);

  REQUIRE(deps.find("Requires:") != std::string::npos);
  REQUIRE(deps.find("Required By:") != std::string::npos);
  REQUIRE(deps.find("Provides:") != std::string::npos);
  REQUIRE(deps.find("Conflicts:") != std::string::npos);
  REQUIRE(deps.find("Obsoletes:") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that dependency details for an update row describe the currently installed package.
// -----------------------------------------------------------------------------
TEST_CASE("Dependency info uses installed package for update rows")
{
  reset_backend_globals();

  PackageRow installed_row;
  PackageRow update_row;
  if (!find_update_pair_from_installed_annotation(installed_row, update_row)) {
    SUCCEED("No installed package with a newer repo candidate in the test environment.");
    return;
  }

  auto installed_deps = dnf_backend_get_package_deps(installed_row.nevra);
  auto upgrade_deps = dnf_backend_get_package_deps(update_row.nevra);

  REQUIRE(upgrade_deps == installed_deps);
  REQUIRE(upgrade_deps.find("(installed packages only)") == std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that file list lookup returns either content or a friendly state.
// -----------------------------------------------------------------------------
TEST_CASE("File list query is safe and returns valid state")
{
  reset_backend_globals();

  auto results = dnf_backend_search_package_rows_interruptible("bash", backend_search_options(false, false), nullptr);
  REQUIRE(!results.empty());

  std::optional<std::vector<std::string>> file_paths;
  auto files = dnf_backend_get_installed_package_files(
      results.front().nevra, PackageDetailsContext::INSTALLED_CONTEXT, 0, &file_paths);

  // Either it is installed (returns file list)
  // or not installed (returns friendly message).
  bool is_not_installed_msg = files.find("File list available only for installed packages.") != std::string::npos;
  REQUIRE(file_paths.has_value() != is_not_installed_msg);

  bool has_content = !files.empty();
  if (!is_not_installed_msg) {
    REQUIRE(has_content);
  }
}

// -----------------------------------------------------------------------------
// Verify that an installed file list provides one path per recorded file.
// -----------------------------------------------------------------------------
TEST_CASE("Installed file list provides individual paths")
{
  reset_backend_globals();

  auto installed_rows = dnf_backend_get_installed_package_rows_interruptible(nullptr);
  REQUIRE(!installed_rows.empty());

  std::optional<std::vector<std::string>> file_paths;
  std::string files = dnf_backend_get_installed_package_files(
      installed_rows.front().nevra, PackageDetailsContext::INSTALLED_CONTEXT, 0, &file_paths);
  REQUIRE(file_paths.has_value());

  if (file_paths->empty()) {
    REQUIRE(files == "No files recorded for this installed package.");
    return;
  }

  size_t offset = 0;
  for (const std::string &path : *file_paths) {
    REQUIRE(files.compare(offset, path.size(), path) == 0);
    offset += path.size();
    REQUIRE(offset < files.size());
    REQUIRE(files[offset] == '\n');
    ++offset;
  }
  REQUIRE(offset == files.size());
}

// -----------------------------------------------------------------------------
// Verify that the Files tab can use an available update row to show the files from the currently installed package.
// -----------------------------------------------------------------------------
TEST_CASE("File list query uses installed package for update rows")
{
  reset_backend_globals();

  PackageRow installed_row;
  PackageRow update_row;
  if (!find_update_pair_from_installed_annotation(installed_row, update_row)) {
    SUCCEED("No installed package with a newer repo candidate in the test environment.");
    return;
  }

  auto installed_files = dnf_backend_get_installed_package_files(installed_row.nevra, 1500);
  auto upgrade_files = dnf_backend_get_installed_package_files(update_row.nevra, 1500);

  REQUIRE(upgrade_files.find("File list available only for installed packages.") == std::string::npos);
  REQUIRE(upgrade_files == installed_files);
}

// -----------------------------------------------------------------------------
// Verify that selected-version file lookup does not use another installed version.
// -----------------------------------------------------------------------------
TEST_CASE("File list selected-version mode requires exact installed package")
{
  reset_backend_globals();

  PackageRow installed_row;
  PackageRow update_row;
  if (!find_update_pair_from_installed_annotation(installed_row, update_row)) {
    SUCCEED("No installed package with a newer repo candidate in the test environment.");
    return;
  }

  auto files = dnf_backend_get_installed_package_files(update_row.nevra, PackageDetailsContext::SELECTED_VERSION, 1500);

  REQUIRE(files.find("File list available only for installed packages.") != std::string::npos);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
