// -----------------------------------------------------------------------------
// src/tui/transaction_apply.cpp
// -----------------------------------------------------------------------------
#include "transaction_apply.hpp"

#include "dnf5daemon_client/transaction_service_client.hpp"
#include "dnf5daemon_client/transaction_service_client_internal.hpp"
#include "dnf_backend/base_manager.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <gio/gio.h>

#include <exception>
#include <iostream>

namespace tui {
namespace {

  // A private connection lets cancellation abandon a read-only session request
  // without closing the connection that owns an approved transaction.
  struct StatusConnectionCloser {
    void operator()(GDBusConnection *connection) const noexcept
    {
      g_dbus_connection_close_sync(connection, nullptr, nullptr);
      g_object_unref(connection);
    }
  };

  bool read_offline_status(bool &prepared, std::string &error, GCancellable *cancel)
  {
    GError *bus_error = nullptr;
    std::unique_ptr<gchar, decltype(&g_free)> address(
        g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, cancel, &bus_error), g_free);
    if (!address) {
      error = bus_error ? bus_error->message : "Could not locate the system bus.";
      g_clear_error(&bus_error);
      return false;
    }
    std::unique_ptr<GDBusConnection, StatusConnectionCloser> connection(g_dbus_connection_new_for_address_sync(
        address.get(),
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr,
        cancel,
        &bus_error));
    if (!connection) {
      error = bus_error ? bus_error->message : "Could not connect to the system bus.";
      g_clear_error(&bus_error);
      return false;
    }

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "load_available_repos", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&options, "{sv}", "load_system_repo", g_variant_new_boolean(FALSE));
    std::unique_ptr<GVariant, decltype(&g_variant_unref)> reply(
        g_dbus_connection_call_sync(connection.get(),
                                    "org.rpm.dnf.v0",
                                    "/org/rpm/dnf/v0",
                                    "org.rpm.dnf.v0.SessionManager",
                                    "open_session",
                                    g_variant_new("(@a{sv})", g_variant_builder_end(&options)),
                                    G_VARIANT_TYPE("(o)"),
                                    G_DBUS_CALL_FLAGS_NONE,
                                    -1,
                                    cancel,
                                    &bus_error),
        g_variant_unref);
    if (!reply) {
      error = bus_error ? bus_error->message : "Could not open a status session.";
      g_clear_error(&bus_error);
      return false;
    }

    const gchar *path = nullptr;
    g_variant_get(reply.get(), "(&o)", &path);
    const bool ok = transaction_service_client_has_prepared_offline_updates_request(
        connection.get(), path, prepared, error, cancel);
    // On cancellation the private connection is closed instead of waiting for
    // another daemon reply. The daemon releases sessions owned by disconnected clients.
    if (!g_cancellable_is_cancelled(cancel)) {
      GVariant *closed = g_dbus_connection_call_sync(connection.get(),
                                                     "org.rpm.dnf.v0",
                                                     "/org/rpm/dnf/v0",
                                                     "org.rpm.dnf.v0.SessionManager",
                                                     "close_session",
                                                     g_variant_new("(o)", path),
                                                     G_VARIANT_TYPE("(b)"),
                                                     G_DBUS_CALL_FLAGS_NONE,
                                                     -1,
                                                     cancel,
                                                     nullptr);
      if (closed) {
        g_variant_unref(closed);
      }
    }
    return ok && !g_cancellable_is_cancelled(cancel);
  }

} // namespace

PreparedTransaction::~PreparedTransaction()
{
  transaction_service_client_release_request(session_path);
}

std::string &
PreparedTransaction::path_for_resolution()
{
  return session_path;
}

void
PreparedTransaction::set_offline_mode(bool value)
{
  offline = value;
}

const std::string &
PreparedTransaction::path() const
{
  return session_path;
}

bool
PreparedTransaction::requires_offline() const
{
  return offline;
}

void
TransactionProgress::set_message(const std::string &value)
{
  std::lock_guard<std::mutex> lock(mutex);
  message = value;
}

void
TransactionProgress::set_count(bool determinate, uint64_t processed, uint64_t total)
{
  std::lock_guard<std::mutex> lock(mutex);
  count = determinate && total > 0 ? std::to_string(processed) + " of " + std::to_string(total) + "  " : "";
}

std::string
TransactionProgress::text() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return count + message;
}

OfflineState
query_offline_state(GCancellable *cancel)
{
  if (g_cancellable_is_cancelled(cancel)) {
    return OfflineState::UNKNOWN;
  }
  bool prepared = false;
  std::string error;
  try {
    if (read_offline_status(prepared, error, cancel)) {
      return prepared ? OfflineState::PREPARED : OfflineState::NOT_PREPARED;
    }
  } catch (const std::exception &exception) {
    error = exception.what();
  }
  if (!error.empty() && !g_cancellable_is_cancelled(cancel)) {
    std::cerr << "Could not check prepared offline updates: " << error << '\n';
  }
  return OfflineState::UNKNOWN;
}

const char *
offline_recovery_hint()
{
  return "Use dnf5 offline status to inspect stored changes. "
         "Restart to install prepared updates, or use sudo dnf5 offline clean to discard them.";
}

ApplyResult
apply_transaction(std::unique_ptr<PreparedTransaction> prepared, TransactionProgress &progress)
{
  ApplyResult result;
  if (!prepared || prepared->path().empty()) {
    result.message = "No approved transaction session.";
    return result;
  }

  result.offline = prepared->requires_offline();
  std::string error;
  bool transaction_started = false;
  bool key_requested = false;
  try {
    result.success = transaction_service_client_apply_started_request(
        prepared->path(),
        [&progress](const std::string &message) { progress.set_message(message); },
        [&progress](const TransactionApplyProgress &update) {
          progress.set_count(update.determinate, update.processed, update.total);
        },
        [&key_requested](const TransactionKeyImportRequest &) {
          key_requested = true;
          return false;
        },
        error,
        transaction_started,
        nullptr,
        result.offline);
  } catch (const std::exception &exception) {
    error = exception.what();
  }

  prepared.reset();
  // An unsuccessful reply does not prove that the RPM transaction never started.
  DaemonUpgradeState::instance().mark_stale();
  BaseManager::instance().drop_cached_base();
  result.offline_state = query_offline_state(nullptr);

  if (result.success) {
    result.message = result.offline ? "Changes prepared for the next reboot. No packages have been changed yet.\n\n"
                                      "Restart when ready. Use dnf5 offline status to inspect the prepared transaction."
                                    : "Transaction applied successfully.";
  } else {
    result.message = error.empty() ? "Transaction did not complete successfully." : error;
    if (key_requested) {
      result.message +=
          "\n\nA repository signing key was declined. Key import is not supported in the terminal frontend.";
    }
    if (result.offline) {
      result.message +=
          "\n\nPreparation may have left a stored transaction. Check dnf5 offline status before retrying. "
          "Use sudo dnf5 offline clean to discard prepared changes if needed.";
    } else {
      result.message += "\n\nInstalled packages may have changed. Pending actions were cleared. "
                        "Review the refreshed package list before trying again.";
      if (result.offline_state == OfflineState::PREPARED) {
        result.message += std::string("\n\n") + offline_recovery_hint();
      }
    }
  }
  return result;
}

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
