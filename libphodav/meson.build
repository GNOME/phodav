headers = [
  'phodav.h',
  'phodav-server.h',
  'phodav-virtual-dir.h',
]

install_headers(headers, subdir : 'libphodav-3.0/libphodav')

sources = [
  'phodav-backend.c',
  'phodav-backend-gfile.c',
  'phodav-backend-native.c',
  'phodav-backend-virtual.c',
  'phodav-if.c',
  'phodav-lock.c',
  'phodav-lock-store.c',
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
  'phodav-request.c',
  'phodav-server.c',
  'phodav-utils.c',
  'phodav-virtual-dir.c',
  'phodav-xattr.c'
]

# the same list, as file objects usable from the other
# subdirectories, for the fuzzing build in tests/fuzz
libphodav_sources = files(sources)

libphodav_inc = include_directories('.')

# the private symbols are needed by the unit tests, so build them as
# a static library which is included as a whole in the shared one
libphodav_internals = static_library(
  'phodav-internal',
  sources,
  c_args : [ '-DG_LOG_DOMAIN="phodav"' ],
  include_directories : incdir,
  dependencies : deps,
)

symbol_map = 'libphodav.syms'
version_script = meson.current_source_dir() / symbol_map

libphodav = library(
  'phodav-3.0',
  link_whole : libphodav_internals,
  include_directories : incdir,
  # libtool-style ABI version current.revision.age: bump current and age
  # when adding symbols, revision otherwise; soversion (current - age)
  # must only change on incompatible ABI changes
  soversion : 0,
  version : '0.0.0',
  link_args : compiler.get_supported_link_arguments(
    [ '-Wl,--no-undefined', f'-Wl,--version-script,@version_script@' ]),
  link_depends : symbol_map,
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

build_gir = false
gir_scanner = find_program('g-ir-scanner', required : get_option('introspection'))
if gir_scanner.found()
  if meson.is_cross_build()
    if get_option('introspection').enabled()
      error('gobject-introspection requires a native build')
    endif
    message('Skipping gobject-introspection when cross-compiling')
  else
    build_gir = true
    libphodav_gir = gnome.generate_gir(libphodav,
      sources : [ headers, 'phodav-server.c', 'phodav-virtual-dir.c' ],
      namespace : 'Phodav',
      nsversion : '3.0',
      includes : [ 'Gio-2.0', 'Soup-3.0' ],
      export_packages : 'libphodav-3.0',
      install : true)
  endif
endif
