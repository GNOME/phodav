# libFuzzer targets for the DAV request parsers.
#
# The harnesses drive a real in-process PhodavServer, over a
# socketpair: no socket is listened on and nothing leaves the process
# (see fuzz-server.c). libFuzzer is only implemented by clang, so the
# option requires it.
#
# The seed corpora double as smoke tests: given individual files as
# arguments, libFuzzer runs each input once and exits. The corpora
# grow over a fuzzing session, so the longer runs in the CI fuzzing
# job use copies of them.

if host_machine.system() == 'windows'
  if get_option('fuzzing').enabled()
    error('the fuzzing targets are not supported on Windows')
  endif
elif get_option('fuzzing').enabled()
  if compiler.get_id() != 'clang'
    error('the fuzzing targets require clang, which implements libFuzzer')
  endif

  fuzz_c_args = ['-fsanitize=fuzzer-no-link', '-fno-omit-frame-pointer']
  fuzz_link_args = ['-fsanitize=fuzzer']

  # a coverage-instrumented copy of the library, so that libFuzzer
  # sees what the request parsers are doing
  libphodav_fuzz = static_library('phodav-fuzz',
    libphodav_sources,
    c_args : fuzz_c_args + ['-DG_LOG_DOMAIN="phodav"'],
    include_directories : incdir,
    dependencies : deps)

  fuzz_common = ['fuzz-server.c', 'fuzz-server.h']

  request_seeds = files(
    'corpus/request/0001-get',
    'corpus/request/0002-head',
    'corpus/request/0003-options',
    'corpus/request/0004-propfind-allprop',
    'corpus/request/0005-propfind-prop',
    'corpus/request/0006-propfind-propname',
    'corpus/request/0007-propfind-bad-depth',
    'corpus/request/0008-put-if-not',
    'corpus/request/0009-put-if-tagged',
    'corpus/request/0010-put-if-etag',
    'corpus/request/0011-put-if-malformed',
    'corpus/request/0012-put-if-multi-lists',
    'corpus/request/0013-lock',
    'corpus/request/0014-lock-refresh',
    'corpus/request/0015-unlock',
    'corpus/request/0016-delete',
    'corpus/request/0017-copy',
    'corpus/request/0018-move',
    'corpus/request/0019-proppatch',
    'corpus/request/0020-mkcol',
    'corpus/request/0021-traversal',
    'corpus/request/0022-fragment',
    'corpus/request/0023-query',
    'corpus/request/0024-unknown-method',
    'corpus/request/0025-garbage',
    'corpus/request/0026-put-expect-continue')

  body_seeds = files(
    'corpus/body/0001-propfind-allprop',
    'corpus/body/0002-propfind-dir',
    'corpus/body/0003-propfind-propname',
    'corpus/body/0004-propfind-prop',
    'corpus/body/0005-propfind-dead-props',
    'corpus/body/0006-propfind-infinity',
    'corpus/body/0007-proppatch-set',
    'corpus/body/0008-proppatch-remove',
    'corpus/body/0009-lockinfo-exclusive',
    'corpus/body/0010-lockinfo-shared',
    'corpus/body/0011-lockinfo-owner-xml',
    'corpus/body/0012-lockinfo-invalid',
    'corpus/body/0013-lock-refresh',
    'corpus/body/0014-put-contents',
    'corpus/body/0015-mkcol',
    'corpus/body/0016-garbage',
    'corpus/body/0017-wrong-root',
    'corpus/body/0018-no-ns',
    'corpus/body/0019-empty-prop',
    'corpus/body/0020-proppatch-empty')

  fuzzers = {
    'fuzz-request': ['fuzz-request.c', request_seeds],
    'fuzz-body': ['fuzz-body.c', body_seeds],
  }

  foreach name, fuzz : fuzzers
    fuzzer = executable(name,
      sources : [fuzz[0]] + fuzz_common,
      c_args : fuzz_c_args,
      link_args : fuzz_link_args,
      include_directories : [incdir, libphodav_inc],
      link_with : libphodav_fuzz,
      dependencies : deps)

    # libFuzzer exits with 77 on an error by default, which meson
    # reports as a SKIP: make the crashes fail the test
    test(name, fuzzer, args : ['-error_exitcode=1'] + fuzz[1], timeout : 60)
  endforeach
endif

summary({'libFuzzer targets' : get_option('fuzzing').enabled()},
        section : 'Configuration')
