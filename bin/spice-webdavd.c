/*
 * Copyright (C) 2013 Red Hat, Inc.
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
#include "config.h"

#include <stdlib.h>
#include <gio/gio.h>

#ifdef G_OS_UNIX
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <errno.h>
#endif

#ifdef G_OS_WIN32
#include <gio/gwin32inputstream.h>
#include <gio/gwin32outputstream.h>
#include <windows.h>
#define SERVICE_NAME "spice-webdavd"
#endif

#ifdef WITH_AVAHI
#include "avahi-common.h"
#endif

#include "output-queue.h"

typedef struct _ServiceData
{
#ifdef G_OS_WIN32
  gchar drive_letter;
  GMutex mutex;
#endif
} ServiceData;

static GCancellable *cancel;

static struct _DemuxData
{
  gint64  client;
  guint16 size;
  gchar   buf[G_MAXUINT16];
} demux;

typedef struct _Client
{
  guint              ref_count;
  struct
  {
    gint64             id;
    guint16            size;
    guint8             buf[G_MAXUINT16];
  } mux;
  GSocketConnection *client_connection;
} Client;

static volatile gboolean quit_service;
static GMainLoop *loop;
static GInputStream *mux_istream;
static GOutputStream *mux_ostream;
static OutputQueue *mux_queue;
static GHashTable *clients;
static GSocketService *socket_service;
#ifdef G_OS_UNIX
static gint port_fd;
#elif defined(G_OS_WIN32)
static HANDLE port_handle;
#endif

static void start_mux_read (GInputStream *istream);

static void
quit (int sig)
{
  g_debug ("quit %d", sig);

  if (sig == SIGINT || sig == SIGTERM)
      quit_service = TRUE;

  if (loop)
    g_main_loop_quit (loop);
}

#ifdef G_OS_UNIX
static gboolean
signal_handler (gpointer user_data)
{
  quit(SIGINT);
  return G_SOURCE_REMOVE;
}
#endif

static Client *
add_client (GSocketConnection *client_connection)
{
  Client *client;
  client = g_new0 (Client, 1);
  client->ref_count = 1;
  client->client_connection = g_object_ref (client_connection);
  // TODO: check if usage of this idiom is portable, or if we need to check collisions
  client->mux.id = GPOINTER_TO_INT (client_connection);
  g_hash_table_insert (clients, &client->mux.id, client);
  g_warn_if_fail (g_hash_table_lookup (clients, &client->mux.id));

  return client;
}

static Client *
client_ref (Client *c)
{
  c->ref_count++;
  return c;
}

static void
client_unref (gpointer user_data)
{
  Client *c = user_data;
  if (--c->ref_count > 0)
    return;

  g_debug ("Free client %" G_GINT64_FORMAT, c->mux.id);

  g_io_stream_close (G_IO_STREAM (c->client_connection), NULL, NULL);
  g_object_unref (c->client_connection);
  g_free (c);
}

static void
remove_client (Client *client)
{
  g_debug ("remove client %" G_GINT64_FORMAT, client->mux.id);

  g_hash_table_remove (clients, &client->mux.id);
}

static void
mux_pushed_client_cb (GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
{
  Client *client = user_data;
  GError *error = NULL;
  g_output_stream_write_all_finish (G_OUTPUT_STREAM (source_object), res, NULL, &error);

  if (error)
    {
      g_warning ("error pushing to client %" G_GINT64_FORMAT ": %s",
        client->mux.id, error->message);
      g_error_free (error);
      remove_client (client);
    }
  client_unref (client);

  start_mux_read (mux_istream);
}

static void
mux_data_read_cb (GObject      *source_object,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  GError *error = NULL;
  gsize size;

  g_input_stream_read_all_finish (G_INPUT_STREAM (source_object), res, &size, &error);
  g_debug ("read %" G_GSIZE_FORMAT " bytes from mux", size);
  if (error)
    {
      g_warning ("%s: error: %s", __FUNCTION__, error->message);
      g_clear_error (&error);
    }
  if (size != demux.size)
    {
      quit (-1);
      return;
    }

  Client *c = g_hash_table_lookup (clients, &demux.client);
  g_debug ("looked up client %" G_GINT64_FORMAT ": %p", demux.client, c);

  if (c)
    {
      GOutputStream *out;
      out = g_io_stream_get_output_stream (G_IO_STREAM (c->client_connection));
      g_output_stream_write_all_async (out, demux.buf, demux.size,
        G_PRIORITY_DEFAULT, cancel, mux_pushed_client_cb, client_ref (c));
    }
  else
    start_mux_read (mux_istream);
}

static void
mux_size_read_cb (GObject      *source_object,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  GInputStream *istream = G_INPUT_STREAM (source_object);
  GError *error = NULL;
  gsize size;

  g_input_stream_read_all_finish (G_INPUT_STREAM (source_object), res, &size, &error);
  if (error || size != sizeof (guint16))
    goto end;

  g_input_stream_read_all_async (istream,
                                 &demux.buf, demux.size, G_PRIORITY_DEFAULT,
                                 cancel, mux_data_read_cb, NULL);
  return;

end:
  if (error)
    {
      g_warning ("%s: error: %s", __FUNCTION__, error->message);
      g_clear_error (&error);
    }

  quit (-2);
}

static void
mux_client_read_cb (GObject      *source_object,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  GInputStream *istream = G_INPUT_STREAM (source_object);
  GError *error = NULL;
  gsize size;

  g_input_stream_read_all_finish (G_INPUT_STREAM (source_object), res, &size, &error);
  if (error || size != sizeof (gint64))
    goto end;
  g_input_stream_read_all_async (istream,
                                 &demux.size, sizeof (guint16), G_PRIORITY_DEFAULT,
                                 cancel, mux_size_read_cb, NULL);
  return;

end:
  if (error)
    {
      g_warning ("%s: error: %s", __FUNCTION__, error->message);
      g_clear_error (&error);
    }

  quit (-3);
}

static void
start_mux_read (GInputStream *istream)
{
  g_debug ("start reading mux");
  g_input_stream_read_all_async (istream,
                                 &demux.client, sizeof (gint64), G_PRIORITY_DEFAULT,
                                 cancel, mux_client_read_cb, NULL);
}

static void client_start_read (Client *client);

static void
mux_pushed_cb (OutputQueue *q, gpointer user_data, GError *error)
{
  Client *client = user_data;

  if (error)
    {
      g_warning ("error pushing to mux from client %" G_GINT64_FORMAT ": %s",
        client->mux.id, error->message);
      remove_client (client);
      goto end;
    }

  if (client->mux.size == 0)
    {
      remove_client (client);
      goto end;
    }

  client_start_read (client);
end:
  client_unref (client);
}

static void
client_read_cb (GObject      *source_object,
                GAsyncResult *res,
                gpointer      user_data)
{
  Client *client = user_data;
  GError *error = NULL;
  gssize size;

  size = g_input_stream_read_finish (G_INPUT_STREAM (source_object), res, &error);
  g_debug ("read %" G_GSSIZE_FORMAT " bytes from client %" G_GINT64_FORMAT, size, client->mux.id);
  if (error)
    {
      g_warning ("%s: error for client %" G_GINT64_FORMAT ": %s",
        __FUNCTION__, client->mux.id, error->message);
      g_clear_error (&error);
      remove_client (client);
      client_unref (client);
      return;
    }

  g_return_if_fail (size <= G_MAXUINT16);
  g_return_if_fail (size >= 0);
  client->mux.size = size;

  output_queue_push (mux_queue, (guint8 *) &client->mux,
    sizeof (gint64) + sizeof (guint16) + size, mux_pushed_cb, client);
}

static void
client_start_read (Client *client)
{
  GIOStream *iostream = G_IO_STREAM (client->client_connection);
  GInputStream *istream = g_io_stream_get_input_stream (iostream);

  g_debug ("start read client %" G_GINT64_FORMAT, client->mux.id);
  g_input_stream_read_async (istream,
                             client->mux.buf, G_MAXUINT16, G_PRIORITY_DEFAULT,
                             cancel, client_read_cb, client_ref (client));
}

static gboolean
incoming_callback (GSocketService    *service,
                   GSocketConnection *client_connection,
                   GObject           *source_object,
                   gpointer           user_data)
{
  Client *client;

  client = add_client (client_connection);
  g_debug ("new client %" G_GINT64_FORMAT, client->mux.id);
  client_start_read (client);

  return FALSE;
}

static int port;

#ifdef G_OS_WIN32
static gboolean no_service;
#endif

#ifdef G_OS_UNIX
static void
wait_for_virtio_host (gint fd)
{
    GPollFD pfd = { .fd = fd, .events = G_IO_HUP | G_IO_IN | G_IO_OUT };

    while (1)
      {
        gboolean connected;

        g_debug ("polling");
        g_assert_cmpint (g_poll (&pfd, 1, -1), ==, 1);
        g_debug ("polling end");
        connected = !(pfd.revents & G_IO_HUP);
        g_debug ("connected: %d", connected);
        if (connected)
          break;

        // FIXME: I don't see a reasonable way to wait for virtio
        // host-side to be connected..
        g_usleep (G_USEC_PER_SEC);
      }
}
#endif

static void
open_mux_path (const char *path)
{
  g_return_if_fail (path);
  g_return_if_fail (!mux_istream);
  g_return_if_fail (!mux_ostream);
  g_return_if_fail (!mux_queue);

  g_debug ("Open %s", path);
#ifdef G_OS_UNIX
  port_fd = g_open (path, O_RDWR);
  gint errsv = errno;
  if (port_fd == -1)
    {
      g_printerr("Failed to open %s: %s\n", path, g_strerror(errsv));
      exit (1);
    }

  wait_for_virtio_host (port_fd);

  mux_ostream = g_unix_output_stream_new (port_fd, TRUE);
  mux_istream = g_unix_input_stream_new (port_fd, FALSE);
#else
  port_handle = CreateFile (path,
                         GENERIC_WRITE | GENERIC_READ,
                         0,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL);

  if (port_handle == INVALID_HANDLE_VALUE)
      g_error ("%s", g_win32_error_message (GetLastError ()));

  mux_ostream = G_OUTPUT_STREAM (g_win32_output_stream_new (port_handle, TRUE));
  mux_istream = G_INPUT_STREAM (g_win32_input_stream_new (port_handle, TRUE));
#endif

  mux_queue = output_queue_new (G_OUTPUT_STREAM (mux_ostream), cancel);
}

#ifdef G_OS_WIN32
#define MAX_SHARED_FOLDER_NAME_SIZE 64
#define MAX_DRIVE_LETTER_SIZE 3

typedef struct _MapDriveData
{
  ServiceData *service_data;
  GCancellable *cancel_map;
} MapDriveData;

static gchar
get_free_drive_letter(void)
{
  const guint32 max_mask = 1 << 25;
  guint32 drives;
  gint i;

  drives = GetLogicalDrives ();
  if (drives == 0)
    {
      g_warning ("%s", g_win32_error_message (GetLastError ()));
      return 0;
    }

  for (i = 0; i < 26; i++)
    {
      guint32 mask = max_mask >> i;
      if ((drives & mask) == 0)
        return 'z' - i;
    }

  return 0;
}

static gchar
get_spice_folder_letter(void)
{
  const guint32 max_mask = 1 << 25;
  gchar local_name[3] = "z:";
  gchar *spice_folder_name;
  gchar spice_folder_letter = 0;
  guint32 drives;
  guint32 i;

  drives = GetLogicalDrives ();
  spice_folder_name = g_strdup_printf("\\\\localhost@%d\\DavWWWRoot", port);

  for (i = 0; i < 26; i++)
    {
      gchar remote_name[MAX_SHARED_FOLDER_NAME_SIZE];
      guint32 size = sizeof(remote_name);
      guint32 mask = max_mask >> i;
      if (drives & mask)
        {
          local_name[0] = 'z' - i;
          if ((WNetGetConnection (local_name, remote_name, (LPDWORD)&size) == NO_ERROR) &&
              (g_strcmp0 (remote_name, spice_folder_name) == 0))
            {
              spice_folder_letter = local_name[0];
              g_debug ("Found Spice Shared Folder at %c drive", spice_folder_letter);
              break;
            }
        }
    }

  g_free(spice_folder_name);

  return spice_folder_letter;
}

/* User is required to call netresource_free, when no longer needed. */
static void
netresource_init(NETRESOURCE *net_resource, const gchar drive_letter)
{
  net_resource->dwType = RESOURCETYPE_DISK;
  net_resource->lpLocalName = g_strdup_printf("%c:", drive_letter);
  net_resource->lpRemoteName = g_strdup_printf("http://localhost:%d/", port);
  net_resource->lpProvider = NULL;
}

