NULL =
AUTOMAKE_OPTIONS = 1.6

DOC_MODULE = phodav
DOC_MAIN_SGML_FILE = $(DOC_MODULE)-docs.sgml
DOC_SOURCE_DIR = $(top_srcdir)/libphodav

SCANGOBJ_OPTIONS =
SCAN_OPTIONS = --rebuild-types
MKDB_OPTIONS = --xml-mode --output-format=xml
MKTMPL_OPTIONS =
MKHTML_OPTIONS =
FIXXREF_OPTIONS =

# Used for dependencies. The docs will be rebuilt if any of these change.
HFILE_GLOB = $(top_srcdir)/libphodav/phodav-*.h
CFILE_GLOB = $(top_srcdir)/libphodav/phodav-*.c
EXTRA_HFILES =
IGNORE_HFILES =					\
	guuid.h					\
	glib-compat.h				\
	$(NULL)

GTKDOC_CFLAGS = $(SOUP_CFLAGS)
GTKDOC_LIBS = $(top_builddir)/libphodav-1.0.la

include $(top_srcdir)/gtk-doc.make

EXTRA_DIST += phodav-docs.sgml.in
DISTCLEANFILES = $(DOC_MODULE).types

if ENABLE_GTK_DOC
TESTS_ENVIRONMENT = cd $(srcdir) && \
  DOC_MODULE=$(DOC_MODULE) DOC_MAIN_SGML_FILE=$(DOC_MAIN_SGML_FILE) \
  SRCDIR=$(abs_srcdir) BUILDDIR=$(abs_builddir)
#TESTS = $(GTKDOC_CHECK)
endif

-include $(top_srcdir)/git.mk
