// -----------------------------------------------------------------------------
// transaction_service_system_bus_client.cpp
// Native system bus smoke-test client for the installed transaction service.
// Keeps one client connection alive across preview, apply, and release so the
// installed service ownership checks stay enabled during native smoke tests.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "transaction_request.hpp"
#include "transaction_service_client.hpp"
#include "transaction_service_client_internal.hpp"

#include <gio/gio.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

enum class SmokeMode {
  PREVIEW,
  APPLY,
  DISCONNECT,
};

struct SmokeOptions {
  SmokeMode mode = SmokeMode::PREVIEW;
  std::string install_spec;
  std::string reinstall_spec;
};

// -----------------------------------------------------------------------------
// Release one prepared request when the helper exits after a preview or apply.
// -----------------------------------------------------------------------------
struct ScopedRequestRelease {
  std::string transaction_path;
  bool enabled = true;

  ~ScopedRequestRelease()
  {
    if (enabled && !transaction_path.empty()) {
      transaction_service_client_release_request(transaction_path);
    }
  }
};

// -----------------------------------------------------------------------------
// Print the supported command line shape for this helper.
// -----------------------------------------------------------------------------
void
print_usage()
{
  std::cerr << "Usage:\n"
            << "  dnfui-service-smoke-client --preview --install-spec <spec>\n"
            << "  dnfui-service-smoke-client --preview --reinstall-spec <nevra>\n"
            << "  dnfui-service-smoke-client --apply --install-spec <spec>\n"
            << "  dnfui-service-smoke-client --apply --reinstall-spec <nevra>\n"
            << "  dnfui-service-smoke-client --disconnect --install-spec <spec>\n"
            << "  dnfui-service-smoke-client --disconnect --reinstall-spec <nevra>\n";
}

