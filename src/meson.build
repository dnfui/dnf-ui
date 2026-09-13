backend_sources = files(
  'dnf_backend/base_manager.cpp',
  'dnf_backend/dnf_common.cpp',
  'dnf_backend/dnf_details.cpp',
  'dnf_backend/dnf_history.cpp',
  'dnf_backend/dnf_query.cpp',
  'dnf_backend/dnf_state.cpp',
)

daemon_client_sources = files(
  'dnf5daemon_client/transaction_service_client_dbus.cpp',
  'dnf5daemon_client/transaction_service_client.cpp',
  'dnf5daemon_client/transaction_service_client_wait.cpp',
)

repository_client_sources = files(
  'dnf5daemon_client/repository_service_client.cpp',
)

app_sources = files(
  'app.cpp',
  'config.cpp',
  'i18n.cpp',
  'ui/window/main_menu.cpp',
  'ui/window/main_window.cpp',
  'ui/window/main_window_layout.cpp',
  'main.cpp',
  'ui/history/transaction_history_view.cpp',
  'ui/transaction/pending_transaction_action_rows.cpp',
  'ui/details/package_details_controller.cpp',
  'ui/package_query/package_query_cache.cpp',
  'ui/package_query/package_query_controls.cpp',
  'ui/package_query/package_query_controller.cpp',
  'ui/package_query/package_query_tasks.cpp',
  'ui/package_table/package_table_model.cpp',
  'ui/package_table/package_table_context_menu.cpp',
  'ui/package_table/package_table_columns.cpp',
  'ui/package_table/package_table_export.cpp',
  'ui/package_table/package_table_export_csv.cpp',
  'ui/package_table/package_table_sort.cpp',
  'ui/package_table/package_table_status.cpp',
  'ui/package_table/package_table_view.cpp',
  'ui/repository/repository_apply_model.cpp',
  'ui/repository/repository_view.cpp',
  'ui/transaction/pending_transaction_apply.cpp',
  'ui/transaction/offline_transaction_view.cpp',
  'ui/transaction/pending_transaction_controller.cpp',
  'ui/transaction/pending_transaction_request.cpp',
  'ui/transaction/pending_transaction_view.cpp',
  'ui/refresh/repository_refresh_controller.cpp',
  'ui/transaction/transaction_progress.cpp',
  'ui/transaction/transaction_dialogs.cpp',
  'upgrade/daemon_upgrade_state.cpp',
  'ui/common/ui_helpers.cpp',
  'ui/common/widgets.cpp',
)

dnfui_app = executable(
  'dnfui',
  backend_sources + daemon_client_sources + repository_client_sources + app_sources,
  include_directories: src_inc,
  dependencies: [
    libdnf5_dep,
    gtk4_dep,
  ],
  install: true,
  install_tag: 'runtime',
)

gio_dep = dependency('gio-2.0')

dnfui_backend_cli = executable(
  'dnfui-backend-cli',
  backend_sources + daemon_client_sources + repository_client_sources + files(
    'cli/backend_cli.cpp',
    'i18n.cpp',
  ),
  include_directories: src_inc,
  dependencies: [
    libdnf5_dep,
    gio_dep,
  ],
  build_by_default: false,
  install: false,
)
