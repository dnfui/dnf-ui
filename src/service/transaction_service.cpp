// -----------------------------------------------------------------------------
// transaction_service.cpp
// Owns the transaction service process runtime.
// Registers the manager object on D-Bus, runs the main loop, and releases
// service state during shutdown.
// -----------------------------------------------------------------------------
#include "transaction_service.hpp"

#include "transaction_service_internal.hpp"

#include "debug_trace.hpp"
#include "i18n.hpp"
#include "service/transaction_service_introspection.hpp"

#include <glib-unix.h>

#include <cstdio>
#include <memory>

// -----------------------------------------------------------------------------
// Main loop and bus callbacks
// -----------------------------------------------------------------------------
// Stop the service main loop when the process receives a quit signal.
// -----------------------------------------------------------------------------
static gboolean
on_quit_signal(gpointer user_data)
{
  GMainLoop *loop = static_cast<GMainLoop *>(user_data);
  if (loop) {
    g_main_loop_quit(loop);
  }
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Register the manager object after the service acquires its D-Bus name.
// -----------------------------------------------------------------------------
static void
on_bus_acquired(GDBusConnection *connection, const gchar *, gpointer user_data)
{
  TransactionService *service = static_cast<TransactionService *>(user_data);
  if (!service) {
    return;
  }

  service->connection = G_DBUS_CONNECTION(g_object_ref(connection));
  GError *error = nullptr;
  service->manager_registration_id = g_dbus_connection_register_object(service->connection,
                                                                       kManagerObjectPath,
                                                                       service->manager_node_info->interfaces[0],
                                                                       &kManagerVTable,
                                                                       service,
                                                                       nullptr,
                                                                       &error);

  if (service->manager_registration_id == 0) {
    std::fputs(dnfui_i18n_format(_("Failed to register transaction service object: %s\n"),
                                 error ? error->message : _("unknown"))
                   .c_str(),
               stderr);
    g_clear_error(&error);
    if (service->loop) {
      g_main_loop_quit(service->loop);
    }
    return;
  }

  DNFUI_TRACE("Transaction service bus ready");
}

// -----------------------------------------------------------------------------
// Stop the service if it loses its D-Bus name.
// -----------------------------------------------------------------------------
static void
on_name_lost(GDBusConnection *, const gchar *, gpointer user_data)
{
  TransactionService *service = static_cast<TransactionService *>(user_data);
  DNFUI_TRACE("Transaction service name lost");
  if (service && service->loop) {
    g_main_loop_quit(service->loop);
  }
}

// -----------------------------------------------------------------------------
// Service cleanup
// -----------------------------------------------------------------------------
// Unregister service objects and release GLib resources.
// Pending authorization requests are answered before sessions are destroyed.
// -----------------------------------------------------------------------------
static void
cleanup_service(TransactionService &service)
{
  // Signal that shutdown is in progress to prevent async callbacks from accessing freed memory.
  service.shutting_down = true;
  bool keep_alive_until_exit = false;

  // Reply to any pending authorization requests with an error before destroying sessions.
  for (auto &[path, session] : service.transactions) {
    GDBusMethodInvocation *pending_apply_invocation = nullptr;
    {
      std::lock_guard<std::mutex> lock(session->state_mutex);
      pending_apply_invocation = session->pending_apply_invocation;
      session->pending_apply_invocation = nullptr;
    }

    if (pending_apply_invocation) {
      keep_alive_until_exit = true;
      DNFUI_TRACE("Transaction service cancelling pending authorization during shutdown path=%s", path.c_str());
      g_dbus_method_invocation_return_error(pending_apply_invocation,
                                            G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "%s",
                                            _("Transaction service is shutting down."));
      g_object_unref(pending_apply_invocation);
    }

    if (!session->finished.load()) {
      keep_alive_until_exit = true;
      session->cancelled = true;
    }
  }

  for (auto &[path, session] : service.transactions) {
    if (session->owner_watch_id != 0) {
      g_bus_unwatch_name(session->owner_watch_id);
      session->owner_watch_id = 0;
    }

    if (service.connection && session->registration_id != 0) {
      g_dbus_connection_unregister_object(service.connection, session->registration_id);
    }
  }

  // Detached worker threads and authorization callbacks still use raw service
  // or session pointers. During shutdown, keep that state allocated until
  // process exit instead of freeing it during teardown.
  if (keep_alive_until_exit) {
    service.keep_alive_until_exit = true;
  } else {
    service.transactions.clear();
  }

  if (service.connection && service.manager_registration_id != 0) {
    g_dbus_connection_unregister_object(service.connection, service.manager_registration_id);
  }

  if (service.owner_id != 0) {
    g_bus_unown_name(service.owner_id);
  }

  if (service.connection) {
    g_object_unref(service.connection);
    service.connection = nullptr;
  }

  if (service.manager_node_info) {
    g_dbus_node_info_unref(service.manager_node_info);
    service.manager_node_info = nullptr;
  }

  if (service.transaction_node_info) {
    g_dbus_node_info_unref(service.transaction_node_info);
    service.transaction_node_info = nullptr;
  }

  if (service.loop) {
    g_main_loop_unref(service.loop);
    service.loop = nullptr;
  }
}

// -----------------------------------------------------------------------------
// Transaction service entrypoint
// -----------------------------------------------------------------------------
// Build the service runtime state, own the bus name, and run the main loop.
// -----------------------------------------------------------------------------
int
transaction_service_run(const TransactionServiceOptions &options)
{
  auto service = std::make_unique<TransactionService>();
  service->bus_type = options.bus_type;
  service->loop = g_main_loop_new(nullptr, FALSE);
  service->main_context = g_main_loop_get_context(service->loop);

  GError *error = nullptr;
  service->manager_node_info = g_dbus_node_info_new_for_xml(kTransactionServiceManagerIntrospectionXml, &error);
  if (!service->manager_node_info) {
    std::fputs(
        dnfui_i18n_format(_("Failed to parse manager introspection XML: %s\n"), error ? error->message : _("unknown"))
            .c_str(),
        stderr);
    g_clear_error(&error);
    cleanup_service(*service);
    return 1;
  }

  service->transaction_node_info = g_dbus_node_info_new_for_xml(kTransactionServiceRequestIntrospectionXml, &error);
  if (!service->transaction_node_info) {
    std::fputs(dnfui_i18n_format(_("Failed to parse transaction introspection XML: %s\n"),
                                 error ? error->message : _("unknown"))
                   .c_str(),
               stderr);
    g_clear_error(&error);
    cleanup_service(*service);
    return 1;
  }

  service->owner_id = g_bus_own_name(options.bus_type,
                                     kServiceName,
                                     G_BUS_NAME_OWNER_FLAGS_NONE,
                                     on_bus_acquired,
                                     nullptr,
                                     on_name_lost,
                                     service.get(),
                                     nullptr);

  g_unix_signal_add(SIGINT, on_quit_signal, service->loop);
  g_unix_signal_add(SIGTERM, on_quit_signal, service->loop);

  DNFUI_TRACE("Transaction service run loop start");
  g_main_loop_run(service->loop);
  DNFUI_TRACE("Transaction service run loop stop");

  cleanup_service(*service);
  if (service->keep_alive_until_exit) {
    (void)service.release();
  }
  return 0;
}