static void
netresource_free(NETRESOURCE *net_resource)
{
  g_free(net_resource->lpLocalName);
  g_free(net_resource->lpRemoteName);
}

static guint32
map_drive(const gchar drive_letter)
{
  NETRESOURCE net_resource;
  guint32 errn;

  netresource_init(&net_resource, drive_letter);
  errn = WNetAddConnection2 (&net_resource, NULL, NULL, CONNECT_TEMPORARY);
  netresource_free(&net_resource);

  if (errn == NO_ERROR)
    {
      g_debug ("Shared folder mapped to %c succesfully", drive_letter);
    }
  else if (errn == ERROR_ALREADY_ASSIGNED)
    {
      g_debug ("Drive letter %c is already assigned", drive_letter);
    }
  else
    {
      g_warning ("map_drive error %d", errn);
    }

  return errn;
}

static void
unmap_drive(ServiceData *service_data)
{
  gchar local_name[MAX_DRIVE_LETTER_SIZE];
  guint32 errn;

  g_mutex_lock(&service_data->mutex);
  g_snprintf(local_name, MAX_DRIVE_LETTER_SIZE, "%c:", service_data->drive_letter);
  errn = WNetCancelConnection2(local_name, CONNECT_UPDATE_PROFILE, TRUE);

  if (errn == NO_ERROR)
    {
      g_debug ("Shared folder unmapped succesfully");
    }
  else if (errn == ERROR_NOT_CONNECTED)
    {
      g_debug ("Drive %c is not connected", service_data->drive_letter);
    }
  else
    {
      g_warning ("map_drive error %d", errn);
    }

  g_mutex_unlock(&service_data->mutex);
}

