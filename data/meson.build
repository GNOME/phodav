systemd_systemunitdir = get_option('systemdsystemunitdir')
if systemd_systemunitdir == ''
  systemd = dependency('systemd', required : false)
  if systemd.found()
    systemd_systemunitdir = systemd.get_variable(pkgconfig: 'systemdsystemunitdir')
  endif
endif
if systemd_systemunitdir != ''
  install_data(
    'spice-webdavd.service',
    install_dir : systemd_systemunitdir
  )
endif

udev_rulesdir = get_option('udevrulesdir')
if udev_rulesdir == ''
  udev = dependency('udev', required : false)
  if udev.found()
    udev_rulesdir = '@0@/@1@'.format(udev.get_variable(pkgconfig: 'udevdir'), 'rules.d')
  endif
endif
if udev_rulesdir != ''
  install_data(
    '70-spice-webdavd.rules',
    install_dir : udev_rulesdir
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
                        'top_srcdir' : meson.project_source_root(),
                        'prefix' : prefix,
                      }
    )
  endforeach
endif
