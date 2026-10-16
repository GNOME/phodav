tests_sources = [
  'virtual-dir.c',
]

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

chezdav_env = []
if conf.has('WITH_AVAHI')
  # chezdav exits when there is no avahi daemon to announce to
  chezdav_env += 'CHEZDAV_TEST_NO_MDNS=1'
endif

test('chezdav',
     executable('test-chezdav',
                sources : 'chezdav.c',
                dependencies : deps),
     env : chezdav_env)