static void
map_drive_cb(GTask *task,
             gpointer source_object,
             gpointer task_data,
             GCancellable *cancellable)
{
  const guint32 delay = 500; //half a second
  MapDriveData *map_drive_data = task_data;
  gchar drive_letter;
  GPollFD cancel_pollfd;
  guint32 ret = 0;

  if (!g_cancellable_make_pollfd (map_drive_data->cancel_map, &cancel_pollfd))
    {
      g_critical ("GPollFD failed to create.");
      return;
    }

  ret = g_poll (&cancel_pollfd, 1, delay);
  g_cancellable_release_fd (map_drive_data->cancel_map);

  if (ret != 0)
    {
      return;
    }

  while (TRUE)
    {
      drive_letter = get_free_drive_letter ();
      if (drive_letter == 0)
        {
          g_warning ("all drive letters already assigned.");
          break;
        }

      ret = map_drive (drive_letter);
      if (ret == ERROR_ALREADY_ASSIGNED)
        {
          /* try again with another letter */
          continue;
        }
      if (ret != NO_ERROR)
        {
          drive_letter = 0;
        }
      break;
      //TODO: After mapping, rename network drive from \\localhost@PORT\DavWWWRoot
      //      to something like SPICE Shared Folder
    }
  g_mutex_lock(&map_drive_data->service_data->mutex);
  map_drive_data->service_data->drive_letter = drive_letter;
  g_mutex_unlock(&map_drive_data->service_data->mutex);
}