// -----------------------------------------------------------------------------
// Parse the smoke-test mode and one package spec from argv.
// -----------------------------------------------------------------------------
bool
parse_arguments(int argc, char **argv, SmokeOptions &options_out, std::string &error_out)
{
  options_out = {};
  error_out.clear();

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--preview") {
      options_out.mode = SmokeMode::PREVIEW;
      continue;
    }

    if (arg == "--apply") {
      options_out.mode = SmokeMode::APPLY;
      continue;
    }

    if (arg == "--disconnect") {
      options_out.mode = SmokeMode::DISCONNECT;
      continue;
    }

    if (arg == "--install-spec" || arg == "--reinstall-spec") {
      if (i + 1 >= argc) {
        error_out = "Missing value for " + arg + ".";
        return false;
      }

      const std::string value = argv[++i];
      if (arg == "--install-spec") {
        options_out.install_spec = value;
      } else {
        options_out.reinstall_spec = value;
      }
      continue;
    }

    error_out = "Unknown argument: " + arg + ".";
    return false;
  }

  if (options_out.install_spec.empty() == options_out.reinstall_spec.empty()) {
    error_out = "Set exactly one of --install-spec or --reinstall-spec.";
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Build one shared transaction request from the parsed smoke-test options.
// -----------------------------------------------------------------------------
bool
build_request(const SmokeOptions &options,
              TransactionRequest &request_out,
              std::string &expected_spec_out,
              std::string &error_out)
{
  request_out = {};
  expected_spec_out.clear();
  error_out.clear();

  if (!options.install_spec.empty()) {
    request_out.install.push_back(options.install_spec);
    expected_spec_out = options.install_spec;
  } else {
    request_out.reinstall.push_back(options.reinstall_spec);
    expected_spec_out = options.reinstall_spec;
  }

  return request_out.validate(error_out);
}

// -----------------------------------------------------------------------------
// Return true when any preview row contains the requested package spec text.
// -----------------------------------------------------------------------------
bool
preview_contains_expected_spec(const TransactionPreview &preview, const std::string &expected_spec)
{
  const std::array<const std::vector<std::string> *, 5> preview_lists = {
    &preview.install, &preview.upgrade, &preview.downgrade, &preview.reinstall, &preview.remove,
  };

  for (const auto *specs : preview_lists) {
    for (const auto &spec : *specs) {
      if (spec.find(expected_spec) != std::string::npos) {
        return true;
      }
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
// Print one preview section when it contains resolved package rows.
// -----------------------------------------------------------------------------
void
print_preview_section(const char *label, const std::vector<std::string> &specs)
{
  if (!label || specs.empty()) {
    return;
  }

  std::cout << label << ":\n";
  for (const auto &spec : specs) {
    std::cout << "  " << spec << "\n";
  }
}

// -----------------------------------------------------------------------------
// Print the resolved preview in a human-readable layout.
// -----------------------------------------------------------------------------
void
print_preview(const TransactionPreview &preview)
{
  print_preview_section("Install", preview.install);
  print_preview_section("Upgrade", preview.upgrade);
  print_preview_section("Downgrade", preview.downgrade);
  print_preview_section("Reinstall", preview.reinstall);
  print_preview_section("Remove", preview.remove);
  std::cout << "Disk space delta: " << preview.disk_space_delta << "\n";
}

// -----------------------------------------------------------------------------
// Run one preview or apply smoke-test flow on the installed system bus service.
// -----------------------------------------------------------------------------
bool
run_preview_or_apply(const SmokeOptions &options)
{
  TransactionRequest request;
  std::string expected_spec;
  std::string error;
  if (!build_request(options, request, expected_spec, error)) {
    std::cerr << error << "\n";
    return false;
  }

  TransactionPreview preview;
  std::string transaction_path;
  if (!transaction_service_client_preview_request(request, preview, transaction_path, error)) {
    std::cerr << error << "\n";
    return false;
  }

  ScopedRequestRelease release_guard;
  release_guard.transaction_path = transaction_path;

  std::cout << "Transaction path: " << transaction_path << "\n";
  std::cout << "Preview is ready.\n";

  if (!preview_contains_expected_spec(preview, expected_spec)) {
    std::cerr << "Resolved preview did not contain the expected package spec: " << expected_spec << "\n";
    return false;
  }

  print_preview(preview);

  if (options.mode != SmokeMode::APPLY) {
    transaction_service_client_release_request(transaction_path);
    release_guard.transaction_path.clear();
    std::cout << "Preview request released.\n";
    return true;
  }

  std::cout << "Starting apply.\n";
  if (!transaction_service_client_apply_started_request(
          transaction_path,
          [](const std::string &line) {
            if (!line.empty()) {
              std::cout << line << "\n";
            }
          },
          error)) {
    std::cerr << error << "\n";
    return false;
  }

  transaction_service_client_release_request(transaction_path);
  release_guard.transaction_path.clear();
  std::cout << "Apply finished successfully.\n";
  std::cout << "Apply request released.\n";
  return true;
}

// -----------------------------------------------------------------------------
// Start one request and then exit so the installed service sees the client
// disconnect and cleans up the orphaned request object.
// -----------------------------------------------------------------------------
bool
run_disconnect(const SmokeOptions &options)
{
  TransactionRequest request;
  std::string expected_spec;
  std::string error;
  if (!build_request(options, request, expected_spec, error)) {
    std::cerr << error << "\n";
    return false;
  }

  GDBusConnection *connection = transaction_service_client_connect(error);
  if (!connection) {
    std::cerr << error << "\n";
    return false;
  }

  std::string transaction_path;
  const bool ok = transaction_service_client_start_transaction_request(connection, request, transaction_path, error);
  g_object_unref(connection);

  if (!ok) {
    std::cerr << error << "\n";
    return false;
  }

  std::cout << transaction_path << std::endl;
  return true;
}

} // namespace

// -----------------------------------------------------------------------------
// Entry point for the native installed-service smoke-test helper.
// -----------------------------------------------------------------------------
int
main(int argc, char **argv)
{
  dnfui_i18n_init();

  SmokeOptions options;
  std::string error;
  if (!parse_arguments(argc, argv, options, error)) {
    if (!error.empty()) {
      std::cerr << error << "\n";
    }
    print_usage();
    return EXIT_FAILURE;
  }

  // The native installed-service smoke tests must exercise the real system bus
  // path that the desktop app uses on Fedora.
  g_setenv("DNFUI_TRANSACTION_BUS", "system", TRUE);

  const bool ok = options.mode == SmokeMode::DISCONNECT ? run_disconnect(options) : run_preview_or_apply(options);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
