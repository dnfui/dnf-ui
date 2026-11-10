// -----------------------------------------------------------------------------
// src/cli/backend_cli.cpp
// Minimal backend probe for developer testing
// -----------------------------------------------------------------------------
#include "dnf5daemon_client/transaction_service_client.hpp"
#include "dnf5daemon_client/repository_service_client.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "transaction/transaction_preview.hpp"
#include "transaction/transaction_request.hpp"
#include "upgrade/daemon_upgrade_target.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char *
state_text(PackageInstallState state)
{
  switch (state) {
  case PackageInstallState::INSTALLED:
    return "installed";
  case PackageInstallState::LOCAL_ONLY:
    return "installed-local-only";
  case PackageInstallState::INSTALLED_NEWER_THAN_REPO:
    return "installed-newer-than-repo";
  case PackageInstallState::UPGRADEABLE:
    return "upgradeable";
  case PackageInstallState::DOWNGRADEABLE:
    return "downgradeable";
  case PackageInstallState::AVAILABLE:
  default:
    return "available";
  }
}

void
print_usage(const char *program_name)
{
  std::cout << "Usage:\n"
            << "  " << program_name << " search [--description] [--exact] [--all-versions] [--] TERM\n"
            << "  " << program_name << " list-installed [FILTER]\n"
            << "  " << program_name << " list-packages [--all-versions]\n"
            << "  " << program_name << " list-upgrades\n"
            << "  " << program_name << " repos list\n"
            << "  " << program_name << " repos enable REPO_ID\n"
            << "  " << program_name << " repos disable REPO_ID\n"
            << "  " << program_name << " details NEVRA\n"
            << "  " << program_name << " preview ACTION SPEC\n"
            << "  " << program_name << " preview-upgrade-all\n\n"
            << "Preview actions: install, upgrade, downgrade, remove, reinstall\n";
}

bool
row_matches_filter(const PackageRow &row, const std::string &filter)
{
  return filter.empty() || row.nevra.find(filter) != std::string::npos || row.name.find(filter) != std::string::npos;
}

void
print_rows(const std::vector<PackageRow> &rows, const std::string &filter = {})
{
  size_t shown = 0;
  for (const auto &row : rows) {
    if (!row_matches_filter(row, filter)) {
      continue;
    }

    InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(row);
    std::cout << row.nevra << '\t' << state_text(resolution.state) << '\t' << row.repo << '\t' << row.summary << '\n';
    shown++;
  }

  std::cout << "Rows: " << shown << '\n';
}

bool
parse_search_options(int argc, char *argv[], int start, DnfBackendSearchOptions &options, std::string &term)
{
  bool options_done = false;
  for (int i = start; i < argc; i++) {
    const std::string arg = argv[i];
    if (!options_done && arg == "--") {
      if (!term.empty()) {
        return false;
      }
      options_done = true;
      continue;
    }
    if (!options_done && arg == "--description") {
      options.search_in_description = true;
      continue;
    }
    if (!options_done && arg == "--exact") {
      options.exact_match = true;
      continue;
    }
    if (!options_done && arg == "--all-versions") {
      options.latest_only = false;
      continue;
    }
    if (!options_done && !arg.empty() && arg[0] == '-') {
      return false;
    }
    if (!term.empty()) {
      return false;
    }
    term = arg;
  }

  return !term.empty();
}

bool
parse_list_options(int argc, char *argv[], int start, DnfBackendSearchOptions &options)
{
  for (int i = start; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--all-versions") {
      options.latest_only = false;
      continue;
    }
    return false;
  }

  return true;
}

void
print_preview_section(const char *title, const std::vector<TransactionPreviewPackage> &packages)
{
  if (packages.empty()) {
    return;
  }

  std::cout << title << ":\n";
  for (const auto &package : packages) {
    std::cout << "  " << package.label << '\n';
  }
}

void
print_preview(const TransactionPreview &preview)
{
  if (!preview.resolve_warnings.empty()) {
    std::cout << "Warnings:\n" << preview.resolve_warnings << '\n';
  }

  if (preview.empty()) {
    std::cout << "No transaction changes were resolved.\n";
    return;
  }

  print_preview_section("Install", preview.install);
  print_preview_section("Upgrade", preview.upgrade);
  print_preview_section("Downgrade", preview.downgrade);
  print_preview_section("Reinstall", preview.reinstall);
  print_preview_section("Remove", preview.remove);
  print_preview_section("Replaced", preview.replaced);
  std::cout << "Disk space delta: " << preview.disk_space_delta << '\n';
}

bool
fill_request(TransactionRequest &request, const std::string &action, const std::string &spec)
{
  if (action == "install") {
    request.install.push_back(spec);
    return true;
  }
  if (action == "upgrade") {
    request.upgrade.push_back(spec);
    return true;
  }
  if (action == "downgrade") {
    request.downgrade.push_back(spec);
    return true;
  }
  if (action == "remove") {
    request.remove.push_back(spec);
    return true;
  }
  if (action == "reinstall") {
    request.reinstall.push_back(spec);
    return true;
  }

  return false;
}

int
run_search(int argc, char *argv[])
{
  DnfBackendSearchOptions options;
  std::string term;
  if (!parse_search_options(argc, argv, 2, options, term)) {
    std::cerr << "Invalid search arguments.\n";
    return 1;
  }

  dnf_backend_refresh_installed_snapshot();
  print_rows(dnf_backend_search_package_rows_interruptible(term, options, nullptr));
  return 0;
}

