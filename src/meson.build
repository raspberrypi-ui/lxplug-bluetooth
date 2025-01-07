gtk = dependency('gtk+-3.0')
gtkmm = dependency('gtkmm-3.0', version: '>=3.24')

lsources = files(
  'bluetooth.c'
)

ldeps = [ gtk ]

lincdir = include_directories('/usr/include/lxpanel')

largs = [ '-DLXPLUG', '-DPACKAGE_DATA_DIR="' + lresource_dir + '"' ]

shared_module('bluetooth', lsources,
        dependencies: ldeps,
        install: true,
        install_dir: get_option('libdir') / 'lxpanel/plugins',
        c_args : largs,
        include_directories : lincdir,
        name_prefix: ''
)

wsources = files(
  'bluetooth.cpp',
  'bluetooth.c'
)

wdeps = [ gtkmm ]

wincdir = include_directories('/usr/include/wf-panel-pi')

wargs = [ '-DPLUGIN_NAME="bluetooth"', '-DPACKAGE_DATA_DIR="' + wresource_dir + '"' ]

shared_module('libbluetooth', wsources,
        dependencies: wdeps,
        install: true,
        install_dir: get_option('libdir') / 'wf-panel-pi',
        c_args : wargs,
        cpp_args : wargs,
        include_directories : wincdir,
        name_prefix: ''
)

metadata = files(
)
install_data(metadata, install_dir: metadata_dir)
