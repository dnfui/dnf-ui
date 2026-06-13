project(
  'dnf-ui',
  'cpp',
  version: '0.2.1',
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

desktop_file_dir = join_paths(get_option('datadir'), 'applications')
metainfo_dir = join_paths(get_option('datadir'), 'metainfo')
app_icon_root_dir = join_paths(get_option('datadir'), 'icons', 'hicolor')

subdir('src')
subdir('packaging')
subdir('po')

if get_option('build_tests')
  subdir('test')
endif
