backend_sources = files(
  'base_manager.cpp',
  'dnf_backend/dnf_common.cpp',
  'dnf_backend/dnf_details.cpp',
  'dnf_backend/dnf_query.cpp',
  'dnf_backend/dnf_state.cpp',
  'dnf_backend/dnf_transaction.cpp',
)

app_sources = files(
  'app.cpp',
  'config.cpp',
  'i18n.cpp',
  'ui/main_menu.cpp',
  'ui/main_window.cpp',
  'main.cpp',
  'ui/package_action_rows.cpp',
  'ui/package_info_controller.cpp',
  'ui/package_query_cache.cpp',
  'ui/package_query_controller.cpp',
  'ui/package_table_context_menu.cpp',
  'ui/package_table_status.cpp',
  'ui/package_table_view.cpp',
  'ui/pending_transaction_controller.cpp',
  'ui/pending_transaction_request.cpp',
  'ui/transaction_progress.cpp',
  'ui/transaction_review_dialog.cpp',
  'transaction_service_client.cpp',
  'ui/ui_helpers.cpp',
  'ui/widgets.cpp',
)

dnfui_app = executable(
  'dnfui',
  backend_sources + app_sources,
  include_directories: src_inc,
  dependencies: [
    libdnf5_dep,
    gtk4_dep,
    polkit_dep,
  ],
  install: true,
  install_tag: 'runtime',
)

subdir('service')
