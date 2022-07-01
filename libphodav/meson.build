headers = [
  'phodav.h',
  'phodav-server.h',
  'phodav-virtual-dir.h',
]

install_headers(headers, subdir : 'libphodav-3.0/libphodav')

sources = [
  'phodav-if.c',
  'phodav-lock.c',
  'phodav-method-delete.c',
  'phodav-method-get.c',
  'phodav-method-lock.c',
  'phodav-method-mkcol.c',
  'phodav-method-movecopy.c',
  'phodav-method-propfind.c',
  'phodav-method-proppatch.c',
  'phodav-method-put.c',
  'phodav-method-unlock.c',
  'phodav-multistatus.c',
  'phodav-path.c',
  'phodav-server.c',
  'phodav-utils.c',
  'phodav-virtual-dir.c'
]

if not dependency('glib-2.0', version : '>= 2.51.2', required: false).found()
  sources += 'guuid.c'
endif

mapfile = 'libphodav.syms'
vflag = compiler.get_supported_link_arguments([
  '-Wl,--version-script,@0@/@1@'.format(meson.current_source_dir(), mapfile),
])

libphodav = library(
  'phodav-3.0',
  sources,
  c_args : [ '-DG_LOG_DOMAIN="phodav"' ],
  include_directories : incdir,
  version: '0.0.0',
  link_args : compiler.get_supported_link_arguments([ '-Wl,--no-undefined', vflag ]),
  link_depends : mapfile,
  dependencies : deps,
  install : true,
)

pkgconfig = import('pkgconfig')
pkgconfig.generate(
  libphodav,
  filebase : 'libphodav-3.0',
  description : 'A WebDAV library based on libsoup',
  subdirs : 'libphodav-3.0',
  requires : 'libsoup-3.0'
)
