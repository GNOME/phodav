/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* Unit tests of the GFile implementation of PhodavVirtualDir,
 * complementing the HTTP level tests with direct API calls. */

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "libphodav/phodav.h"

typedef struct
{
  gchar *dir;
  PhodavVirtualDir *root;
} VdirFixture;

static void
vdir_fixture_setup (VdirFixture *f, gconstpointer user_data)
{
  g_autofree gchar *file_path = NULL;
  g_autofree gchar *subdir_path = NULL;

  f->dir = g_dir_make_tmp ("phodav-vdir-XXXXXX", NULL);
  g_assert_nonnull (f->dir);

  file_path = g_build_filename (f->dir, "real.txt", NULL);
  g_assert_true (g_file_set_contents (file_path, "real", -1, NULL));

  subdir_path = g_build_filename (f->dir, "subdir", NULL);
  g_assert_cmpint (g_mkdir (subdir_path, 0700), ==, 0);

  f->root = phodav_virtual_dir_new_root ();
  phodav_virtual_dir_root_set_real (f->root, f->dir);
}

static void
vdir_fixture_teardown (VdirFixture *f, gconstpointer user_data)
{
  g_autoptr(GFile) file = g_file_new_for_path (f->dir);
  GFileEnumerator *e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

  while (e)
    {
      GFileInfo *info = g_file_enumerator_next_file (e, NULL, NULL);
      GFile *child;

      if (!info)
        break;

      child = g_file_get_child (file, g_file_info_get_name (info));
      g_file_delete (child, NULL, NULL);
      g_object_unref (child);
      g_object_unref (info);
    }

  g_clear_object (&e);
  g_file_delete (file, NULL, NULL);

  g_object_unref (f->root);
  g_free (f->dir);
}

static void
test_names (VdirFixture *f, gconstpointer user_data)
{
  g_autofree gchar *path = NULL;
  g_autofree gchar *basename = NULL;
  g_autoptr(GFile) parent = NULL;
  g_autoptr(GFile) child = NULL;
  g_autoptr(PhodavVirtualDir) dir = NULL;
  g_autoptr(GError) error = NULL;

  /* the root path falls through to the real root */
  path = g_file_get_path (G_FILE (f->root));
  g_assert_cmpstr (path, ==, f->dir);

  basename = g_file_get_basename (G_FILE (f->root));
  g_assert_cmpstr (basename, ==, "/");

  /* the root has no parent */
  parent = g_file_get_parent (G_FILE (f->root));
  g_assert_null (parent);

  /* a virtual directory has a path of its own */
  dir = phodav_virtual_dir_new_dir (f->root, "/virtual", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  g_assert_true (g_file_equal (G_FILE (dir), G_FILE (dir)));
  g_assert_cmpint (g_file_hash (G_FILE (dir)), ==, g_file_hash (G_FILE (dir)));

  g_clear_pointer (&basename, g_free);
  basename = g_file_get_basename (G_FILE (dir));
  g_assert_cmpstr (basename, ==, "virtual");

  g_clear_pointer (&path, g_free);
  path = g_file_get_path (G_FILE (dir));
  g_assert_cmpstr (path, ==, "/virtual");

  parent = g_file_get_parent (G_FILE (dir));
  g_assert_true (g_file_equal (parent, G_FILE (f->root)));

  /* the operations which a virtual directory does not support */
  g_assert_false (g_file_has_uri_scheme (G_FILE (dir), "file"));
  g_assert_false (g_file_is_native (G_FILE (dir)));
  g_assert_null (g_file_get_uri_scheme (G_FILE (dir)));
  g_assert_null (g_file_get_uri (G_FILE (dir)));
  g_assert_null (g_file_get_parse_name (G_FILE (dir)));
  g_assert_null (g_file_dup (G_FILE (dir)));
  g_assert_null (g_file_get_relative_path (G_FILE (f->root), G_FILE (dir)));
  g_assert_false (g_file_has_prefix (G_FILE (dir), G_FILE (f->root)));

  child = g_file_get_child_for_display_name (G_FILE (dir), "new", &error);
  g_assert_null (child);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);

  child = g_file_set_display_name (G_FILE (dir), "new", NULL, &error);
  g_assert_null (child);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
}

