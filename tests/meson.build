tests_sources = [
  'virtual-dir.c',
  'virtual-dir-file.c',
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
# the HTTPS tests use the self-signed certificate of tests/tls
chezdav_env += [
  'CHEZDAV_TEST_CERT=' + meson.current_source_dir() / 'tls' / 'cert.pem',
  'CHEZDAV_TEST_KEY=' + meson.current_source_dir() / 'tls' / 'key.pem',
]
# the mingw glib-networking under Wine rejects the GnuTLS priority it
# builds its sessions with by default ("NORMAL:%COMPAT"), which fails
# (and even hangs) every handshake; a plain priority works everywhere,
# and is ignored by the other TLS backends
chezdav_env += 'G_TLS_GNUTLS_PRIORITY=NORMAL'

test('chezdav',
     executable('test-chezdav',
                sources : 'chezdav.c',
                include_directories : incdir,
                dependencies : deps),
     depends : chezdav,
     env : chezdav_env)

# unit test for the write queue used by spice-webdavd
output_queue_test = executable('test-output-queue',
                               sources : ['output-queue.c'] + output_queue_sources,
                               include_directories : [incdir, bin_inc],
                               dependencies : deps)
test('test-output-queue', output_queue_test)

# functional test of the spice-webdavd daemon, over a FIFO standing in
# for the virtio port (mkfifo is not available on Windows)
if host_machine.system() != 'windows'
  webdavd_env = ['SPICE_WEBDAVD=' + spice_webdavd.full_path()]
  test('test-spice-webdavd',
       executable('test-spice-webdavd',
                  sources : 'spice-webdavd.c',
                  include_directories : incdir,
                  dependencies : deps),
       depends : spice_webdavd,
       env : webdavd_env,
       timeout : 60)
endif

# run the litmus compliance suite; litmus is not packaged everywhere
# (Fedora retired it), so build it from source as an optional subproject
# when it is not installed on the system
litmus = disabler()
litmus_depends = []
if not get_option('litmus').disabled()
  litmus = find_program('litmus', required : false)
  if not litmus.found()
    litmus_sub = subproject('litmus', required : get_option('litmus'))
    if litmus_sub.found()
      litmus = litmus_sub.get_variable('litmus_prog')
      # the litmus(1) driver script runs the suite programs built by the
      # subproject; meson test would not build them otherwise, as they
      # are not in the dependency graph of the script itself
      litmus_depends += litmus_sub.get_variable('litmus_suites')
    endif
  endif
endif
if litmus.found()
  # the litmus and neon code leaks its own test state; do not let it
  # drown out leaks in the phodav test server (see litmus-leaks.supp).
  # LSAN_OPTIONS is ignored unless the tests run with sanitizers
  litmus_env = ['LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'litmus-leaks.supp']
  test('litmus',
       find_program(files('litmus.sh')),
       args : [server.full_path(), litmus.full_path()],
       depends : [server] + litmus_depends,
       env : litmus_env,
       timeout : 120)
endif

summary({'litmus compliance tests' : litmus.found()},
        section : 'Configuration')

subdir('fuzz')
