# Fedora ships Catch2 v3 as the catch-devel package, which provides
# pkgconfig(catch2-with-main).
catch2_dep = dependency('catch2-with-main')
gio_dep = dependency('gio-2.0')

dnfui_service_smoke_client = executable(
  'dnfui-service-smoke-client',
  files(
    'transaction_service_system_bus_client.cpp',
    '../src/i18n.cpp',
    '../src/transaction_service_client_dbus.cpp',
    '../src/transaction_service_client.cpp',
    '../src/transaction_service_client_wait.cpp',
  ),
  include_directories: src_inc,
  dependencies: [
    libdnf5_dep,
    gio_dep,
  ],
  cpp_args: ['-DDNFUI_BUILD_TESTS=1'],
)

dnfui_tests = executable(
  'dnfui-tests',
  files(
    'unit/test_backend.cpp',
    'unit/test_config.cpp',
    'unit/test_offline.cpp',
    'unit/test_package_action_rows.cpp',
    'unit/test_package_table_status.cpp',
    'unit/test_package_table_sort.cpp',
    'unit/test_package_query_cache.cpp',
    'unit/test_pending_transaction_request.cpp',
    'unit/test_search.cpp',
    'unit/test_transaction_service_client.cpp',
    'unit/test_transaction_service_preview_formatter.cpp',
    'unit/test_transaction_service_request_parser.cpp',
    'unit/test_transaction_service_validation.cpp',
    'unit/test_transaction_preview.cpp',
    'unit/test_transaction_request.cpp',
  ) + backend_sources + files(
    '../src/i18n.cpp',
    '../src/config.cpp',
    '../src/service/transaction_service_preview_authorization.cpp',
    '../src/service/transaction_service_preview_formatter.cpp',
    '../src/service/transaction_service_request_parser.cpp',
    '../src/service/transaction_service_validation.cpp',
    '../src/transaction_service_client_dbus.cpp',
    '../src/transaction_service_client.cpp',
    '../src/transaction_service_client_wait.cpp',
    '../src/ui/package_action_rows.cpp',
    '../src/ui/package_table_model.cpp',
    '../src/ui/package_table_sort.cpp',
    '../src/ui/package_table_status.cpp',
    '../src/ui/package_query_cache.cpp',
    '../src/ui/pending_transaction_request.cpp',
  ),
  include_directories: src_inc,
  dependencies: [
    libdnf5_dep,
    gio_dep,
    gtk4_dep,
    polkit_dep,
    catch2_dep,
  ],
  cpp_args: [
    '-DDNFUI_BUILD_TESTS=1',
    '-DDNFUI_TEST_SERVICE_BIN="@0@"'.format(dnfui_service_tests.full_path()),
  ],
)

test('dnfui-tests', dnfui_tests, timeout: 120)
