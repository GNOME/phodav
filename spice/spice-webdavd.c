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
#endif

#ifdef G_OS_WIN32
#include <gio/gwin32inputstream.h>
#include <gio/gwin32outputstream.h>
#include <windows.h>
#endif

#ifdef WITH_AVAHI
#include <avahi-gobject/ga-client.h>
#include <avahi-gobject/ga-entry-group.h>
#endif

typedef struct _OutputQueue
{
  guint          refs;
  GOutputStream *output;
  gboolean       flushing;
  guint          idle_id;
  GQueue        *queue;
} OutputQueue;

typedef void (*PushedCb) (OutputQueue *q, gpointer user_data, GError *error);

typedef struct _OutputQueueElem
{
  OutputQueue  *queue;
  const guint8 *buf;
  gsize         size;
  PushedCb      cb;
  gpointer      user_data;
} OutputQueueElem;

typedef struct _ServiceData
{
#ifdef G_OS_WIN32
  gchar drive_letter;
  GMutex mutex;
#endif
} ServiceData;

static GCancellable *cancel;

static OutputQueue*
output_queue_new (GOutputStream *output)
{
  OutputQueue *queue = g_new0 (OutputQueue, 1);

  queue->output = g_object_ref (output);
  queue->queue = g_queue_new ();
  queue->refs = 1;

  return queue;
}

static
void
output_queue_free (OutputQueue *queue)
{
  g_warn_if_fail (g_queue_get_length (queue->queue) == 0);
  g_warn_if_fail (!queue->flushing);
  g_warn_if_fail (!queue->idle_id);

  g_queue_free_full (queue->queue, g_free);
  g_clear_object (&queue->output);
  g_free (queue);
}

static OutputQueue*
output_queue_ref (OutputQueue *q)
{
  q->refs++;
  return q;
}

static void
output_queue_unref (OutputQueue *q)
{
  g_return_if_fail (q != NULL);

  q->refs--;
  if (q->refs == 0)
    output_queue_free (q);
}

static gboolean output_queue_idle (gpointer user_data);

static void
output_queue_flush_cb (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  GError *error = NULL;
  OutputQueueElem *e = user_data;
  OutputQueue *q = e->queue;

  g_debug ("flushed");
  q->flushing = FALSE;
  g_output_stream_flush_finish (G_OUTPUT_STREAM (source_object),
                                res, &error);
  if (error)
    g_warning ("error: %s", error->message);

  g_clear_error (&error);

  if (!q->idle_id)
    q->idle_id = g_idle_add (output_queue_idle, output_queue_ref (q));

  g_free (e);
  output_queue_unref (q);
}

static gboolean
output_queue_idle (gpointer user_data)
{
  OutputQueue *q = user_data;
  OutputQueueElem *e = NULL;
  GError *error = NULL;

  if (q->flushing)
    {
      g_debug ("already flushing");
      goto end;
    }

  e = g_queue_pop_head (q->queue);
  if (!e)
    {
      g_debug ("No more data to flush");
      goto end;
    }

  g_debug ("flushing %" G_GSIZE_FORMAT, e->size);
  g_output_stream_write_all (q->output, e->buf, e->size, NULL, cancel, &error);
  if (e->cb)
    e->cb (q, e->user_data, error);

  if (error)
      goto end;

  q->flushing = TRUE;
  g_output_stream_flush_async (q->output, G_PRIORITY_DEFAULT, cancel, output_queue_flush_cb, e);

  q->idle_id = 0;
  return FALSE;

end:
  g_clear_error (&error);
  q->idle_id = 0;
  g_free (e);
  output_queue_unref (q);

  return FALSE;
}

static void
output_queue_push (OutputQueue *q, const guint8 *buf, gsize size,
                   PushedCb pushed_cb, gpointer user_data)
{
  OutputQueueElem *e;

  g_return_if_fail (q != NULL);

  e = g_new (OutputQueueElem, 1);
  e->buf = buf;
  e->size = size;
  e->cb = pushed_cb;
  e->user_data = user_data;
  e->queue = q;
  g_queue_push_tail (q->queue, e);

  if (!q->idle_id && !q->flushing)
    q->idle_id = g_idle_add (output_queue_idle, output_queue_ref (q));
}


