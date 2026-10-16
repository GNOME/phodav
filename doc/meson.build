build_gtk_doc = false
if build_machine.system() == 'windows'
  message('Disabling gtk-doc while building on Windows')
elif find_program('gtkdoc-scan', required : get_option('gtk_doc')).found()
  build_gtk_doc = true
  subdir('reference')
else
  message('Not building documentation as gtk-doc was not found')
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
