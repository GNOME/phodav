tests_sources = [
  'virtual-dir.c',
]

server = executable('virtual-dir-server',
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
  # the tests spawn the server, make sure it is rebuilt too
  test(name, exe, depends : server)
endforeach

chezdav_env = ['CHEZDAV=' + chezdav.full_path()]
if conf.has('WITH_AVAHI')
  # chezdav exits when there is no avahi daemon to announce to
  chezdav_env += 'CHEZDAV_TEST_NO_MDNS=1'
endif

test('chezdav',
     executable('test-chezdav',
                sources : 'chezdav.c',
                dependencies : deps),
     depends : chezdav,
     env : chezdav_env)