int
run_list_installed(int argc, char *argv[])
{
  if (argc > 3) {
    std::cerr << "Invalid list-installed arguments.\n";
    return 1;
  }

  dnf_backend_refresh_installed_snapshot();
  const std::string filter = argc == 3 ? argv[2] : "";
  print_rows(dnf_backend_get_installed_package_rows_interruptible(nullptr), filter);
  return 0;
}

int
run_list_packages(int argc, char *argv[])
{
  DnfBackendSearchOptions options;
  if (!parse_list_options(argc, argv, 2, options)) {
    std::cerr << "Invalid list-packages arguments.\n";
    return 1;
  }

  dnf_backend_refresh_installed_snapshot();
  print_rows(dnf_backend_get_browse_package_rows_interruptible(options, nullptr));
  return 0;
}

int
run_list_upgrades(int argc)
{
  if (argc != 2) {
    std::cerr << "Invalid list-upgrades arguments.\n";
    return 1;
  }

  std::vector<DaemonUpgradeTarget> targets;
  std::string error;
  if (!transaction_service_client_list_upgrade_targets(targets, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  for (const auto &target : targets) {
    std::cout << target.full_nevra << '\t' << target.repo_id << '\t' << target.upgrade_spec() << '\n';
  }

  std::cout << "Rows: " << targets.size() << '\n';
  return 0;
}

int
run_repos(int argc, char *argv[])
{
  if (argc < 3) {
    std::cerr << "Invalid repos arguments.\n";
    return 1;
  }

  const std::string action = argv[2];
  if (action == "list") {
    if (argc != 3) {
      std::cerr << "Invalid repos list arguments.\n";
      return 1;
    }

    std::vector<RepositoryInfo> repositories;
    std::string error;
    if (!repository_service_client_list(repositories, error)) {
      std::cerr << error << '\n';
      return 1;
    }

    for (const auto &repository : repositories) {
      std::cout << repository.id << '\t' << (repository.enabled ? "enabled" : "disabled") << '\t' << repository.name
                << '\n';
    }
    std::cout << "Rows: " << repositories.size() << '\n';
    return 0;
  }

  if (action == "enable" || action == "disable") {
    if (argc != 4) {
      std::cerr << "Invalid repos " << action << " arguments.\n";
      return 1;
    }

    const std::string repo_id = argv[3];
    RepositoryWriteResult result;
    if (action == "enable") {
      result = repository_service_client_apply_changes({ repo_id }, {});
    } else {
      result = repository_service_client_apply_changes({}, { repo_id });
    }

    if (!result.enable_succeeded || !result.disable_succeeded) {
      std::cerr << result.error << '\n';
      return 1;
    }

    std::cout << "Repository " << repo_id << ' ' << (action == "enable" ? "enabled" : "disabled") << ".\n";
    return 0;
  }

  std::cerr << "Unknown repos action.\n";
  return 1;
}

int
run_details(int argc, char *argv[])
{
  if (argc != 3) {
    std::cerr << "Invalid details arguments.\n";
    return 1;
  }

  dnf_backend_refresh_installed_snapshot();
  PackageInfoPresentation info =
      dnf_backend_get_package_info_presentation(argv[2], PackageDetailsContext::SELECTED_VERSION);
  if (info.name.empty()) {
    std::cerr << info.full_text << '\n';
    return 1;
  }

  std::cout << info.full_text << '\n';
  return 0;
}

int
run_preview(int argc, char *argv[])
{
  if (argc != 4) {
    std::cerr << "Invalid preview arguments.\n";
    return 1;
  }

  TransactionRequest request;
  if (!fill_request(request, argv[2], argv[3])) {
    std::cerr << "Unknown preview action.\n";
    return 1;
  }

  std::string validation_error;
  if (!request.validate(validation_error)) {
    std::cerr << validation_error << '\n';
    return 1;
  }

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;
  if (!transaction_service_client_preview_request(request, preview, transaction_path, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  print_preview(preview);
  if (!transaction_path.empty()) {
    transaction_service_client_release_request(transaction_path);
  }
  return 0;
}

int
run_preview_upgrade_all(int argc)
{
  if (argc != 2) {
    std::cerr << "Invalid preview-upgrade-all arguments.\n";
    return 1;
  }

  TransactionPreview preview;
  std::string transaction_path;
  std::string error;
  if (!transaction_service_client_preview_upgrade_all_request(preview, transaction_path, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  print_preview(preview);
  if (!transaction_path.empty()) {
    transaction_service_client_release_request(transaction_path);
  }
  return 0;
}

} // namespace

int
main(int argc, char *argv[])
{
  dnfui_i18n_init();

  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  try {
    const std::string command = argv[1];
    if (command == "search") {
      return run_search(argc, argv);
    }
    if (command == "list-installed") {
      return run_list_installed(argc, argv);
    }
    if (command == "list-packages") {
      return run_list_packages(argc, argv);
    }
    if (command == "list-upgrades" || command == "list-upgradable") {
      return run_list_upgrades(argc);
    }
    if (command == "repos") {
      return run_repos(argc, argv);
    }
    if (command == "details") {
      return run_details(argc, argv);
    }
    if (command == "preview") {
      return run_preview(argc, argv);
    }
    if (command == "preview-upgrade-all") {
      return run_preview_upgrade_all(argc);
    }
    if (command == "help" || command == "--help" || command == "-h") {
      print_usage(argv[0]);
      return 0;
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }

  std::cerr << "Unknown command.\n";
  print_usage(argv[0]);
  return 1;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