static struct _DemuxData
{
  gint64  client;
  guint16 size;
  gchar   buf[G_MAXUINT16];
} demux;

typedef struct _Client
{
  gint64             id;
  guint8             buf[G_MAXUINT16];
  guint16            size;
  GSocketConnection *client_connection;
  OutputQueue       *queue;
} Client;

static gboolean quit_service;
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
#ifdef WITH_AVAHI
static void mdns_unregister_service (void);
#endif

static void
quit (int sig)
{
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
  GIOStream *iostream = G_IO_STREAM (client_connection);
  GOutputStream *ostream = g_io_stream_get_output_stream (iostream);
  GOutputStream *bostream;
  Client *client;

  bostream = g_buffered_output_stream_new (ostream);
  g_buffered_output_stream_set_auto_grow (G_BUFFERED_OUTPUT_STREAM (bostream), TRUE);

  client = g_new0 (Client, 1);
  client->client_connection = g_object_ref (client_connection);
  // TODO: check if usage of this idiom is portable, or if we need to check collisions
  client->id = GPOINTER_TO_INT (client_connection);
  client->queue = output_queue_new (bostream);
  g_object_unref (bostream);

  g_hash_table_insert (clients, &client->id, client);
  g_warn_if_fail (g_hash_table_lookup (clients, &client->id));

  return client;
}

static void
client_free (Client *c)
{
  g_debug ("Free client %p", c);

  g_io_stream_close (G_IO_STREAM (c->client_connection), NULL, NULL);
  g_object_unref (c->client_connection);
  output_queue_unref (c->queue);
  g_free (c);
}

static void
remove_client (Client *client)
{
  g_debug ("remove client %p", client);

  g_hash_table_remove (clients, &client->id);
}

typedef struct ReadData
{
  void  *buffer;
  gsize  count;
} ReadData;

static void
read_thread (GTask *task,
             gpointer source_object,
             gpointer task_data,
             GCancellable *cancellable)
{
  GError *error = NULL;
  GInputStream *stream = G_INPUT_STREAM (source_object);
  ReadData *data;
  gsize bread;

  data = g_task_get_task_data (task);

  g_debug ("thread read %" G_GSIZE_FORMAT, data->count);
  g_input_stream_read_all (stream,
                           data->buffer, data->count, &bread,
                           cancellable, &error);
  g_debug ("thread read result %" G_GSIZE_FORMAT, bread);

  if (error)
    {
      g_debug ("error: %s", error->message);
      g_task_return_error (task, error);
    }
  if (bread != data->count)
    {
      g_task_return_int (task, -1);
    }
  else
    {
      g_task_return_int (task, bread);
    }
}

static void
input_stream_read_thread_async (GInputStream       *stream,
                                void               *buffer,
                                gsize               count,
                                int                 io_priority,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
  GTask *task;
  ReadData *data = g_new (ReadData, 1);

  data->buffer = buffer;
  data->count = count;

  task = g_task_new (stream, cancellable, callback, user_data);
  g_task_set_task_data (task, data, g_free);
  g_task_run_in_thread (task, read_thread);

  g_object_unref (task);
}

static gssize
input_stream_read_thread_finish (GInputStream *stream,
                                 GAsyncResult *result,
                                 GError      **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), -1);

  return g_task_propagate_int (G_TASK (result), error);
}

static void
handle_push_error (OutputQueue *q, gpointer user_data, GError *error)
{
  Client *client = user_data;

  if (!error)
    return;

  g_warning ("push error: %s", error->message);
  remove_client (client);
  return;
}

static void
mux_pushed_client_cb (OutputQueue *q, gpointer user_data, GError *error)
{
  if (error) {
    handle_push_error (q, user_data, error);
    return;
  }

  start_mux_read (mux_istream);
}

