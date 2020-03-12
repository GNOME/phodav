systemd = dependency('systemd', required : get_option('systemd'))
if systemd.found()
  install_data(
    'spice-webdavd.service',
    install_dir : systemd.get_pkgconfig_variable('systemdsystemunitdir')
  )
endif

udev = dependency('udev', required : get_option('udev'))
if udev.found()
  install_data(
    '70-spice-webdavd.rules',
    install_dir : '@0@/@1@'.format(udev.get_pkgconfig_variable('udevdir'), 'rules.d'),
  )
endif

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
                        'top_srcdir' : meson.source_root(),
                        'prefix' : prefix,
                      }
    )
  endforeach
endif
