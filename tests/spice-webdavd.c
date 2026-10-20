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

/* Functional test of the spice-webdavd multiplexing daemon.
 *
 * The daemon normally talks to the spice virtio port; the --mux-path
 * option allows pointing it at any file instead.  A virtio port is a
 * bidirectional channel with independent directions, which a FIFO
 * cannot model (the daemon would read back its own writes), so the
 * test runs the daemon on the slave side of a pseudo terminal and
 * speaks the framing protocol on the master side:
 *
 *   client -> daemon:  [gint64 connection id][guint16 size][data]
 *   daemon -> client:  [gint64 connection id][guint16 size][data]
 *
 * The connection id is chosen by the daemon (the address of its
 * GSocketConnection); the test learns it from the first frame that
 * the daemon pushes. */

#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#define HELLO_FROM_MUX "hello from the mux"
#define HELLO_FROM_CLIENT "hello from the client"
#define FRAME_HEADER (sizeof (gint64) + sizeof (guint16))

static GSubprocess *daemon_;
static gint mux_fd = -1;

/* read exactly @len bytes from the mux channel, failing the test on
 * timeout, and returning the data */
static gchar *
mux_read (gsize len)
{
  gchar *buf = g_new0 (gchar, len + 1);
  gsize done = 0;

  while (done < len)
    {
      GPollFD pfd = { mux_fd, G_IO_IN };
      gint n = g_poll (&pfd, 1, 5000);

      g_assert_cmpint (n, >=, 0);
      if (n == 0)
        g_error ("timed out waiting for %zu bytes from the mux channel", len - done);

      n = read (mux_fd, buf + done, len - done);
      if (n < 0)
        g_error ("mux read failed: %s", g_strerror (errno));
      done += n;
    }

  return buf;
}

/* read one daemon frame: the connection id, the size and the data */
static gchar *
mux_read_frame (gint64 *id, gsize *size)
{
  gchar *header = mux_read (FRAME_HEADER);

  memcpy (id, header, sizeof (gint64));
  memcpy (size, header + sizeof (gint64), sizeof (guint16));
  g_free (header);

  return mux_read (*size);
}

/* write one frame to the mux channel */
static void
mux_write (const gchar *buf, gsize len)
{
  gsize done = 0;

  while (done < len)
    {
      gint n = write (mux_fd, buf + done, len - done);

      g_assert_cmpint (n, >, 0);
      done += n;
    }
}

static void
mux_write_frame (gint64 id, const gchar *data, gsize size)
{
  gchar header[FRAME_HEADER];

  memcpy (header, &id, sizeof (gint64));
  memcpy (header + sizeof (gint64), &size, sizeof (guint16));

  mux_write (header, sizeof (header));
  mux_write (data, size);
}

/* read exactly @len bytes from the client connection */
static void
client_read (GInputStream *in, gchar *buf, gsize len)
{
  gsize done = 0;

  while (done < len)
    {
      g_autoptr(GError) error = NULL;
      gssize n = g_input_stream_read (in, buf + done, len - done, NULL, &error);

      g_assert_no_error (error);
      g_assert_cmpint (n, >, 0);
      done += n;
    }
}

/* find a free TCP port by briefly listening on one */
static guint16
find_free_port (void)
{
  g_autoptr(GSocket) socket = NULL;
  g_autoptr(GInetAddress) iaddr = NULL;
  g_autoptr(GSocketAddress) saddr = NULL;
  g_autoptr(GSocketAddress) bound = NULL;
  guint16 port;

  socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL);
  g_assert_nonnull (socket);

  iaddr = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  saddr = g_inet_socket_address_new (iaddr, 0);

  g_assert_true (g_socket_bind (socket, saddr, FALSE, NULL));
  g_assert_true (g_socket_listen (socket, NULL));

  bound = g_socket_get_local_address (socket, NULL);
  port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound));

  /* close the socket, so that the port is free for the daemon */
  g_socket_close (socket, NULL);

  return port;
}

