/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <stdlib.h>

#include <glib.h>

#include "libphodav/phodav.h"

static void
create_test_file (const gchar *path)
{
  g_autoptr(GFile) f = g_file_new_for_path (path);
  g_autoptr(GFileOutputStream) out = g_file_replace (f, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL);
  g_output_stream_printf (G_OUTPUT_STREAM (out), NULL, NULL, NULL, "test data");
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

static gboolean
reject_auth (SoupAuthDomain *domain, SoupServerMessage *msg, const gchar *username, const gchar *password, gpointer data)
{
  return FALSE;
}

static gboolean
stdin_watch (GIOChannel *source, GIOCondition condition, gpointer data)
{
  g_debug ("stdin condition %d, quitting", condition);
  GMainLoop *loop = data;
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

/* This is basically a very stripped-down version of chezdav
 * for the purpose of testing PhodavVirtualDir.
 *
 * It creates a folder that acts as the "real" root and one virtual dir "/virtual"
 * inside it - so the setup is similar to the one used by spice-gtk.
 * The "/protected" folder requires authentication that always fails. */
gint
main (gint argc, gchar *argv[])
{
  gboolean quit_on_stdin = FALSE;
  if (argc >= 2 && g_strcmp0 (argv[1], "--quit-on-stdin") == 0)
    {
      quit_on_stdin = TRUE;
    }

  g_autoptr(GFile) root_dir = g_file_new_for_path ("./phodav-virtual-root");
  g_autoptr(GFile) real_dir = g_file_get_child (root_dir, "real");
  g_autoptr(GFile) protected_dir = g_file_get_child (root_dir, "protected");

  /* clean-up after any previous tests */
  delete_file_recursive (root_dir);

  /* setup local dir structure */
  g_assert_true (g_file_make_directory (root_dir, NULL, NULL));
  g_assert_true (g_file_make_directory (real_dir, NULL, NULL));
  g_assert_true (g_file_make_directory (protected_dir, NULL, NULL));
  create_test_file ("./phodav-virtual-root/test.txt");

  /* setup virtual dir structure */
  g_autoptr(PhodavVirtualDir) root = phodav_virtual_dir_new_root ();
  phodav_virtual_dir_root_set_real (root, "./phodav-virtual-root");
  g_autoptr(PhodavVirtualDir) virtual_dir = phodav_virtual_dir_new_dir (root, "/virtual", NULL);
  phodav_virtual_dir_attach_real_child (virtual_dir, real_dir);

  g_autoptr(PhodavServer) phodav = phodav_server_new_for_root_file (G_FILE (root));

  SoupServer *server = phodav_server_get_soup_server (phodav);
  g_autoptr(GError) error = NULL;

  /* nobody may access /protected */
  g_autoptr(SoupAuthDomain) auth = soup_auth_domain_basic_new ("realm", "test", NULL);
  soup_auth_domain_add_path (auth, "/protected");
  soup_auth_domain_basic_set_auth_callback (auth, reject_auth, NULL, NULL);
  soup_server_add_auth_domain (server, auth);

  /* let the system pick a free port */
  if (!soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error))
    {
      g_printerr ("Failed to listen: %s\n", error->message);
      return 1;
    }

  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);

  if (quit_on_stdin)
    {
      g_autoptr(GIOChannel) stdin_channel = NULL;
#ifdef G_OS_WIN32
      stdin_channel = g_io_channel_win32_new_fd (0);
#else
      stdin_channel = g_io_channel_unix_new (0);
#endif
      g_io_add_watch (stdin_channel, G_IO_IN | G_IO_HUP | G_IO_ERR, stdin_watch, loop);
    }

  g_autoslist(GUri) uris = soup_server_get_uris (server);
  g_assert_nonnull (uris);
  g_print ("OK %d\n", g_uri_get_port (uris->data));

  g_main_loop_run (loop);

  return 0;
}