#endif

/* returns FALSE if the service should quit */
static gboolean
run_service (ServiceData *service_data)
{
  g_debug ("Run service");

  if (quit_service)
    return FALSE;

#ifdef G_OS_WIN32
  MapDriveData map_drive_data;
  map_drive_data.cancel_map = g_cancellable_new ();
  gchar drive_letter = get_spice_folder_letter ();

  g_mutex_lock(&service_data->mutex);
  service_data->drive_letter = drive_letter;
  map_drive_data.service_data = service_data;
  g_mutex_unlock(&service_data->mutex);

  if (drive_letter == 0)
    {
      GTask *map_drive_task = g_task_new (NULL, NULL, NULL, NULL);
      g_task_set_task_data (map_drive_task, &map_drive_data, NULL);
      g_task_run_in_thread (map_drive_task, map_drive_cb);
      g_object_unref (map_drive_task);
    }
#endif

  g_socket_service_start (socket_service);

  cancel = g_cancellable_new ();
  clients = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                   NULL, client_unref);

  loop = g_main_loop_new (NULL, TRUE);
#ifdef G_OS_UNIX
#ifdef __APPLE__
  open_mux_path ("/dev/tty.org.spice-space.webdav.0");
#else
  open_mux_path ("/dev/virtio-ports/org.spice-space.webdav.0");
#endif
#else
  open_mux_path ("\\\\.\\Global\\org.spice-space.webdav.0");
#endif

  /* listen on port for incoming clients, multiplex there input into
     virtio path, demultiplex input from there to the respective
     clients */

#ifdef WITH_AVAHI
  GError *error = NULL;
  if (!avahi_client_start ("Spice client folder", port, TRUE, &error))
    {
      g_printerr ("%s\n", error->message);
      exit (1);
    }
#endif

  start_mux_read (mux_istream);
  g_main_loop_run (loop);
  g_clear_pointer (&loop, g_main_loop_unref);

#ifdef G_OS_WIN32
  g_cancellable_cancel (map_drive_data.cancel_map);
  g_object_unref (map_drive_data.cancel_map);
