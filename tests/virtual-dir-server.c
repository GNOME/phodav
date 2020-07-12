/*
 * Copyright (C) 2020 Red Hat, Inc.
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
#include <stdlib.h>
#include <glib.h>

#include "libphodav/phodav.h"

#define PORT 8080

static void
create_test_file (const gchar *path)
{
  GFile *f = g_file_new_for_path (path);
  GFileOutputStream *out = g_file_replace (f, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL);
  g_output_stream_printf (G_OUTPUT_STREAM (out), NULL, NULL, NULL, "test data");
  g_object_unref (out);
  g_object_unref (f);
}

/* Taken from phodav-method-delete.c and simplified */
static void
delete_file_recursive (GFile *file)
{
  GFileEnumerator *e;
  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (e)
    {
      while (TRUE)
        {
          GFileInfo *info = g_file_enumerator_next_file (e, NULL, NULL);
          if (!info)
            break;
          GFile *del = g_file_get_child (file, g_file_info_get_name (info));
          delete_file_recursive (del);
          g_object_unref (del);
          g_object_unref (info);
        }

      g_file_enumerator_close (e, NULL, NULL);
      g_clear_object (&e);
    }

  g_file_delete (file, NULL, NULL);
}

/* This is basically a very stripped-down version of chezdav
 * for the purpose of testing PhodavVirtualDir.
 *
 * It creates a folder that acts as the "real" root and one virtual dir "/virtual"
 * inside it - so the setup is similar to the one used by spice-gtk */
int
main (int argc, char *argv[])
{
  GFile *root_dir = g_file_new_for_path ("./phodav-virtual-root");
  GFile *real_dir = g_file_get_child (root_dir, "real");

  /* clean-up after any previous tests */
  delete_file_recursive (root_dir);

  /* setup local dir structure */
  g_assert_true (g_file_make_directory (root_dir, NULL, NULL));
  g_assert_true (g_file_make_directory (real_dir, NULL, NULL));
  create_test_file ("./phodav-virtual-root/test.txt");

  /* setup virtual dir structure */
  PhodavVirtualDir *root = phodav_virtual_dir_new_root ();
  phodav_virtual_dir_root_set_real (root, "./phodav-virtual-root");
  PhodavVirtualDir *virtual_dir = phodav_virtual_dir_new_dir (root, "/virtual", NULL);
  phodav_virtual_dir_attach_real_child (virtual_dir, real_dir);

  PhodavServer *phodav = phodav_server_new_for_root_file (G_FILE(root));

  g_object_unref (virtual_dir);
  g_object_unref (real_dir);
  g_object_unref (root_dir);
  g_object_unref (root);

  SoupServer *server = phodav_server_get_soup_server (phodav);
  GError *error = NULL;
  if (!soup_server_listen_all (server, PORT, 0, &error))
    {
      g_printerr ("Failed to listen on port %d: %s\n", PORT, error->message);
      g_error_free (error);
      return 1;
    }

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  g_object_unref (phodav);

  return 0;
}
