project(
  'dnf-ui',
  'cpp',
  version: '0.1.5',
  default_options: [
    'cpp_std=c++20',
    'warning_level=0',
    'werror=true',
  ],
)

add_project_arguments('-fdiagnostics-color=always', language: 'cpp')
add_project_arguments('-DDNFUI_VERSION="@0@"'.format(meson.project_version()), language: 'cpp')
add_project_arguments('-DGETTEXT_PACKAGE="@0@"'.format(meson.project_name()), language: 'cpp')
add_project_arguments(
  '-DLOCALEDIR="@0@"'.format(join_paths(get_option('prefix'), get_option('localedir'))),
  language: 'cpp',
)

if get_option('final_build')
  add_project_arguments(
    [
      '-DNDEBUG_BUILD',
      '-Wpedantic',
      '-Wmaybe-uninitialized',
      '-W',
      '-Wformat=2',
      '-Wpointer-arith',
      '-Wdisabled-optimization',
      '-Wfloat-equal',
    ],
    language: 'cpp',
  )
else
  add_project_arguments('-DDEBUG_BUILD', language: 'cpp')
endif

if get_option('debug_trace')
  add_project_arguments('-DDNFUI_DEBUG_TRACE', language: 'cpp')
endif

src_inc = include_directories('src')

libdnf5_dep = dependency('libdnf5')
gtk4_dep = dependency('gtk4')
polkit_dep = dependency('polkit-gobject-1')
systemd_dep = dependency('systemd', required: false)

polkit_action_dir = join_paths(get_option('datadir'), 'polkit-1', 'actions')
dbus_system_service_dir = join_paths(get_option('datadir'), 'dbus-1', 'system-services')
dbus_system_policy_dir = join_paths(get_option('sysconfdir'), 'dbus-1', 'system.d')
systemd_system_unit_dir = join_paths(get_option('prefix'), 'lib', 'systemd', 'system')
desktop_file_dir = join_paths(get_option('datadir'), 'applications')
metainfo_dir = join_paths(get_option('datadir'), 'metainfo')
app_icon_root_dir = join_paths(get_option('datadir'), 'icons', 'hicolor')

if systemd_dep.found()
  systemd_system_unit_dir = systemd_dep.get_variable(pkgconfig: 'systemdsystemunitdir')
endif

subdir('src')
subdir('packaging')
subdir('po')

if get_option('build_tests')
  subdir('test')
endif