#endif

  g_cancellable_cancel (cancel);

  g_clear_object (&mux_istream);
  g_clear_object (&mux_ostream);

  g_clear_object (&mux_queue);
  g_hash_table_unref (clients);

#ifdef WITH_AVAHI
  avahi_client_stop ();
#endif
  g_socket_service_stop (socket_service);

  g_clear_object (&cancel);

#ifdef G_OS_WIN32
  CloseHandle (port_handle);
#else
  close (port_fd);
#endif
  return !quit_service;
}

#ifdef G_OS_WIN32
static SERVICE_STATUS service_status;
static SERVICE_STATUS_HANDLE service_status_handle;

DWORD WINAPI
service_ctrl_handler(DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx);
VOID WINAPI
service_main(DWORD argc, TCHAR *argv[]);

DWORD WINAPI
service_ctrl_handler (DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx)
{
  DWORD ret = NO_ERROR;

  ServiceData *service_data = ctx;

  switch (ctrl)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        if (service_data->drive_letter != 0)
          {
            unmap_drive (service_data);
            g_mutex_clear(&service_data->mutex);
          }
        quit (SIGTERM);
        service_status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus (service_status_handle, &service_status);
        break;

    default:
        ret = ERROR_CALL_NOT_IMPLEMENTED;
    }

  return ret;
}

VOID WINAPI
service_main (DWORD argc, TCHAR *argv[])
{
  ServiceData service_data;

  service_data.drive_letter = 0;
  g_mutex_init(&service_data.mutex);

  service_status_handle =
    RegisterServiceCtrlHandlerEx (SERVICE_NAME, service_ctrl_handler, &service_data);

  g_return_if_fail (service_status_handle != 0);

  service_status.dwServiceType = SERVICE_WIN32;
  service_status.dwCurrentState = SERVICE_RUNNING;
  service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  service_status.dwWin32ExitCode = NO_ERROR;
  service_status.dwServiceSpecificExitCode = NO_ERROR;
  service_status.dwCheckPoint = 0;
  service_status.dwWaitHint = 0;
  SetServiceStatus (service_status_handle, &service_status);

  while (run_service(&service_data)) {
    g_usleep(G_USEC_PER_SEC);
  }

  service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus (service_status_handle, &service_status);
}
#endif

static GOptionEntry entries[] = {
  { "port", 'p', 0,
    G_OPTION_ARG_INT, &port,
    "Port to listen on", NULL },
#ifdef G_OS_WIN32
  { "no-service", 0, 0,
    G_OPTION_ARG_NONE, &no_service,
    "Don't start as a service", NULL },
#endif
  { NULL }
};

int
main (int argc, char *argv[])
{
  ServiceData service_data;
  GOptionContext *opts;
  GError *error = NULL;

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
  g_option_context_free (opts);

#ifdef G_OS_UNIX
  g_unix_signal_add (SIGINT, signal_handler, NULL);
#else
  signal (SIGINT, quit);
#endif

  /* run socket service once at beginning, there seems to be a bug on
     windows, and it can't accept new connections if cleanup and
     restart a new service */
  socket_service = g_socket_service_new ();
  GInetAddress *iaddr = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  GSocketAddress *saddr = g_inet_socket_address_new (iaddr, port);
  g_object_unref (iaddr);

  g_socket_listener_add_address (G_SOCKET_LISTENER (socket_service),
                                 saddr,
                                 G_SOCKET_TYPE_STREAM,
                                 G_SOCKET_PROTOCOL_TCP,
                                 NULL,
                                 NULL,
                                 &error);
  g_object_unref (saddr);
  if (error)
    {
      g_printerr ("%s\n", error->message);
      exit (1);
    }

  g_signal_connect (socket_service,
                    "incoming", G_CALLBACK (incoming_callback),
                    NULL);

#ifdef G_OS_WIN32
  service_data.drive_letter = 0;
  g_mutex_init(&service_data.mutex);

  SERVICE_TABLE_ENTRY service_table[] =
    {
      { SERVICE_NAME, service_main }, { NULL, NULL }
    };
  if (!no_service && !getenv("DEBUG"))
    {
      if (!StartServiceCtrlDispatcher (service_table))
        {
          g_error ("%s", g_win32_error_message (GetLastError ()));
          exit (1);
        }
    } else
#endif
  while (run_service(&service_data)) {
    g_usleep (G_USEC_PER_SEC);
  }

#ifdef G_OS_WIN32
  if (service_data.drive_letter != 0)
    {
      unmap_drive (&service_data);
      g_mutex_clear(&service_data.mutex);
    }
#endif

  g_clear_object (&socket_service);

  return 0;
}
