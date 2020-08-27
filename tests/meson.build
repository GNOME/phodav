tests_sources = []

if host_machine.system() != 'windows'
  tests_sources += 'virtual-dir.c'
endif

executable('virtual-dir-server',
           sources : 'virtual-dir-server.c',
           include_directories: incdir,
           link_with : libphodav,
           dependencies : deps)

foreach src : tests_sources
  name = 'test-@0@'.format(src).split('.')[0]
  exe = executable(name,
                   sources : src,
                   include_directories: incdir,
                   link_with : libphodav,
                   dependencies : deps)
  test(name, exe)
endforeach
