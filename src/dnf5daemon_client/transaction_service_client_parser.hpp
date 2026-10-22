// -----------------------------------------------------------------------------
// transaction_service_client_parser.hpp
// Private parsers for dnf5daemon transaction and package-list items.
// -----------------------------------------------------------------------------
#pragma once

#include <gio/gio.h>

#include <string>

struct DaemonUpgradeTarget;
struct TransactionPreview;

bool transaction_service_client_append_daemon_preview_item(TransactionPreview &preview,
                                                           const std::string &object_type,
                                                           const std::string &action,
                                                           GVariant *attributes,
                                                           GVariant *object,
                                                           std::string &error_out);

bool transaction_service_client_upgrade_target_from_daemon_object(GVariant *object,
                                                                  DaemonUpgradeTarget &target_out,
                                                                  std::string &error_out);