static void
mux_data_read_cb (GObject      *source_object,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  GError *error = NULL;
  gssize size;

  size = input_stream_read_thread_finish (G_INPUT_STREAM (source_object), res, &error);
  g_return_if_fail (size == demux.size);
  if (error)
    {
      g_warning ("error: %s", error->message);
      g_clear_error (&error);
      quit (-1);
      return;
    }

  Client *c = g_hash_table_lookup (clients, &demux.client);
  g_debug ("looked up client: %p", c);
  g_warn_if_fail(c != NULL);

  if (c)
    output_queue_push (c->queue, (guint8 *) demux.buf, demux.size,
                       mux_pushed_client_cb, c);
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
  gssize size;

  size = input_stream_read_thread_finish (G_INPUT_STREAM (source_object), res, &error);
  if (error || size != sizeof (guint16))
    goto end;

  input_stream_read_thread_async (istream,
                                  &demux.buf, demux.size, G_PRIORITY_DEFAULT,
                                  cancel, mux_data_read_cb, NULL);
  return;

end:
  if (error)
    {
      g_warning ("error: %s", error->message);
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
  gssize size;

  size = input_stream_read_thread_finish (G_INPUT_STREAM (source_object), res, &error);
  g_debug ("read %" G_GSSIZE_FORMAT, size);
  if (error || size != sizeof (gint64))
    goto end;
  input_stream_read_thread_async (istream,
                                  &demux.size, sizeof (guint16), G_PRIORITY_DEFAULT,
                                  cancel, mux_size_read_cb, NULL);
  return;

end:
  if (error)
    {
      g_warning ("error: %s", error->message);
      g_clear_error (&error);
    }

#ifdef WITH_AVAHI
  mdns_unregister_service ();
#endif
  quit (-3);
}

static void
start_mux_read (GInputStream *istream)
{
  input_stream_read_thread_async (istream,
                                  &demux.client, sizeof (gint64), G_PRIORITY_DEFAULT,
                                  cancel, mux_client_read_cb, NULL);
}

static void client_start_read (Client *client);

static void
mux_pushed_cb (OutputQueue *q, gpointer user_data, GError *error)
{
  Client *client = user_data;

  if (error) {
      handle_push_error (q, client, error);
      return;
  }

  if (client->size == 0)
    {
      remove_client (client);
      return;
    }

  client_start_read (client);
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
  g_debug ("end read %" G_GSSIZE_FORMAT, size);
  if (error)
    {
      g_warning ("error: %s", error->message);
      g_clear_error (&error);
      remove_client (client);
      return;
    }

  g_return_if_fail (size <= G_MAXUINT16);
  g_return_if_fail (size >= 0);
  client->size = size;

  output_queue_push (mux_queue, (guint8 *) &client->id, sizeof (gint64), handle_push_error, client);
  output_queue_push (mux_queue, (guint8 *) &client->size, sizeof (guint16), handle_push_error, client);
  output_queue_push (mux_queue, (guint8 *) client->buf, size, mux_pushed_cb, client);

  return;
}

static void
client_start_read (Client *client)
{
  GIOStream *iostream = G_IO_STREAM (client->client_connection);
  GInputStream *istream = g_io_stream_get_input_stream (iostream);

  g_debug ("start read client %p", client);
  g_input_stream_read_async (istream,
                             client->buf, G_MAXUINT16, G_PRIORITY_DEFAULT,
                             NULL, client_read_cb, client);
}

static gboolean
incoming_callback (GSocketService    *service,
                   GSocketConnection *client_connection,
                   GObject           *source_object,
                   gpointer           user_data)
{
  Client *client;

  g_debug ("new client!");
  client = add_client (client_connection);
  client_start_read (client);

  return FALSE;
}

static int port;

#ifdef G_OS_WIN32
static gboolean no_service;
#endif

#ifdef WITH_AVAHI
static GaClient *mdns_client;
static GaEntryGroup *mdns_group;
static GaEntryGroupService *mdns_service;

static void
mdns_register_service (void)
{
  GError *error = NULL;
  gchar *name = NULL;

  if (!mdns_group)
    {
      mdns_group = ga_entry_group_new ();

      if (!ga_entry_group_attach (mdns_group, mdns_client, &error))
        {
          g_warning ("Could not attach MDNS group to client: %s", error->message);
          g_clear_error (&error);
          goto end;
        }
    }

  name = g_strdup_printf ("Spice client folder");
  mdns_service = ga_entry_group_add_service (mdns_group,
                                             name, "_webdav._tcp",
                                             port, &error,
                                             NULL);
  if (!mdns_service)
    {
      g_warning ("Could not create service: %s", error->message);
      g_clear_error (&error);
      goto end;

    }

  ga_entry_group_service_freeze (mdns_service);
  if (!ga_entry_group_service_set (mdns_service, "u", "", &error) ||
      !ga_entry_group_service_set (mdns_service, "p", "", &error) ||
      !ga_entry_group_service_set (mdns_service, "path", "/", &error) ||
      !ga_entry_group_service_thaw (mdns_service, &error))
    {
      g_warning ("Could not update TXT: %s", error->message);
      g_clear_error (&error);
    }

  if (!ga_entry_group_commit (mdns_group, &error))
    {
      g_warning ("Could not announce MDNS service: %s", error->message);
      g_clear_error (&error);
    }

end:
  g_free (name);
}

static void
mdns_unregister_service (void)
{
  GError *error = NULL;

  if (mdns_group)
    {
      if (!ga_entry_group_reset (mdns_group, &error))
        {
          g_warning ("Could not disconnect MDNS service: %s", error->message);
          g_clear_error (&error);
        }

      mdns_service = 0;
      g_debug ("MDNS client disconected");
    }
}

static void
mdns_state_changed (GaClient *client, GaClientState state, gpointer user_data)
{
  switch (state)
    {
    case GA_CLIENT_STATE_FAILURE:
      g_warning ("MDNS client state failure");
      break;

    case GA_CLIENT_STATE_S_RUNNING:
      g_debug ("MDNS client found server running");
      mdns_register_service ();
      break;

    case GA_CLIENT_STATE_S_COLLISION:
    case GA_CLIENT_STATE_S_REGISTERING:
      g_message ("MDNS collision");
      mdns_unregister_service ();
      break;

    default:
      // Do nothing
      break;
    }
}
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
  if (port_fd == -1)
      exit (1);

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

  mux_queue = output_queue_new (G_OUTPUT_STREAM (mux_ostream));
}

