// -----------------------------------------------------------------------------
// transaction_service_client_wait.cpp
// Wait and progress signal handling for the GUI-side transaction client.
// Receives progress from DNF5 dnf5daemon and forwards a small set of useful
// messages to the existing transaction progress window.
// -----------------------------------------------------------------------------
#include "transaction_service_client_internal.hpp"

#include "debug_trace.hpp"
#include "i18n.hpp"

#include <glib.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr const char *kDnfDaemonName = "org.rpm.dnf.v0";

// -----------------------------------------------------------------------------
// Read one string or object path child from a D-Bus signal parameter tuple.
// -----------------------------------------------------------------------------
std::string
variant_child_text(GVariant *parameters, guint index)
{
  if (!parameters || g_variant_n_children(parameters) <= index) {
    return {};
  }

  GVariant *child = g_variant_get_child_value(parameters, index);
  std::string value;
  if (g_variant_is_of_type(child, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(child, G_VARIANT_TYPE_OBJECT_PATH)) {
    value = g_variant_get_string(child, nullptr);
  }
  g_variant_unref(child);
  return value;
}

// -----------------------------------------------------------------------------
// Read one unsigned integer child from a D-Bus signal parameter tuple.
// -----------------------------------------------------------------------------
uint64_t
variant_child_uint64(GVariant *parameters, guint index)
{
  if (!parameters || g_variant_n_children(parameters) <= index) {
    return 0;
  }

  GVariant *child = g_variant_get_child_value(parameters, index);
  uint64_t value = 0;
  if (g_variant_is_of_type(child, G_VARIANT_TYPE_UINT64)) {
    value = g_variant_get_uint64(child);
  } else if (g_variant_is_of_type(child, G_VARIANT_TYPE_INT64)) {
    gint64 signed_value = g_variant_get_int64(child);
    value = signed_value > 0 ? static_cast<uint64_t>(signed_value) : 0;
  } else if (g_variant_is_of_type(child, G_VARIANT_TYPE_UINT32)) {
    value = g_variant_get_uint32(child);
  } else if (g_variant_is_of_type(child, G_VARIANT_TYPE_INT32)) {
    gint32 signed_value = g_variant_get_int32(child);
    value = signed_value > 0 ? static_cast<uint64_t>(signed_value) : 0;
  }
  g_variant_unref(child);
  return value;
}

// -----------------------------------------------------------------------------
// Read one string array child from a D-Bus signal parameter tuple.
// -----------------------------------------------------------------------------
std::vector<std::string>
variant_child_string_array(GVariant *parameters, guint index)
{
  std::vector<std::string> values;
  if (!parameters || g_variant_n_children(parameters) <= index) {
    return values;
  }

  GVariant *child = g_variant_get_child_value(parameters, index);
  if (g_variant_is_of_type(child, G_VARIANT_TYPE_STRING_ARRAY)) {
    GVariantIter iter;
    const gchar *value = nullptr;
    g_variant_iter_init(&iter, child);
    while (g_variant_iter_next(&iter, "&s", &value)) {
      values.emplace_back(value ? value : "");
    }
  }
  g_variant_unref(child);
  return values;
}

// -----------------------------------------------------------------------------
// Return true when a daemon signal belongs to the transaction session we started.
// -----------------------------------------------------------------------------
bool
signal_matches_transaction(TransactionServiceProgressForwarder *forwarder, GVariant *parameters)
{
  if (!forwarder || forwarder->transaction_path.empty()) {
    return false;
  }

  return variant_child_text(parameters, 0) == forwarder->transaction_path;
}

// -----------------------------------------------------------------------------
// Forward one line to the UI progress callback.
// -----------------------------------------------------------------------------
void
forward_progress_line(TransactionServiceProgressForwarder *forwarder, const std::string &line)
{
  if (!forwarder || !forwarder->progress_callback || !*forwarder->progress_callback || line.empty()) {
    return;
  }

  DNFUI_TRACE("dnf5daemon progress line=%s", line.c_str());
  (*forwarder->progress_callback)(line);
}

// -----------------------------------------------------------------------------
// Return the package text that belongs to one daemon download id.
// -----------------------------------------------------------------------------
std::string
download_description(TransactionServiceProgressForwarder *forwarder, const std::string &download_id)
{
  if (!forwarder || download_id.empty()) {
    return {};
  }

  auto it = forwarder->download_description_by_id.find(download_id);
  if (it == forwarder->download_description_by_id.end()) {
    return {};
  }

  return it->second;
}

// -----------------------------------------------------------------------------
// Ask the UI whether dnf5daemon should trust one repository signing key.
// -----------------------------------------------------------------------------
void
handle_key_import_request(GDBusConnection *connection,
                          TransactionServiceProgressForwarder *forwarder,
                          GVariant *parameters)
{
  TransactionKeyImportRequest request;
  request.key_id = variant_child_text(parameters, 1);
  request.user_ids = variant_child_string_array(parameters, 2);
  request.fingerprint = variant_child_text(parameters, 3);
  request.key_url = variant_child_text(parameters, 4);

  DNFUI_TRACE("dnf5daemon key import request path=%s key=%s",
              forwarder ? forwarder->transaction_path.c_str() : "",
              request.key_id.c_str());

  bool confirmed = false;
  if (forwarder && forwarder->key_import_callback && *forwarder->key_import_callback) {
    confirmed = (*forwarder->key_import_callback)(request);
  } else {
    if (forwarder) {
      forwarder->key_confirm_error = _("The repository cannot be used until its signing key is trusted.");
    }
    forward_progress_line(forwarder, _("The repository cannot be used until its signing key is trusted."));
    return;
  }

  std::string confirm_error;
  if (!transaction_service_client_confirm_key(
          connection, forwarder ? forwarder->transaction_path : "", request.key_id, confirmed, confirm_error)) {
    if (forwarder) {
      forwarder->key_confirm_error = confirm_error;
    }
    forward_progress_line(forwarder, confirm_error);
    return;
  }

  if (!confirmed) {
    if (forwarder) {
      forwarder->key_confirm_error = _("Repository signing key was rejected.");
    }
    forward_progress_line(forwarder, _("Repository signing key rejected."));
    return;
  }

  forward_progress_line(forwarder, _("Repository signing key accepted."));
}

// -----------------------------------------------------------------------------
// Receive one dnf5daemon progress signal and convert it to a user-facing line.
// -----------------------------------------------------------------------------
void
on_transaction_progress_signal(GDBusConnection *connection,
                               const gchar *,
                               const gchar *,
                               const gchar *,
                               const gchar *signal_name,
                               GVariant *parameters,
                               gpointer user_data)
{
  auto *forwarder = static_cast<TransactionServiceProgressForwarder *>(user_data);
  if (!signal_matches_transaction(forwarder, parameters) || !signal_name) {
    return;
  }

  const std::string signal = signal_name;

  if (signal == "repo_key_import_request") {
    handle_key_import_request(connection, forwarder, parameters);
    return;
  }

  if (signal == "download_add_new" && !forwarder->downloads_started) {
    forwarder->downloads_started = true;
    forward_progress_line(forwarder, _("Starting package downloads..."));
  }

  if (signal == "download_add_new") {
    std::string download_id = variant_child_text(parameters, 1);
    std::string description = variant_child_text(parameters, 2);
    if (!download_id.empty() && !description.empty()) {
      forwarder->download_description_by_id[download_id] = description;
    }
    if (!description.empty()) {
      forward_progress_line(forwarder, std::string(_("Downloading: ")) + description);
    }
    return;
  }

  if (signal == "download_progress") {
    std::string download_id = variant_child_text(parameters, 1);
    uint64_t total = variant_child_uint64(parameters, 2);
    uint64_t downloaded = variant_child_uint64(parameters, 3);
    if (download_id.empty() || total == 0) {
      return;
    }

    int percent = static_cast<int>((downloaded * 100) / total);
    percent = std::clamp(percent, 0, 100);
    int bucket = percent / 10;

    // Report progress once per 10% range so large transactions do not flood the GTK progress window.
    auto [it, inserted] = forwarder->download_bucket_by_id.emplace(download_id, -1);
    if (!inserted && it->second == bucket) {
      return;
    }

    it->second = bucket;
    std::string description = download_description(forwarder, download_id);
    forward_progress_line(forwarder,
                          std::string(_("Download progress: ")) +
                              (description.empty() ? _("package download") : description) + " (" +
                              std::to_string(percent) + "%)");
    return;
  }

  if (signal == "download_mirror_failure") {
    std::string message = variant_child_text(parameters, 2);
    forward_progress_line(forwarder, message.empty() ? _("Download mirror failed.") : message);
    return;
  }

  if (signal == "download_end") {
    std::string download_id = variant_child_text(parameters, 1);
    uint64_t status = variant_child_uint64(parameters, 2);
    if (status == 2) {
      std::string message = variant_child_text(parameters, 3);
      forward_progress_line(forwarder, message.empty() ? _("Package download failed.") : message);
    } else if (!download_id.empty()) {
      std::string description = download_description(forwarder, download_id);
      if (!description.empty()) {
        forward_progress_line(forwarder, std::string(_("Download ready: ")) + description);
      }
    }
    return;
  }

  if (signal == "transaction_before_begin" && !forwarder->transaction_started) {
    forwarder->transaction_started = true;
    forward_progress_line(forwarder, _("Running transaction."));
    return;
  }

  if (signal == "transaction_verify_start" && !forwarder->verify_started) {
    forwarder->verify_started = true;
    forward_progress_line(forwarder, _("Verifying package files."));
    return;
  }

  if (signal == "transaction_transaction_start" && !forwarder->prepare_started) {
    forwarder->prepare_started = true;
    forward_progress_line(forwarder, _("Preparing transaction."));
    return;
  }

  if (signal == "transaction_action_start") {
    std::string nevra = variant_child_text(parameters, 1);
    forward_progress_line(forwarder,
                          nevra.empty() ? _("Processing package.") : std::string(_("Processing package: ")) + nevra);
    return;
  }

  if (signal == "transaction_unpack_error") {
    std::string nevra = variant_child_text(parameters, 1);
    forward_progress_line(
        forwarder, nevra.empty() ? _("Package unpack failed.") : std::string(_("Package unpack failed: ")) + nevra);
  }
}

} // namespace

