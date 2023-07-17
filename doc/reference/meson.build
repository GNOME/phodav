ent_conf = configuration_data()
ent_conf.set('PACKAGE', proj_name)
ent_conf.set('PACKAGE_BUGREPORT', proj_bugurl)
ent_conf.set('PACKAGE_NAME', 'PhoDAV')
ent_conf.set('PACKAGE_STRING', proj_name)
ent_conf.set('PACKAGE_VERSION', meson.project_version())
configure_file(
  input: 'gtkdocentities.ent.in',
  output: 'gtkdocentities.ent',
  configuration: ent_conf
)

glib_prefix = dependency('glib-2.0').get_variable(pkgconfig: 'prefix')
glib_docpath = join_paths(glib_prefix, 'share', 'gtk-doc', 'html')
docpath = join_paths(datadir, 'gtk-doc', 'html')

private_headers = [
  'config.h',
  'guuid.h',
  'phodav-lock.h',
  'phodav-multistatus.h',
  'phodav-path.h',
  'phodav-priv.h',
  'phodav-utils.h',
]

gnome.gtkdoc(
  'phodav-3.0',
  main_sgml : 'phodav-3.0-docs.sgml',
  dependencies : declare_dependency(link_with : [libphodav]),
  src_dir : join_paths(meson.project_source_root(), 'libphodav'),
  gobject_typesfile: 'libphodav.types',
  scan_args: [
    '--rebuild-types',
    '--ignore-headers=' + ' '.join(private_headers),
  ],
  fixxref_args: [
    '--html-dir=@0@'.format(docpath),
    '--extra-dir=@0@'.format(join_paths(glib_docpath, 'glib')),
    '--extra-dir=@0@'.format(join_paths(glib_docpath, 'gobject')),
    '--extra-dir=@0@'.format(join_paths(glib_docpath, 'gio')),
  ],
 install : true,
)
