/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#ifdef G_OS_WIN32
#include <signal.h>

#include "drive-mapping.h"
#include "win32-service.h"
#endif

#ifdef WITH_AVAHI
#include "avahi-common.h"
#endif

#include "spice-mux.h"

/* the default mux channel: the spice WebDAV virtio port */
#ifdef __APPLE__
#define DEFAULT_MUX_PATH "/dev/tty.org.spice-space.webdav.0"
#elif defined(G_OS_WIN32)
#define DEFAULT_MUX_PATH "\\\\.\\Global\\org.spice-space.webdav.0"
#else
#define DEFAULT_MUX_PATH "/dev/virtio-ports/org.spice-space.webdav.0"
#endif

/* the delay before the mux channel is reopened, when it broke, in
 * seconds */
#define RECONNECT_DELAY 1

/* the daemon: the client connections are multiplexed over the mux
 * channel, which is reopened when it breaks, until the daemon is
 * asked to stop */
typedef struct _Daemon
{
  gchar *mux_path;

  /* the live mux, and the pending source reopening it when it broke;
   * borrowed pointers, owned by run_daemon() and read by the stop
   * handlers */
  SpiceMux *mux;
  guint retry_id;
  GMainLoop *loop;

  /* set when the daemon must stop; read from the main loop, written
   * by the signal source (the main thread) or the Windows service
   * control handler (its own thread) */
  gint stopped;
} Daemon;

/* the daemon of the process, for the handlers which take no user
 * data */
static Daemon *the_daemon;

/* ask the daemon to stop: leave the main loop of the daemon, the mux
 * channel is not reopened afterwards */
static void
daemon_stop (gpointer user_data)
{
  Daemon *daemon = user_data;

  g_debug ("stopping the daemon");

  g_atomic_int_set (&daemon->stopped, TRUE);

  if (daemon->loop)
    g_main_loop_quit (daemon->loop);
}

static void daemon_open_mux (Daemon *daemon);

/* the retry of a mux channel which could not be opened */
static void
open_mux_retry_cb (gpointer user_data)
{
  Daemon *daemon = user_data;

  daemon->retry_id = 0;

  if (g_atomic_int_get (&daemon->stopped))
    return;

  daemon_open_mux (daemon);
}

/* the mux channel broke (or the mux was stopped): drop it, and
 * reopen it after a delay unless the daemon is stopping */
static void
mux_closed_cb (SpiceMux *mux, gpointer user_data)
{
  Daemon *daemon = user_data;

  daemon->mux = NULL;
  g_object_unref (mux);

  /* let the operations cancelled by the close complete, they release
   * the last references on the mux and its clients */
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  if (!g_atomic_int_get (&daemon->stopped))
    daemon->retry_id = g_timeout_add_seconds_once (RECONNECT_DELAY, open_mux_retry_cb, daemon);
}

static void
daemon_open_mux (Daemon *daemon)
{
  SpiceMux *mux;
  g_autoptr(GError) error = NULL;

  mux = spice_mux_new (daemon->mux_path, &error);
  if (mux)
    {
      daemon->mux = mux;
      g_signal_connect (mux, "closed", G_CALLBACK (mux_closed_cb), daemon);
      return;
    }

  /* the channel cannot be opened (the device is not there yet?):
   * retry after a delay */
  g_warning ("Failed to open the mux channel: %s", error->message);
  daemon->retry_id = g_timeout_add_seconds_once (RECONNECT_DELAY, open_mux_retry_cb, daemon);
}

static gboolean
incoming_callback (GSocketService *service,
                   GSocketConnection *client_connection,
                   GObject *source_object,
                   gpointer user_data)
{
  Daemon *daemon = user_data;

  /* the mux channel is being reopened: the connection cannot be
   * multiplexed, drop it */
  if (!daemon->mux)
    return FALSE;

  spice_mux_add_client (daemon->mux, client_connection);

  return FALSE;
}

#ifdef G_OS_UNIX
static gboolean
signal_handler (gpointer user_data)
{
  g_debug ("quit SIGINT");

  daemon_stop (user_data);

  return G_SOURCE_REMOVE;
}
#endif

#ifdef G_OS_WIN32
static void
console_signal_handler (gint sig)
{
  g_debug ("quit %d", sig);

  daemon_stop (the_daemon);
}
#endif

/* run the daemon until it is asked to stop: the mux channel is
 * reopened when it breaks on its own (the guest was disconnected...),
 * dropping the connected clients with it, while the HTTP server keeps
 * listening and the announced service stays up. */