static void
test_mux (void)
{
  g_autofree gchar *pts_path = NULL;
  g_autofree gchar *port_str = NULL;
  g_autofree gchar *data = NULL;
  g_autoptr(GSocketClient) client = NULL;
  g_autoptr(GSocketConnection) connection = NULL;
  g_autoptr(GError) error = NULL;
  const gchar *daemon = g_getenv ("SPICE_WEBDAVD");
  struct termios raw;
  gchar buf[4096];
  gint64 id;
  gsize size;
  guint16 port;
  gint pts_slave_keep;

  g_assert_nonnull (daemon);

  /* the daemon multiplexes over the slave side of a pseudo terminal
   * instead of the virtio port; the slave is kept open on this side
   * as well, so that the terminal settings survive
   *
   * keep the slave end open in raw mode */
  mux_fd = posix_openpt (O_RDWR | O_NOCTTY);
  g_assert_cmpint (mux_fd, >, 0);
  g_assert_cmpint (grantpt (mux_fd), ==, 0);
  g_assert_cmpint (unlockpt (mux_fd), ==, 0);
  pts_path = g_strdup (ptsname (mux_fd));
  g_assert_nonnull (pts_path);

  /* binary frames, no echo, no line discipline mangling */
  g_assert_cmpint (tcgetattr (mux_fd, &raw), ==, 0);
  cfmakeraw (&raw);
  g_assert_cmpint (tcsetattr (mux_fd, TCSANOW, &raw), ==, 0);

  /* keep a slave descriptor open, so that the terminal settings are
   * not reset when the daemon (re)opens it */
  pts_slave_keep = g_open (pts_path, O_RDWR | O_NOCTTY);
  g_assert_cmpint (pts_slave_keep, >, 0);

  port = find_free_port ();
  port_str = g_strdup_printf ("%u", port);

  daemon_ = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error, daemon,
                              "--port", port_str,
                              "--mux-path", pts_path,
                              NULL);
  g_assert_no_error (error);

  /* a client connects to the daemon (the daemon may take a moment
   * to start listening, especially under load) */
  client = g_socket_client_new ();
  for (guint i = 0; i < 100; i++)
    {
      connection = g_socket_client_connect_to_host (client, "127.0.0.1", port, NULL, &error);
      if (connection)
        break;
      g_clear_error (&error);
      g_usleep (100 * 1000);
    }
  g_assert_no_error (error);
  g_assert_nonnull (connection);

  /* whatever the client sends is forwarded to the mux channel; the
   * frame also tells us the id the daemon assigned to the connection */
  {
    GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (connection));

    g_assert_true (g_output_stream_write_all (out, HELLO_FROM_CLIENT,
                                              strlen (HELLO_FROM_CLIENT), NULL, NULL, &error));
    g_assert_no_error (error);
  }

  data = mux_read_frame (&id, &size);
  g_assert_cmpuint (size, ==, strlen (HELLO_FROM_CLIENT));
  g_assert_cmpstr (data, ==, HELLO_FROM_CLIENT);

  /* and whatever is sent to the mux channel is forwarded to the client */
  mux_write_frame (id, HELLO_FROM_MUX, strlen (HELLO_FROM_MUX));

  {
    GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (connection));

    client_read (in, buf, strlen (HELLO_FROM_MUX));
    buf[strlen (HELLO_FROM_MUX)] = '\0';
    g_assert_cmpstr (buf, ==, HELLO_FROM_MUX);
  }

  /* closing the connection makes the daemon drop the client, with a
   * final empty frame */
  g_clear_object (&connection);

  g_clear_pointer (&data, g_free);
  data = mux_read_frame (&id, &size);
  g_assert_cmpuint (size, ==, 0);

  /* SIGINT shuts the daemon down cleanly */
  g_subprocess_send_signal (daemon_, SIGINT);
  g_assert_true (g_subprocess_wait (daemon_, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_subprocess_get_if_exited (daemon_));
  g_assert_cmpint (g_subprocess_get_exit_status (daemon_), ==, 0);
  g_clear_object (&daemon_);

  close (mux_fd);
  mux_fd = -1;
  close (pts_slave_keep);
}

gint
main (gint argc, gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/spice-webdavd/mux", test_mux);

  return g_test_run ();
}
