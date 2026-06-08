i18n = import('i18n')

i18n.merge_file(
  input: 'com.fedora.dnfui.desktop.in',
  output: 'com.fedora.dnfui.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: desktop_file_dir,
  install_tag: 'runtime',
)

i18n.merge_file(
  input: 'com.fedora.dnfui.metainfo.xml.in',
  output: 'com.fedora.dnfui.metainfo.xml',
  type: 'xml',
  po_dir: '../po',
  install: true,
  install_dir: metainfo_dir,
  install_tag: 'runtime',
)

app_icon_sizes = ['48', '64', '128', '256', '512', '1024']

foreach app_icon_size : app_icon_sizes
  app_icon_size_dir = app_icon_size + 'x' + app_icon_size

  install_data(
    join_paths('icons', 'hicolor', app_icon_size_dir, 'apps', 'com.fedora.dnfui.png'),
    install_dir: join_paths(app_icon_root_dir, app_icon_size_dir, 'apps'),
    install_tag: 'runtime',
  )
endforeach
