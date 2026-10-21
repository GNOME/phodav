/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/* The server the fuzz targets send their requests to.
 *
 * A real PhodavServer is created on the default main context, over
 * a temporary directory, and the requests are fed to its SoupServer
 * with soup_server_accept_iostream() over a socketpair: no socket is
 * listened on and nothing leaves the process, but the whole request
 * path is exercised, from the libsoup request parsing to the DAV
 * methods and the response serialization.
 *
 * The requests are processed while the response is being drained, by
 * iterating the default main context from the (single) fuzzer
 * thread, like the context of a regularly running server would. */

#include "config.h"

#include "fuzz-server.h"

#include <errno.h>

#include <sys/socket.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "libphodav/phodav.h"

/* how long to wait for the server to answer a request, at most */
#define RESPONSE_TIMEOUT_SECONDS 2

/* how long the response drain waits for the socket between two main
 * context iterations */
#define POLL_TIMEOUT_MS 20

static struct
{
  PhodavServer *phodav;
  gchar *dir;
  gboolean initialized;
} fuzz_server;

static void
log_ignore (const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer user_data)
{
}

static void
fixture_setup (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *test = NULL;
  g_autofree gchar *dir = NULL;
  g_autofree gchar *subdir = NULL;
  g_autofree gchar *file = NULL;

  dir = g_dir_make_tmp ("phodav-fuzz-XXXXXX", &error);
  g_assert_no_error (error);

  test = g_build_filename (dir, "test", NULL);
  g_assert_true (g_file_set_contents (test, "fuzz", -1, NULL));

  subdir = g_build_filename (dir, "dir", NULL);
  g_assert_cmpint (g_mkdir (subdir, 0700), ==, 0);

  file = g_build_filename (subdir, "file", NULL);
  g_assert_true (g_file_set_contents (file, "fuzz", -1, NULL));

  fuzz_server.dir = g_steal_pointer (&dir);
  fuzz_server.phodav = phodav_server_new (fuzz_server.dir);
}

static void
remove_recursive (GFile *file)
{
  g_autoptr(GFileEnumerator) e = NULL;

  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (e)
    {
      while (TRUE)
        {
          GFileInfo *info = g_file_enumerator_next_file (e, NULL, NULL);

          if (!info)
            break;

          {
            GFile *child = g_file_get_child (file, g_file_info_get_name (info));

            remove_recursive (child);
            g_object_unref (child);
          }
          g_object_unref (info);
        }
    }

  g_file_delete (file, NULL, NULL);
}

void
fuzz_server_reset (void)
{
  g_autoptr(GFile) root = NULL;

  if (!fuzz_server.initialized)
    return;

  /* no request is in flight and the context is quiet at this point,
   * so the server can be dropped with its locks and its paths */
  g_clear_object (&fuzz_server.phodav);

  root = g_file_new_for_path (fuzz_server.dir);
  remove_recursive (root);
  g_clear_pointer (&fuzz_server.dir, g_free);

  fixture_setup ();
}

static void
fuzz_server_ensure (void)
{
  if (fuzz_server.initialized)
    return;

  /* the per-request messages of the library would drown the fuzzer
   * output; the warnings are kept, they may point at a bug */
  g_log_set_handler ("phodav", G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_MESSAGE, log_ignore, NULL);

  fixture_setup ();
  fuzz_server.initialized = TRUE;
}

void
fuzz_server_send (const guint8 *request, gsize length)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GSocket) client = NULL;
  g_autoptr(GSocket) server_socket = NULL;
  g_autoptr(GSocketConnection) connection = NULL;
  g_autoptr(GSocketAddress) local_addr = NULL;
  g_autoptr(GSocketAddress) remote_addr = NULL;
  gint64 deadline;
  gint fds[2];

  fuzz_server_ensure ();

  if (socketpair (AF_UNIX, SOCK_STREAM, 0, fds) != 0)
    g_error ("socketpair failed: %s", g_strerror (errno));

  client = g_socket_new_from_fd (fds[0], &error);
  g_assert_no_error (error);
  server_socket = g_socket_new_from_fd (fds[1], &error);
  g_assert_no_error (error);

  connection = g_socket_connection_factory_create_connection (server_socket);

  /* the addresses are only reported back in the message getters, but
   * they may not be NULL */
  local_addr = g_inet_socket_address_new_from_string ("127.0.0.1", 9876);
  remote_addr = g_inet_socket_address_new_from_string ("127.0.0.1", 54321);

  if (!soup_server_accept_iostream (phodav_server_get_soup_server (fuzz_server.phodav),
                                    G_IO_STREAM (connection), local_addr, remote_addr, &error))
    g_error ("accept_iostream failed: %s", error->message);

  g_socket_send (client, (const gchar *) request, length, NULL, NULL);
  /* the end of the requests: the server answers what it got and
   * closes its end of the connection */
  g_socket_shutdown (client, FALSE, TRUE, NULL);

  /* the server processes the request on the default main context
   * while the response is drained */
  deadline = g_get_monotonic_time () + RESPONSE_TIMEOUT_SECONDS * G_USEC_PER_SEC;

  while (g_get_monotonic_time () < deadline)
    {
      GPollFD pfd = {
        .fd = g_socket_get_fd (client),
        .events = G_IO_IN,
      };

      while (g_main_context_pending (NULL))
        g_main_context_iteration (NULL, FALSE);

      if (g_poll (&pfd, 1, POLL_TIMEOUT_MS) == 0 || !(pfd.revents & (G_IO_IN | G_IO_HUP | G_IO_ERR)))
        continue;

      {
        gchar buf[4096];

        if (g_socket_receive (client, buf, sizeof (buf), NULL, NULL) <= 0)
          break; /* end of the response */
      }
    }
}
