NULL =
EXTRA_DIST =
BUILT_SOURCES =
CONFIG_STATUS_DEPENDENCIES =
ACLOCAL_AMFLAGS = -I m4
makeflags_ = $(makeflags_$(AM_DEFAULT_VERBOSITY))
makeflags_0 = --no-print-directory -s
makeflags_1 =
AM_MAKEFLAGS = $(makeflags_$(V))

SUBDIRS = . po spice libphodav doc
lib_LTLIBRARIES = libphodav-2.0.la
bin_PROGRAMS = chezdav
sbin_PROGRAMS = spice-webdavd

AM_CPPFLAGS =					\
	-DLOCALEDIR=\""$(localedir)"\"		\
	-DG_LOG_DOMAIN=\"phodav\"		\
	$(WARN_CFLAGS)				\
	$(NULL)

libphodav_includedir = $(includedir)/libphodav-2.0/libphodav
libphodav_include_HEADERS =			\
	libphodav/phodav-server.h		\
	libphodav/phodav.h			\
	$(NULL)

libphodav_2_0_la_SOURCES =			\
	libphodav/phodav-priv.h			\
	libphodav/phodav-path.c			\
	libphodav/phodav-path.h			\
	libphodav/phodav-lock.c			\
	libphodav/phodav-lock.h			\
	libphodav/phodav-if.c			\
	libphodav/phodav-method-delete.c	\
	libphodav/phodav-method-get.c		\
	libphodav/phodav-method-lock.c		\
	libphodav/phodav-method-mkcol.c		\
	libphodav/phodav-method-movecopy.c	\
	libphodav/phodav-method-propfind.c	\
	libphodav/phodav-method-proppatch.c	\
	libphodav/phodav-method-unlock.c	\
	libphodav/phodav-method-put.c		\
	libphodav/phodav-multistatus.c		\
	libphodav/phodav-multistatus.h		\
	libphodav/phodav-server.c		\
	libphodav/phodav-server.h		\
	libphodav/phodav-utils.c		\
	libphodav/phodav-utils.h		\
	$(NULL)

if BUILD_GUUID
libphodav_2_0_la_SOURCES +=			\
	libphodav/guuid.c			\
	libphodav/guuid.h			\
	$(NULL)
endif

libphodav_2_0_la_CFLAGS =			\
	$(GIO_CFLAGS)				\
	$(SOUP_CFLAGS)				\
	$(NULL)

libphodav_2_0_la_LIBADD =			\
	$(GIO_LIBS)				\
	$(SOUP_LIBS)				\
	$(NULL)

libphodav_2_0_la_LDFLAGS =				\
	-version-info 0:0:0				\
	-Wl,--version-script=${srcdir}/libphodav.syms	\
	-no-undefined					\
	$(NULL)

libphodav_2_0_la_DEPENDENCIES = libphodav.syms
EXTRA_DIST += libphodav.syms

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libphodav-2.0.pc

spice_webdavd_SOURCES = spice/spice-webdavd.c

spice_webdavd_CFLAGS =		\
	$(GIO_CFLAGS)		\
	$(AVAHI_CFLAGS)		\
	$(PIE_CFLAGS)		\
	$(NULL)

spice_webdavd_LDADD =		\
	$(GIO_LIBS)		\
	$(AVAHI_LIBS)		\
	$(PIE_LDFLAGS)		\
	$(NULL)

if OS_WIN32
spice_webdavd_LDADD += -lnetapi32 -lmpr
endif

deps.txt:
	$(AM_V_GEN)rpm -qa | grep $(host_os) | sort | unix2dos > $@

MANUFACTURER = The Spice Project

EXTRA_DIST += chezdav.wxs.in
CONFIG_STATUS_DEPENDENCIES += chezdav.wxs.in

chezdav-$(WIXL_ARCH)-$(VERSION)$(BUILDID).msi: chezdav.wxs deps.txt all
	$(AM_V_GEN)DESTDIR=`mktemp -d`&&				\
	make -C $(top_builddir) install DESTDIR=$$DESTDIR >/dev/null &&	\
	MANUFACTURER="$(MANUFACTURER)" wixl -D SourceDir=$(prefix)	\
	  -D DESTDIR=$$DESTDIR$(prefix)					\
	  --arch $(WIXL_ARCH)  -o $@ $<

MSI = chezdav-$(WIXL_ARCH)-$(VERSION)$(BUILDID).msi

EXTRA_DIST += spice-webdavd.wxs.in
CONFIG_STATUS_DEPENDENCIES += spice-webdavd.wxs.in

spice-webdavd-$(WIXL_ARCH)-$(VERSION)$(BUILDID).msi: spice-webdavd.wxs deps.txt all
	$(AM_V_GEN)DESTDIR=`mktemp -d`&&				\
	make -C $(top_builddir) install DESTDIR=$$DESTDIR >/dev/null &&	\
	MANUFACTURER="$(MANUFACTURER)" wixl -D SourceDir=$(prefix)	\
	  -D DESTDIR=$$DESTDIR$(prefix)					\
	  --arch $(WIXL_ARCH)  -o $@ $<

MSI += spice-webdavd-$(WIXL_ARCH)-$(VERSION)$(BUILDID).msi

msi: $(MSI)

CLEANFILES = $(MSI)

.PHONY: msi

chezdav_SOURCES = libphodav/chezdav.c

chezdav_CFLAGS =		\
	-I.			\
	$(GIO_CFLAGS)		\
	$(SOUP_CFLAGS)		\
	$(AVAHI_CFLAGS)		\
	$(NULL)

chezdav_LDADD =			\
	libphodav-2.0.la	\
	$(GLIB_LIBS)		\
	$(GIO_LIBS)		\
	$(SOUP_LIBS)		\
	$(AVAHI_LIBS)		\
	$(NULL)

if HAVE_SYSTEMD
systemdunitdir = $(SYSTEMDSYSTEMUNITDIR)
dist_systemdunit_DATA = \
	$(top_srcdir)/data/spice-webdavd.service \
	$(NULL)
endif

if HAVE_UDEV
udevrulesdir = $(UDEVDIR)/rules.d
dist_udevrules_DATA = $(top_srcdir)/data/70-spice-webdavd.rules
endif

BUILT_SOURCES += $(top_srcdir)/.version
$(top_srcdir)/.version:
	@echo $(VERSION) > $@-t && mv $@-t $@
dist-hook:
	echo $(VERSION) > $(distdir)/.tarball-version

EXTRA_DIST +=					\
	$(top_srcdir)/.version			\
	NEWS					\
	autogen.sh				\
	build-aux/git-version-gen		\
	phodav.doap				\
	$(NULL)

AM_DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc

DISTCLEANFILES = 	\
	gtk-doc.make    \
	m4/gtk-doc.m4   \
	m4/intltool.m4

MAINTAINERCLEANFILES =					\
	$(srcdir)/gtk-doc.make				\
	$(GITIGNORE_MAINTAINERCLEANFILES_TOPLEVEL)	\
	$(GITIGNORE_MAINTAINERCLEANFILES_MAKEFILE_IN)	\
	$(GITIGNORE_MAINTAINERCLEANFILES_M4_LIBTOOL)

DISTCHECK_CONFIGURE_FLAGS =							\
	--with-systemdsystemunitdir='$${libdir}/systemd/system-distcheck'	\
	--with-udevdir='$${libdir}/udev-distcheck'

-include $(top_srcdir)/git.mk
