project(
  'phodav', 'c',
  version : run_command(
    'build-aux/git-version-gen',
    '@0@/.tarball-version'.format(meson.project_source_root()),
    check : true
  ).stdout().strip(),
  license : 'LGPLv2.1',
  meson_version : '>= 0.56',
)

meson.add_dist_script('sh', '-c', 'echo @0@>"$MESON_DIST_ROOT/.tarball-version"'.format(meson.project_version()))

proj_version = meson.project_version()
proj_name = meson.project_name()
proj_gettext = 'phodav-3.0'
proj_bugurl = 'https://gitlab.gnome.org/GNOME/phodav/issues/new'

gnome = import('gnome')
prefix = get_option('prefix')
sbindir = join_paths(prefix, get_option('sbindir'))
datadir = join_paths(prefix, get_option('datadir'))
mandir = join_paths(prefix, get_option('mandir'))
localedir = join_paths(datadir, 'locale')
incdir = include_directories('.')

compiler = meson.get_compiler('c')
conf = configuration_data()

glib_ver = '>= 2.76'

deps = []
if host_machine.system() == 'windows'
  deps += dependency('gio-windows-2.0', version : glib_ver)
else
  deps += dependency('gio-unix-2.0', version : glib_ver)
endif

deps += dependency('libsoup-3.0', version : '>= 3.0.0')
deps += dependency('libxml-2.0')

d1 = dependency('avahi-gobject', required : get_option('avahi'))
d2 = dependency('avahi-client', required : get_option('avahi'))
avahi_deps = []
if d1.found() and d2.found()
  avahi_deps += [ d1, d2 ]
  conf.set('WITH_AVAHI', 1)
endif

if compiler.has_header('sys/xattr.h')
  conf.set('HAVE_SYS_XATTR_H', 1)
endif

subdir('po')
subdir('libphodav')
subdir('bin')
subdir('doc')
subdir('data')
subdir('tests')

config = {
  'GETTEXT_PACKAGE' : proj_gettext,
  'LOCALEDIR' : localedir,
  'PACKAGE_BUGREPORT' : proj_bugurl,
  'PACKAGE_NAME' : proj_name,
  'PACKAGE_STRING' : '@0@ @1@'.format(proj_name, proj_version),
  'PACKAGE_VERSION' : proj_version,
  'VERSION' : proj_version,
}
foreach key, value : config
  conf.set_quoted(key, value)
endforeach

configure_file(
  output : 'config.h',
  install : false,
  configuration : conf
)
