// -----------------------------------------------------------------------------
// transaction_service_client_parser.cpp
// Parse dnf5daemon transaction and package-list items.
// -----------------------------------------------------------------------------
#include "transaction_service_client_parser.hpp"

#include "i18n.hpp"
#include "transaction_service_client.hpp"
#include "transaction/transaction_preview.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace {

// -----------------------------------------------------------------------------
// Convert one string to lower case for daemon enum matching.
// -----------------------------------------------------------------------------
std::string
ascii_lower(std::string value)
{
  std::transform(
      value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// -----------------------------------------------------------------------------
// Read one string field from a daemon object map.
// -----------------------------------------------------------------------------
std::string
map_lookup_string(GVariant *map, const char *key)
{
  const gchar *value = nullptr;
  if (map && g_variant_lookup(map, key, "&s", &value)) {
    return value ? value : "";
  }
  return "";
}

// -----------------------------------------------------------------------------
// Read one signed size field from a daemon object map.
// -----------------------------------------------------------------------------
long long
map_lookup_int64(GVariant *map, const char *key)
{
  if (!map) {
    return 0;
  }

  gint64 signed64 = 0;
  if (g_variant_lookup(map, key, "x", &signed64)) {
    return static_cast<long long>(signed64);
  }

  guint64 unsigned64 = 0;
  if (g_variant_lookup(map, key, "t", &unsigned64)) {
    return static_cast<long long>(unsigned64);
  }

  gint32 signed32 = 0;
  if (g_variant_lookup(map, key, "i", &signed32)) {
    return signed32;
  }

  guint32 unsigned32 = 0;
  if (g_variant_lookup(map, key, "u", &unsigned32)) {
    return unsigned32;
  }

  return 0;
}

// -----------------------------------------------------------------------------
// Build the package item used by preview display and safety checks.
// A daemon package item must contain enough fields to identify one package.
// -----------------------------------------------------------------------------
bool
package_from_daemon_object(GVariant *object, TransactionPreviewPackage &package_out, std::string &error_out)
{
  package_out = {};

  const std::string name = map_lookup_string(object, "name");
  const std::string epoch = map_lookup_string(object, "epoch");
  const std::string version = map_lookup_string(object, "version");
  const std::string release = map_lookup_string(object, "release");
  const std::string arch = map_lookup_string(object, "arch");

  if (name.empty() || version.empty() || release.empty() || arch.empty()) {
    error_out = _("dnf5daemon returned an incomplete package item.");
    return false;
  }

  package_out.name = name;
  package_out.arch = arch;

  std::ostringstream label;
  label << name << "-";
  if (!epoch.empty() && epoch != "0") {
    label << epoch << ":";
  }
  label << version << "-" << release << "." << arch;
  package_out.label = label.str();
  return true;
}

} // namespace

// -----------------------------------------------------------------------------
// Build one upgrade target returned by dnf5daemon's package-list API.
// -----------------------------------------------------------------------------
bool
transaction_service_client_upgrade_target_from_daemon_object(GVariant *object,
                                                             DaemonUpgradeTarget &target_out,
                                                             std::string &error_out)
{
  DaemonUpgradeTarget target;
  target.name = map_lookup_string(object, "name");
  target.epoch = map_lookup_string(object, "epoch");
  target.version = map_lookup_string(object, "version");
  target.release = map_lookup_string(object, "release");
  target.arch = map_lookup_string(object, "arch");
  target.repo_id = map_lookup_string(object, "repo_id");
  target.nevra = map_lookup_string(object, "nevra");
  target.full_nevra = map_lookup_string(object, "full_nevra");

  if (target.name.empty() || target.version.empty() || target.release.empty() || target.arch.empty()) {
    error_out = _("dnf5daemon returned an incomplete upgrade item.");
    return false;
  }

  const std::string normalized_epoch = target.epoch.empty() ? "0" : target.epoch;

  if (target.nevra.empty()) {
    std::ostringstream nevra;
    nevra << target.name << "-";
    if (normalized_epoch != "0") {
      nevra << normalized_epoch << ":";
    }
    nevra << target.version << "-" << target.release << "." << target.arch;
    target.nevra = nevra.str();
  }
  if (target.full_nevra.empty()) {
    std::ostringstream full_nevra;
    full_nevra << target.name << "-" << normalized_epoch << ":" << target.version << "-" << target.release << "."
               << target.arch;
    target.full_nevra = full_nevra.str();
  }

  target_out = std::move(target);
  return true;
}

// -----------------------------------------------------------------------------
// Add one daemon transaction item to the preview model.
// Unknown actions fail the whole preview so the dialog never hides work.
// -----------------------------------------------------------------------------
bool
transaction_service_client_append_daemon_preview_item(TransactionPreview &preview,
                                                      const std::string &object_type,
                                                      const std::string &action,
                                                      GVariant *attributes,
                                                      GVariant *object,
                                                      std::string &error_out)
{
  const std::string lower_object_type = ascii_lower(object_type);
  if (lower_object_type != "package" && lower_object_type != "skipped") {
    error_out = "Unsupported dnf5daemon transaction item type: " + object_type + ".";
    return false;
  }

  const std::string lower_action = ascii_lower(action);

  TransactionPreviewPackage package;
  if (!package_from_daemon_object(object, package, error_out)) {
    return false;
  }

  if (lower_object_type == "skipped") {
    const std::string reason_skipped = map_lookup_string(attributes, "reason_skipped");
    std::string warning;
    if (reason_skipped == "conflict") {
      warning = dnfui_i18n_format(_("%s was skipped because of a package conflict."), package.label.c_str());
    } else if (reason_skipped == "broken_dependency") {
      warning = dnfui_i18n_format(_("%s was skipped because of broken dependencies."), package.label.c_str());
    } else if (reason_skipped == "vendor_change") {
      warning = dnfui_i18n_format(_("%s was skipped because of a vendor change restriction."), package.label.c_str());
    } else if (!reason_skipped.empty()) {
      warning = dnfui_i18n_format(_("%s was skipped by dnf5daemon: %s"), package.label.c_str(), reason_skipped.c_str());
    } else {
      warning = dnfui_i18n_format(_("%s was skipped by dnf5daemon."), package.label.c_str());
    }

    if (!preview.resolve_warnings.empty()) {
      preview.resolve_warnings += "\n";
    }
    preview.resolve_warnings += warning;
    return true;
  }

  const long long install_size = map_lookup_int64(object, "install_size");

  if (lower_action == "install") {
    preview.install.push_back(package);
    preview.disk_space_delta += install_size;
    return true;
  }
  if (lower_action == "upgrade") {
    preview.upgrade.push_back(package);
    preview.disk_space_delta += install_size;
    return true;
  }
  if (lower_action == "downgrade") {
    preview.downgrade.push_back(package);
    preview.disk_space_delta += install_size;
    return true;
  }
  if (lower_action == "reinstall") {
    preview.reinstall.push_back(package);
    return true;
  }
  if (lower_action == "remove") {
    preview.remove.push_back(package);
    preview.disk_space_delta -= install_size;
    return true;
  }
  if (lower_action == "replaced") {
    preview.replaced.push_back(package);
    preview.disk_space_delta -= install_size;
    return true;
  }

  error_out = "Unsupported dnf5daemon transaction action in preview: " + action + ".";
  return false;
}

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Test-only hook for daemon preview parser coverage.
// -----------------------------------------------------------------------------
bool
transaction_service_client_testonly_build_preview_from_item(const std::string &object_type,
                                                            const std::string &action,
                                                            const std::string &name,
                                                            TransactionPreview &preview,
                                                            std::string &error_out,
                                                            const char *reason_skipped)
{
  TransactionPreview built_preview = preview;

  GVariantBuilder attributes_builder;
  g_variant_builder_init(&attributes_builder, G_VARIANT_TYPE("a{sv}"));
  if (reason_skipped) {
    g_variant_builder_add(&attributes_builder, "{sv}", "reason_skipped", g_variant_new_string(reason_skipped));
  }

  GVariantBuilder object_builder;
  g_variant_builder_init(&object_builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&object_builder, "{sv}", "name", g_variant_new_string(name.c_str()));
  g_variant_builder_add(&object_builder, "{sv}", "epoch", g_variant_new_string("0"));
  g_variant_builder_add(&object_builder, "{sv}", "version", g_variant_new_string("2.0"));
  g_variant_builder_add(&object_builder, "{sv}", "release", g_variant_new_string("3"));
  g_variant_builder_add(&object_builder, "{sv}", "arch", g_variant_new_string("x86_64"));
  g_variant_builder_add(&object_builder, "{sv}", "install_size", g_variant_new_int64(4096));

  GVariant *object = g_variant_ref_sink(g_variant_builder_end(&object_builder));
  GVariant *attributes = g_variant_ref_sink(g_variant_builder_end(&attributes_builder));
  bool ok = transaction_service_client_append_daemon_preview_item(
      built_preview, object_type, action, attributes, object, error_out);
  g_variant_unref(attributes);
  g_variant_unref(object);

  if (!ok) {
    return false;
  }

  preview = std::move(built_preview);
  return true;
}

// -----------------------------------------------------------------------------
// Test-only hook for daemon upgrade target parser coverage.
// -----------------------------------------------------------------------------
bool
transaction_service_client_testonly_build_upgrade_target_from_fields(const std::string &name,
                                                                     const std::string &epoch,
                                                                     const std::string &version,
                                                                     const std::string &release,
                                                                     const std::string &arch,
                                                                     const std::string &repo_id,
                                                                     const std::string &nevra,
                                                                     const std::string &full_nevra,
                                                                     DaemonUpgradeTarget &target_out,
                                                                     std::string &error_out)
{
  target_out = {};
  error_out.clear();

  GVariantBuilder object_builder;
  g_variant_builder_init(&object_builder, G_VARIANT_TYPE("a{sv}"));
  if (!name.empty()) {
    g_variant_builder_add(&object_builder, "{sv}", "name", g_variant_new_string(name.c_str()));
  }
  if (!epoch.empty()) {
    g_variant_builder_add(&object_builder, "{sv}", "epoch", g_variant_new_string(epoch.c_str()));
  }
  if (!version.empty()) {
    g_variant_builder_add(&object_builder, "{sv}", "version", g_variant_new_string(version.c_str()));
  }
  if (!release.empty()) {
    g_variant_builder_add(&object_builder, "{sv}", "release", g_variant_new_string(release.c_str()));
  }
  if (!arch.empty()) {
    g_variant_builder_add(&object_builder, "{sv}", "arch", g_variant_new_string(arch.c_str()));
  }
  if (!repo_id.empty()) {
    g_variant_builder_add(&object_builder, "{sv}", "repo_id", g_variant_new_string(repo_id.c_str()));
  }
  if (!nevra.empty()) {
    g_variant_builder_add(&object_builder, "{sv}", "nevra", g_variant_new_string(nevra.c_str()));
  }
  if (!full_nevra.empty()) {
    g_variant_builder_add(&object_builder, "{sv}", "full_nevra", g_variant_new_string(full_nevra.c_str()));
  }

  GVariant *object = g_variant_ref_sink(g_variant_builder_end(&object_builder));
  bool ok = transaction_service_client_upgrade_target_from_daemon_object(object, target_out, error_out);
  g_variant_unref(object);
  return ok;
}
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