static void
test_query_info (VdirFixture *f, gconstpointer user_data)
{
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(PhodavVirtualDir) dir = NULL;
  g_autoptr(GError) error = NULL;
  guint64 usage = 0;

  /* the root queries its real root */
  info = g_file_query_info (G_FILE (f->root), "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (g_file_info_get_file_type (info), ==, G_FILE_TYPE_DIRECTORY);
  g_clear_pointer (&info, g_object_unref);

  /* a plain virtual directory reports itself as a directory */
  dir = phodav_virtual_dir_new_dir (f->root, "/virtual", &error);
  g_assert_no_error (error);

  info = g_file_query_info (G_FILE (dir), "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (g_file_info_get_file_type (info), ==, G_FILE_TYPE_DIRECTORY);
  g_assert_cmpstr (g_file_info_get_name (info), ==, "virtual");
  g_clear_pointer (&info, g_object_unref);

  /* filesystem attributes of the root come from the real root */
  info = g_file_query_filesystem_info (G_FILE (f->root), "filesystem::*", NULL, &error);
  g_assert_no_error (error);
  g_assert_true (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE));
  g_clear_pointer (&info, g_object_unref);

  /* a virtual directory has no filesystem of its own */
  info = g_file_query_filesystem_info (G_FILE (dir), "filesystem::*", NULL, &error);
  g_assert_no_error (error);
  g_clear_pointer (&info, g_object_unref);

  /* disk usage of the real root can be measured */
  g_assert_true (g_file_measure_disk_usage (G_FILE (f->root), G_FILE_MEASURE_NONE, NULL, NULL, NULL,
                                            &usage, NULL, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (usage, >, 0);

  /* but not of a plain virtual directory */
  g_assert_false (g_file_measure_disk_usage (G_FILE (dir), G_FILE_MEASURE_NONE, NULL, NULL, NULL,
                                             &usage, NULL, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);

  /* attributes of the real root are set through the root */
  {
    guint64 mtime = 0;

    g_assert_true (g_file_set_attribute (G_FILE (f->root), G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                         G_FILE_ATTRIBUTE_TYPE_UINT64, &mtime,
                                         G_FILE_QUERY_INFO_NONE, NULL, &error));
    g_assert_no_error (error);
  }

  /* ... but not on a plain virtual directory */
  {
    guint64 mtime = 0;

    g_assert_false (g_file_set_attribute (G_FILE (dir), G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                          G_FILE_ATTRIBUTE_TYPE_UINT64, &mtime,
                                          G_FILE_QUERY_INFO_NONE, NULL, &error));
  }
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);

  info = g_file_query_info (G_FILE (dir), "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_false (g_file_set_attributes_from_info (G_FILE (dir), info,
                                                   G_FILE_QUERY_INFO_NONE, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
}

static void
test_enumerate (VdirFixture *f, gconstpointer user_data)
{
  g_autoptr(PhodavVirtualDir) dir = NULL;
  g_autoptr(GError) error = NULL;
  GFileEnumerator *e = NULL;
  GFileInfo *info;
  guint n_virtual = 0, n_real = 0;
  gboolean found_subdir = FALSE;

  dir = phodav_virtual_dir_new_dir (f->root, "/virtual", &error);
  g_assert_no_error (error);

  /* the root lists the virtual directories first, then the real children */
  e = g_file_enumerate_children (G_FILE (f->root), "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &error);
  g_assert_no_error (error);

  while ((info = g_file_enumerator_next_file (e, NULL, &error)))
    {
      const gchar *name = g_file_info_get_name (info);

      if (g_strcmp0 (name, "virtual") == 0)
        n_virtual++;
      else if (g_strcmp0 (name, "real.txt") == 0)
        n_real++;
      else if (g_strcmp0 (name, "subdir") == 0)
        found_subdir = TRUE;

      g_object_unref (info);
    }

  g_assert_no_error (error);
  g_assert_cmpuint (n_virtual, ==, 1);
  g_assert_cmpuint (n_real, ==, 1);
  g_assert_true (found_subdir);
  g_object_unref (e);

  /* the virtual directory itself is empty */
  e = g_file_enumerate_children (G_FILE (dir), "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &error);
  g_assert_no_error (error);
  info = g_file_enumerator_next_file (e, NULL, &error);
  g_assert_null (info);
  g_assert_no_error (error);
  g_object_unref (e);

  /* children resolve through the virtual tree, or fall through to
   * the real root */
  {
    GFile *child = g_file_get_child (G_FILE (f->root), "virtual");

    g_assert_true (PHODAV_IS_VIRTUAL_DIR (child));
    g_object_unref (child);

    child = g_file_get_child (G_FILE (f->root), "real.txt");
    g_assert_false (PHODAV_IS_VIRTUAL_DIR (child));
    g_object_unref (child);

    child = g_file_resolve_relative_path (G_FILE (f->root), "virtual");
    g_assert_true (PHODAV_IS_VIRTUAL_DIR (child));
    g_object_unref (child);

    child = g_file_resolve_relative_path (G_FILE (f->root), "real.txt");
    g_assert_false (PHODAV_IS_VIRTUAL_DIR (child));
    g_object_unref (child);
  }
}

static void
test_real_child (VdirFixture *f, gconstpointer user_data)
{
  g_autofree gchar *subdir = g_build_filename (f->dir, "subdir", NULL);
  g_autoptr(PhodavVirtualDir) dir = NULL;
  g_autoptr(GFile) real = NULL;
  g_autoptr(GFile) child = NULL;
  g_autoptr(GError) error = NULL;

  dir = phodav_virtual_dir_new_dir (f->root, "/virtual", &error);
  g_assert_no_error (error);

  real = g_file_new_for_path (subdir);
  g_assert_true (phodav_virtual_dir_attach_real_child (dir, real));

  /* attaching the same child twice fails */
  g_assert_false (phodav_virtual_dir_attach_real_child (dir, real));

  /* the real child is reachable through the virtual directory */
  child = g_file_resolve_relative_path (G_FILE (dir), "subdir");
  g_assert_true (g_file_equal (child, real));

  /* ... but a virtual directory cannot be created under it */
  g_clear_pointer (&child, g_object_unref);
  child = G_FILE (phodav_virtual_dir_new_dir (f->root, "/virtual/subdir/vdir", &error));
  g_assert_null (child);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
}

static void
test_new_dir_errors (VdirFixture *f, gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(PhodavVirtualDir) dir = NULL;

  /* relative paths are rejected */
  dir = phodav_virtual_dir_new_dir (f->root, "relative", &error);
  g_assert_null (dir);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME);
  g_clear_error (&error);

  /* missing parents are rejected */
  dir = phodav_virtual_dir_new_dir (f->root, "/missing/child", &error);
  g_assert_null (dir);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  /* duplicates are rejected */
  dir = phodav_virtual_dir_new_dir (f->root, "/virtual", &error);
  g_assert_no_error (error);
  g_clear_pointer (&dir, g_object_unref);

  {
    PhodavVirtualDir *dup = phodav_virtual_dir_new_dir (f->root, "/virtual", &error);

    g_assert_null (dup);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error (&error);
  }
}

static void
test_root_real (VdirFixture *f, gconstpointer user_data)
{
  g_autofree gchar *subdir = g_build_filename (f->dir, "subdir", NULL);
  g_autoptr(GFile) real = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *path = NULL;

  /* the real root can be retrieved while it is set */
  real = phodav_virtual_dir_root_get_real (f->root);
  g_assert_nonnull (real);
  path = g_file_get_path (real);
  g_assert_cmpstr (path, ==, f->dir);

  /* clearing it makes the root purely virtual */
  phodav_virtual_dir_root_set_real (f->root, NULL);
  g_clear_pointer (&real, g_object_unref);
  g_clear_pointer (&path, g_free);

  real = phodav_virtual_dir_root_get_real (f->root);
  g_assert_null (real);

  path = g_file_get_path (G_FILE (f->root));
  g_assert_cmpstr (path, ==, "/");

  /* setting it again restores the real children */
  phodav_virtual_dir_root_set_real (f->root, f->dir);
  real = phodav_virtual_dir_root_get_real (f->root);
  g_assert_nonnull (real);
  g_clear_pointer (&path, g_free);

  {
    g_autoptr(GFile) resolved = g_file_resolve_relative_path (G_FILE (f->root), "subdir");

    g_clear_pointer (&path, g_free);
    path = g_file_get_path (resolved);
    g_assert_cmpstr (path, ==, subdir);
  }
}

static void
test_parent_gone (VdirFixture *f, gconstpointer user_data)
{
  g_autoptr(PhodavVirtualDir) dir = NULL;
  g_autoptr(GError) error = NULL;
  PhodavVirtualDir *old_root = f->root;
  GFile *parent = NULL;

  dir = phodav_virtual_dir_new_dir (f->root, "/virtual", &error);
  g_assert_no_error (error);

  /* drop every reference to the root, keeping only the child; the
   * fixture keeps working with a fresh root */
  f->root = phodav_virtual_dir_new_root ();
  g_object_unref (old_root);

  /* the child survives, but turns into a dummy */
  parent = g_file_get_parent (G_FILE (dir));
  g_assert_nonnull (parent);
  g_object_unref (parent);

  {
    g_autoptr(GFileInfo) info = NULL;

    info = g_file_query_info (G_FILE (dir), "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &error);
    g_assert_null (info);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error (&error);

    info = g_file_query_filesystem_info (G_FILE (dir), "filesystem::*", NULL, &error);
    g_assert_null (info);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error (&error);
  }
}

gint
main (gint argc, gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/virtual-dir-file/names", VdirFixture, NULL, vdir_fixture_setup, test_names, vdir_fixture_teardown);
  g_test_add ("/virtual-dir-file/query-info", VdirFixture, NULL, vdir_fixture_setup, test_query_info, vdir_fixture_teardown);
  g_test_add ("/virtual-dir-file/enumerate", VdirFixture, NULL, vdir_fixture_setup, test_enumerate, vdir_fixture_teardown);
  g_test_add ("/virtual-dir-file/real-child", VdirFixture, NULL, vdir_fixture_setup, test_real_child, vdir_fixture_teardown);
  g_test_add ("/virtual-dir-file/new-dir-errors", VdirFixture, NULL, vdir_fixture_setup, test_new_dir_errors, vdir_fixture_teardown);
  g_test_add ("/virtual-dir-file/root-real", VdirFixture, NULL, vdir_fixture_setup, test_root_real, vdir_fixture_teardown);
  g_test_add ("/virtual-dir-file/parent-gone", VdirFixture, NULL, vdir_fixture_setup, test_parent_gone, vdir_fixture_teardown);

  return g_test_run ();
}