#ifdef G_OS_WIN32
#define MAX_SHARED_FOLDER_NAME_SIZE 64
#define MAX_DRIVE_LETTER_SIZE 3
typedef enum _MapDriveEnum
{
  MAP_DRIVE_OK,
  MAP_DRIVE_TRY_AGAIN,
  MAP_DRIVE_ERROR
} MapDriveEnum;

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

static MapDriveEnum
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
      return MAP_DRIVE_OK;
    }
  else if (errn == ERROR_ALREADY_ASSIGNED)
    {
      g_debug ("Drive letter %c is already assigned", drive_letter);
      return MAP_DRIVE_TRY_AGAIN;
    }

  g_warning ("map_drive error %d", errn);
  return MAP_DRIVE_ERROR;
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
  return;
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

      if (map_drive (drive_letter) != MAP_DRIVE_TRY_AGAIN)
        {
          break;
        }
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
                                   NULL, (GDestroyNotify) client_free);

  loop = g_main_loop_new (NULL, TRUE);
#ifdef G_OS_UNIX
  open_mux_path ("/dev/virtio-ports/org.spice-space.webdav.0");
#else
  open_mux_path ("\\\\.\\Global\\org.spice-space.webdav.0");
#endif

  /* listen on port for incoming clients, multiplex there input into
     virtio path, demultiplex input from there to the respective
     clients */

#ifdef WITH_AVAHI
  GError *error = NULL;

  mdns_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);
  g_signal_connect (mdns_client, "state-changed", G_CALLBACK (mdns_state_changed), NULL);
  if (!ga_client_start (mdns_client, &error))
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

  output_queue_unref (mux_queue);
  g_hash_table_unref (clients);

  g_socket_service_stop (socket_service);

  mux_queue = NULL;
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
    RegisterServiceCtrlHandlerEx ("spice-webdavd", service_ctrl_handler, &service_data);

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
      { (char *)"spice-webdavd", service_main }, { NULL, NULL }
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
