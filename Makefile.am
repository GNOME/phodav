NULL =
EXTRA_DIST =
BUILT_SOURCES =
ACLOCAL_AMFLAGS = -I m4
makeflags_ = $(makeflags_$(AM_DEFAULT_VERBOSITY))
makeflags_0 = --no-print-directory -s
makeflags_1 =
AM_MAKEFLAGS = $(makeflags_$(V))

SUBDIRS = po spice libphodav doc
noinst_LTLIBRARIES = libphodav.la
bin_PROGRAMS = chezdav
sbin_PROGRAMS = spice-webdavd

AM_CPPFLAGS =					\
	-DLOCALEDIR=\""$(localedir)"\"		\
	-DG_LOG_DOMAIN=\"phodav\"		\
	$(WARN_CFLAGS)				\
	$(NULL)

libphodav_la_SOURCES =				\
	libphodav/guuid.c			\
	libphodav/guuid.h			\
	libphodav/phodav-server.c		\
	libphodav/phodav-server.h		\
	libphodav/phodav.h			\
	$(NULL)

libphodav_la_CFLAGS =				\
	$(GIO_CFLAGS)				\
	$(SOUP_CFLAGS)				\
	$(NULL)

libphodav_la_LIBADD =				\
	$(GIO_LIBS)				\
	$(SOUP_LIBS)				\
	$(NULL)

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

chezdav_SOURCES = libphodav/chezdav.c

chezdav_CFLAGS =		\
	-I.			\
	$(GIO_CFLAGS)		\
	$(SOUP_CFLAGS)		\
	$(AVAHI_CFLAGS)		\
	$(NULL)

chezdav_LDADD =			\
	libphodav.la		\
	$(GLIB_LIBS)		\
	$(GIO_LIBS)		\
	$(AVAHI_LIBS)		\
	$(NULL)

systemdunitdir = $(SYSTEMDSYSTEMUNITDIR)
systemdunit_DATA = \
	$(top_srcdir)/data/spice-webdavd.service \
	$(top_srcdir)/data/spice-webdavd.target

udevrulesdir = /lib/udev/rules.d
udevrules_DATA = $(top_srcdir)/data/70-spice-webdavd.rules

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

MAINTAINERCLEANFILES =					\
	$(GITIGNORE_MAINTAINERCLEANFILES_TOPLEVEL)	\
	$(GITIGNORE_MAINTAINERCLEANFILES_MAKEFILE_IN)	\
	$(GITIGNORE_MAINTAINERCLEANFILES_M4_LIBTOOL)

-include $(top_srcdir)/git.mk