static void
run_daemon (gpointer user_data)
{
  Daemon *daemon = user_data;

  g_debug ("Run service");

  daemon->loop = g_main_loop_new (NULL, TRUE);

  daemon_open_mux (daemon);

  g_main_loop_run (daemon->loop);

  /* the daemon was asked to stop: drop the pending retry and the live
   * mux, if any (its close drops it) */
  g_clear_handle_id (&daemon->retry_id, g_source_remove);
  if (daemon->mux)
    spice_mux_stop (daemon->mux);

  g_clear_pointer (&daemon->loop, g_main_loop_unref);
}

gint
main (gint argc, gchar *argv[])
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GOptionContext) opts = NULL;
  g_autoptr(GSocketService) socket_service = NULL;
  g_autoptr(GInetAddress) iaddr = NULL;
  g_autoptr(GSocketAddress) saddr = NULL;
  g_autofree Daemon *daemon = NULL;
#ifdef G_OS_WIN32
  g_autoptr(SharedDrive) drive = NULL;
#endif

  /* the parsed filename option is an owned copy */
  g_autofree gchar *mux_path = NULL;

  /* the command line options, parsed in place by GOption: they are
   * plain locals of main(), which outlives the option context */
  gint port = 0;
#ifdef G_OS_WIN32
  gboolean no_service = FALSE;
#endif

  GOptionEntry entries[] = {
    { "port", 'p', 0,
      G_OPTION_ARG_INT, &port,
      "Port to listen on", NULL },
    { "mux-path", 0, 0,
      G_OPTION_ARG_FILENAME, &mux_path,
      "Path of the multiplexing channel (default: the spice WebDAV virtio port)", "PATH" },
#ifdef G_OS_WIN32
    { "no-service", 0, 0,
      G_OPTION_ARG_NONE, &no_service,
      "Don't start as a service", NULL },
#endif
    { NULL }
  };

  opts = g_option_context_new (NULL);
  g_option_context_add_main_entries (opts, entries, NULL);
  if (!g_option_context_parse (opts, &argc, &argv, &error))
    {
      g_printerr ("Could not parse arguments: %s\n",
                  error->message);
      g_printerr ("%s",
                  g_option_context_get_help (opts, TRUE, NULL));
      exit (1);
    }
  if (port == 0)
    {
      g_printerr ("please specify a valid port\n");
      exit (1);
    }

  daemon = g_new0 (Daemon, 1);
  the_daemon = daemon;
  daemon->mux_path = mux_path ? g_steal_pointer (&mux_path) : g_strdup (DEFAULT_MUX_PATH);

#ifdef G_OS_UNIX
  g_unix_signal_add (SIGINT, signal_handler, daemon);
#endif

  /* the socket service is kept for the whole lifetime of the daemon:
   * there seems to be a bug on Windows, where it cannot accept new
   * connections anymore after being cleaned up and restarted */
  socket_service = g_socket_service_new ();
  iaddr = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  saddr = g_inet_socket_address_new (iaddr, port);

  /* listen on the port for the incoming clients, multiplexing them
   * over the mux channel */
  if (!g_socket_listener_add_address (G_SOCKET_LISTENER (socket_service),
                                      saddr,
                                      G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_TCP,
                                      NULL,
                                      NULL,
                                      &error))
    {
      g_printerr ("%s\n", error->message);
      exit (1);
    }

  g_signal_connect (socket_service,
                    "incoming", G_CALLBACK (incoming_callback),
                    daemon);
  g_socket_service_start (socket_service);

#ifdef WITH_AVAHI
  if (!avahi_client_start ("Spice client folder", port, TRUE, FALSE, &error))
    {
      g_printerr ("%s\n", error->message);
      exit (1);
    }
#endif

#ifdef G_OS_WIN32
  /* map the shared folder over the HTTP server of the daemon */
  drive = shared_drive_new ();
  shared_drive_map (drive, port);

  if (no_service || g_getenv ("DEBUG"))
    {
      signal (SIGINT, console_signal_handler);
      run_daemon (daemon);
    }
  else
    {
      /* this blocks until the service is stopped, when the daemon was
       * asked to stop and the shared folder was unmapped by its
       * control handler */
      win32_service_run (drive, run_daemon, daemon_stop, daemon);
    }

  /* the shared folder is unmapped by the service control handler
   * already; this is for the console mode */
  shared_drive_unmap (drive);
#else
  run_daemon (daemon);
#endif

#ifdef WITH_AVAHI
  avahi_client_stop ();
#endif

  g_free (daemon->mux_path);

  return 0;
}
