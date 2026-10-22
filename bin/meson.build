win32_deps = []
if host_machine.system() == 'windows'
  win32_deps += compiler.find_library('netapi32')
  win32_deps += compiler.find_library('mpr')
endif

avahi_common = []
if conf.has('WITH_AVAHI')
  avahi_common += files('avahi-common.c')
endif

output_queue_sources = files('output-queue.c', 'output-queue.h')
spice_mux_sources = files('spice-mux.c', 'spice-mux.h')
bin_inc = include_directories('.')

spice_webdavd_sources = files('spice-webdavd.c') + spice_mux_sources + output_queue_sources
if host_machine.system() == 'windows'
  spice_webdavd_sources += files(
    'drive-mapping.c',
    'drive-mapping.h',
    'win32-service.c',
    'win32-service.h',
  )
endif

spice_webdavd = executable(
  'spice-webdavd',
  spice_webdavd_sources + avahi_common,
  install_dir : sbindir,
  include_directories : incdir,
  dependencies : win32_deps + avahi_deps + deps,
  link_with : [ libphodav ],
  install : true,
)

chezdav = executable(
  'chezdav',
  [ 'chezdav.c' ] + avahi_common,
  include_directories : incdir,
  dependencies : avahi_deps + deps,
  link_with : [ libphodav ],
  install : true,
)
