tests_sources = [
  'virtual-dir.c',
]

server = executable('virtual-dir-server',
                    sources : 'virtual-dir-server.c',
                    include_directories: incdir,
                    link_with : libphodav,
                    dependencies : deps)

foreach src : tests_sources
  name = src.split('.')[0]
  exe = executable(f'test-@name@',
                   sources : src,
                   include_directories: incdir,
                   link_with : libphodav,
                   dependencies : deps)
  # the tests spawn the server, make sure it is rebuilt too
  test(f'test-@name@', exe, depends : server)
endforeach

# unit tests for the internal helpers, linked against the private symbols
unit = executable('test-unit',
                   sources : 'unit.c',
                   include_directories : [incdir, libphodav_inc],
                   link_with : libphodav_internals,
                   dependencies : deps)
test('test-unit', unit)

# end-to-end tests of the DAV methods over HTTP
dav = executable('test-dav',
                 sources : 'dav.c',
                 include_directories : incdir,
                 link_with : libphodav,
                 dependencies : deps)
test('test-dav', dav)

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
