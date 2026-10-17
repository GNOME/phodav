gidocgen = find_program('gi-docgen', required : get_option('docs'))

build_docs = false
if not build_gir
  if get_option('docs').enabled()
    error('API documentation requires gobject-introspection')
  endif
  message('Not building API documentation as gobject-introspection is unavailable')
elif not gidocgen.found()
  message('Not building API documentation as gi-docgen was not found')
else
  build_docs = true

  toml_conf = configuration_data()
  # gi-docgen requires a version it can parse (PEP 440); development
  # versions from git-version-gen look like "3.0.55-87edf179-dirty"
  toml_conf.set('PHODAV_VERSION', meson.project_version().split('-')[0])
  phodav_toml = configure_file(
    input : 'phodav-3.0.toml.in',
    output : 'phodav-3.0.toml',
    configuration : toml_conf,
  )

  custom_target('phodav-3.0-doc',
    input : libphodav_gir[0],
    output : 'phodav-3.0',
    command : [
      gidocgen, 'generate',
      '--quiet',
      '--config', phodav_toml,
      '--output-dir', '@OUTPUT@',
      '--no-namespace-dir',
      '@INPUT@',
    ],
    build_by_default : true,
    install : true,
    install_dir : datadir / 'doc',
  )
endif

asciidoc = find_program('asciidoc', required : false)
xmlto = find_program('xmlto', required : false)

build_man = asciidoc.found() and xmlto.found()

if build_man
  xml = custom_target(
    'chezdav XML',
    input : [ 'chezdav.txt' ],
    output : [ 'chezdav.xml' ],
    command : [
      asciidoc,
      '-o',
      '@OUTPUT@',
      '--backend=docbook',
      '--doctype=manpage',
      '@INPUT@',
    ],
  )
  man = custom_target(
    'chezdav MAN',
    input : xml,
    output : [ 'chezdav.1' ],
    command : [
      xmlto,
      'man',
      '-o', meson.current_build_dir(),
      '@INPUT@',
    ],
    install : true,
    install_dir : mandir / 'man1',
  )
endif
