project(
  'phodav', 'c',
  version : run_command(
    'build-aux/git-version-gen',
    '@0@/.tarball-version'.format(meson.project_source_root()),
    check : true
  ).stdout().strip(),
  license : 'LGPL-2.1-or-later',
  meson_version : '>= 1.0',
)

proj_version = meson.project_version()
meson.add_dist_script('sh', '-c', f'echo @proj_version@>"$MESON_DIST_ROOT/.tarball-version"')

proj_name = meson.project_name()
proj_gettext = 'phodav-3.0'
proj_bugurl = 'https://gitlab.gnome.org/GNOME/phodav/issues/new'

gnome = import('gnome')
prefix = get_option('prefix')
sbindir = prefix / get_option('sbindir')
datadir = prefix / get_option('datadir')
mandir = prefix / get_option('mandir')
localedir = datadir / 'locale'
incdir = include_directories('.')

compiler = meson.get_compiler('c')
conf = configuration_data()

glib_ver = '>= 2.80'

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

# Turn any accidental use of an API which is deprecated, or only
# available in a newer version than the ones supported here, into a
# compiler warning: the CI builds with -Dwerror=true, so that it fails
# the build. Keep the versions in sync with the dependency checks
# above.
add_project_arguments(
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_80',
  '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_80',
  '-DSOUP_VERSION_MIN_REQUIRED=SOUP_VERSION_3_0',
  '-DSOUP_VERSION_MAX_ALLOWED=SOUP_VERSION_3_0',
  language : 'c',
)

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
  'PACKAGE_STRING' : f'@proj_name@ @proj_version@',
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

summary({
  'mDNS / Avahi support' : d1.found() and d2.found(),
  'xattr support' : conf.has('HAVE_SYS_XATTR_H'),
  'Introspection data' : build_gir,
  'API reference (gi-docgen)' : build_docs,
  'Manual pages' : build_man,
  'systemd unit directory' : systemd_systemunitdir,
  'udev rules directory' : udev_rulesdir,
}, section : 'Configuration')