// -----------------------------------------------------------------------------
// Resolve a started daemon session and read its structured preview.
// The caller opened the session and is responsible for closing it on failure.
// -----------------------------------------------------------------------------
bool
transaction_service_client_wait_for_started_transaction_preview(GDBusConnection *connection,
                                                                const std::string &transaction_path,
                                                                TransactionServiceProgressForwarder *progress_forwarder,
                                                                GCancellable *cancellable,
                                                                TransactionPreview &preview_out,
                                                                std::string &error_out,
                                                                std::vector<std::string> *upgrade_keys_out)
{
  if (transaction_service_client_get_transaction_preview(
          connection, transaction_path, progress_forwarder, cancellable, preview_out, error_out, upgrade_keys_out)) {
    return true;
  }

  DNFUI_TRACE("dnf5daemon preview failed path=%s error=%s", transaction_path.c_str(), error_out.c_str());
  return false;
}

// -----------------------------------------------------------------------------
// Subscribe to dnf5daemon signals for one transaction session.
// -----------------------------------------------------------------------------
guint
transaction_service_client_subscribe_progress(GDBusConnection *connection,
                                              const std::string &transaction_path,
                                              TransactionServiceProgressForwarder *progress_forwarder)
{
  if (progress_forwarder) {
    progress_forwarder->transaction_path = transaction_path;
  }

  return g_dbus_connection_signal_subscribe(connection,
                                            kDnfDaemonName,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            G_DBUS_SIGNAL_FLAGS_NONE,
                                            on_transaction_progress_signal,
                                            progress_forwarder,
                                            nullptr);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
