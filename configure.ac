AC_PREREQ([2.59])
AC_INIT([phodav], [m4_esyscmd(build-aux/git-version-gen .tarball-version)],
                  [https://bugzilla.gnome.org/enter_bug.cgi?product=phodav])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CANONICAL_HOST
AC_PROG_CC

AC_MSG_CHECKING([for native Win32])
case "$host_os" in
  *mingw*|*cygwin*)
    os_win32=yes
    gio_os=gio-windows-2.0
  ;;
  *)
    os_win32=no
    gio_os=gio-unix-2.0
  ;;
esac
AC_MSG_RESULT([$os_win32])
AM_CONDITIONAL([OS_WIN32],[test "$os_win32" = "yes"])

AM_INIT_AUTOMAKE([foreign -Wall -Wno-portability subdir-objects dist-xz])
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])
AM_MAINTAINER_MODE([enable])
LT_INIT([win32-dll disable-fast-install])

gl_MANYWARN_ALL_GCC([warnings])
nw=
nw="$nw -Wsystem-headers"       # Don't let system headers trigger warnings
nw="$nw -Wundef"                # All compiler preprocessors support #if UNDEF
nw="$nw -Wtraditional"          # All compilers nowadays support ANSI C
nw="$nw -Wstrict-overflow"      # yacc/bison generated code uses that..
nw="$nw -Wconversion"           # These warnings usually don't point to mistakes.
nw="$nw -Wsign-conversion"      # Likewise.
nw="$nw -Wunused-parameter"     # Likewise.
nw="$nw -Wunused-local-typedefs" # Likewise.
nw="$nw -Wunused-macros"        # Likewise.
nw="$nw -Wunsafe-loop-optimizations" # Likewise.
# Enable all GCC warnings not in this list.
gl_MANYWARN_COMPLEMENT([warnings], [$warnings], [$nw])
for w in $warnings; do
  gl_WARN_ADD([$w])
done
gl_WARN_ADD([-Wno-sign-compare])
gl_WARN_ADD([-Wno-unused-parameter])
gl_WARN_ADD([-Wno-missing-field-initializers])

GETTEXT_PACKAGE=phodav
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE, ["$GETTEXT_PACKAGE"], [Define to the Gettext package name])
AC_SUBST(GETTEXT_PACKAGE)
IT_PROG_INTLTOOL([0.40.0])

AC_CHECK_HEADERS([attr/xattr.h])

PKG_CHECK_MODULES(GIO, [$gio_os])
PKG_CHECK_MODULES(SOUP, libsoup-2.4 libxml-2.0)

AC_ARG_WITH([avahi],
  AS_HELP_STRING([--without-avahi], [Build without avahi/mdns]))

AS_IF([test "x$with_avahi" != "xno"],
  [PKG_CHECK_MODULES([AVAHI], [avahi-gobject avahi-client],
                     [have_avahi=yes], [have_avahi=no])],
  [have_avahi=no])
AS_IF([test "x$have_avahi" != "xyes"],
  [AS_IF([test "x$with_avahi" = "xyes"],
         [AC_MSG_ERROR([avahi requested but not found])])])
AS_IF([test "x$have_avahi" = "xyes"], [AC_DEFINE([WITH_AVAHI], [1], [With avahi])])

AC_ARG_ENABLE([pie],
  AS_HELP_STRING([--enable-pie=@<:@auto/yes/no@:>@],
                 [Enable position-independent-executable support @<:@default=auto@:>@]),
                 [],
                 [enable_pie="auto"])

if test "x$enable_pie" != "xno"; then
  save_CFLAGS="$CFLAGS"
  save_LDFLAGS="$LDFLAGS"
  CFLAGS="$CFLAGS -fPIE"
  LDFLAGS="$LDFLAGS -pie -Wl,-z,relro -Wl,-z,now"
  AC_MSG_CHECKING([for PIE support])
  AC_LINK_IFELSE([AC_LANG_SOURCE([int main () { return 0; }])],
                 [have_pie=yes],
                 [have_pie=no])
  AC_MSG_RESULT([$have_pie])
  if test "x$have_pie" = "xno" && test "x$enable_pie" = "xyes"; then
    AC_MSG_ERROR([pie support explicitly requested, but your toolchain does not support it])
  fi
  if test "x$have_pie" = "xyes"; then
    PIE_CFLAGS="-fPIE"
    PIE_LDFLAGS="-pie -Wl,-z,relro -Wl,-z,now"
    AC_SUBST(PIE_CFLAGS)
    AC_SUBST(PIE_LDFLAGS)
  fi
  CFLAGS="$save_CFLAGS"
  LDFLAGS="$save_LDFLAGS"
else
  have_pie=no
fi

AC_CONFIG_FILES([
Makefile
po/Makefile.in
])
AC_OUTPUT

dnl ==========================================================================
AC_MSG_NOTICE([

    $PACKAGE $VERSION
    =================

      prefix:                   $prefix
      c compiler:               $CC

      Avahi:                    $have_avahi
])
