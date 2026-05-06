service_sources = files(
  '../i18n.cpp',
  'transaction_service.cpp',
  'transaction_service_authorization.cpp',
  'transaction_service_format.cpp',
  'transaction_service_introspection.cpp',
  'transaction_service_limits.cpp',
  'transaction_service_manager.cpp',
  'transaction_service_preview_formatter.cpp',
  'transaction_service_request_objects.cpp',
  'transaction_service_request_parser.cpp',
  'transaction_service_signals.cpp',
  'transaction_service_workers.cpp',
  'transaction_service_main.cpp',
)

service_cpp_args = []
if get_option('build_tests')
  service_cpp_args += ['-DDNFUI_BUILD_TESTS=1']
endif

dnfui_service = executable(
  'dnfui-service',
  backend_sources + service_sources,
  include_directories: src_inc,
  cpp_args: service_cpp_args,
  dependencies: [
    libdnf5_dep,
    polkit_dep,
  ],
  install: true,
  install_dir: get_option('libexecdir'),
  install_tag: 'transaction-service',
)
