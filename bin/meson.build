win32_deps = []
if host_machine.system() == 'windows'
  win32_deps += compiler.find_library('netapi32')
  win32_deps += compiler.find_library('mpr')
endif

avahi_common = []
if conf.has('WITH_AVAHI')
  avahi_common += files('avahi-common.c')
endif

sources = [
  'spice-webdavd.c',
  'output-queue.c',
  'output-queue.h'
]

executable(
  'spice-webdavd',
  sources + avahi_common,
  install_dir : sbindir,
  include_directories : incdir,
  dependencies : win32_deps + avahi_deps + deps,
  link_with : [ libphodav ],
  install : true,
)

executable(
  'chezdav',
  [ 'chezdav.c' ] + avahi_common,
  include_directories : incdir,
  dependencies : avahi_deps + deps,
  link_with : [ libphodav ],
  install : true,
)
