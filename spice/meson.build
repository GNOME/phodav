win32_deps = []
if host_machine.system() == 'windows'
  win32_deps += compiler.find_library('netapi32')
  win32_deps += compiler.find_library('mpr')
endif

executable(
  'spice-webdavd',
  [ 'spice-webdavd.c' ],
  include_directories : incdir,
  dependencies : win32_deps + avahi_deps + deps,
  link_with : [ libphodav ],
  install : true,
)
