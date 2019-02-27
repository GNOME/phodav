project(
  'phodav', 'c',
  version : run_command(
    'build-aux/git-version-gen',
    '@0@/.tarball-version'.format(meson.source_root()),
    check : true
  ).stdout().strip(),
  license : 'LGPLv2.1',
  meson_version : '>= 0.49',
)

meson.add_dist_script('sh', '-c', 'echo @0@>"$MESON_DIST_ROOT/.tarball-version"'.format(meson.project_version()))

proj_version = meson.project_version()
proj_name = meson.project_name()
proj_gettext = 'phodav-2.0'
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

deps = []
if host_machine.system() == 'windows'
  deps += dependency('gio-windows-2.0')
else
  deps += dependency('gio-unix-2.0')
endif

deps += dependency('libsoup-2.4', version : '>= 2.48.0')
deps += dependency('libxml-2.0')

d1 = dependency('avahi-gobject', required : get_option('avahi'))
d2 = dependency('avahi-client', required : get_option('avahi'))
avahi_deps = []
if d1.found() and d2.found()
  avahi_deps += [ d1, d2 ]
  conf.set('WITH_AVAHI', 1)
endif

systemd = dependency('systemd', required : get_option('systemd'))
if systemd.found()
  install_data(
    'data/spice-webdavd.service',
    install_dir : systemd.get_pkgconfig_variable('systemdsystemunitdir')
  )
endif

udev = dependency('udev', required : get_option('udev'))
if udev.found()
  install_data(
    'data/70-spice-webdavd.rules',
    install_dir : '@0@/@1@'.format(udev.get_pkgconfig_variable('udevdir'), 'rules.d'),
  )
endif

if compiler.has_header('attr/xattr.h')
  conf.set('HAVE_ATTR_XATTR_H', 1)
endif

subdir('po')
subdir('libphodav')
subdir('spice')
subdir('doc')

buildid = get_option('build-id')
version = proj_version.split('.')
maj = version[0].to_int()
min = version[1].split('-')[0].to_int()
mic = 0
if version.length() > 2
  mic = version[2].split('-')[0].to_int()
endif
win_version = '@0@.@1@.@2@'.format(maj, min, mic * 256 + buildid)

if buildid != 0
  buildid = '-@0@'.format(buildid)
else
  buildid = ''
endif

wixl_arch = 'x86'
if host_machine.cpu_family() == 'x86_64'
  wixl_arch = 'x64'
endif

if host_machine.system() == 'windows'
  foreach out : [ 'chezdav.wxs', 'spice-webdavd.wxs', 'make-msi.sh' ]
    configure_file(
      input : '@0@.in'.format(out),
      output : out,
      install : false,
      configuration : {
        'BUILDID' : buildid,
        'VERSION' : proj_version,
        'WINDOWS_PRODUCTVERSION' : win_version,
        'WIXL_ARCH' : wixl_arch,
        'srcdir' : meson.current_source_dir(),
        'prefix' : prefix,
      }
    )
  endforeach
endif

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
