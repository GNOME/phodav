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
  g_output_stream_write_all (q->output, e->buf, e->size, NULL, NULL, &error);
  if (e->cb)
    e->cb (q, e->user_data, error);

  if (error)
      goto end;

  q->flushing = TRUE;
  g_output_stream_flush_async (q->output, G_PRIORITY_DEFAULT, NULL, output_queue_flush_cb, e);

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
static void start_mux_read (GInputStream *istream);

static void
quit (int sig)
{
  g_debug ("quit %d", sig);

  if (sig == SIGINT || sig == SIGTERM)
      quit_service = TRUE;

  g_main_loop_quit (loop);
}

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
  gssize size;
} ReadData;

static void
read_thread (GSimpleAsyncResult *simple,
             GObject            *object,
             GCancellable       *cancellable)
{
  GError *error = NULL;
  GInputStream *stream = G_INPUT_STREAM (object);
  ReadData *data;
  gsize bread;

  data = g_simple_async_result_get_op_res_gpointer (simple);

  g_debug ("my read %" G_GSIZE_FORMAT, data->count);
  g_input_stream_read_all (stream,
                           data->buffer, data->count, &bread,
                           cancellable, &error);
  g_debug ("my read result %" G_GSIZE_FORMAT, bread);
  if (bread != data->count)
    data->size = -1;
  else
    data->size = bread;

  if (error)
    {
      g_debug ("error: %s", error->message);
      g_simple_async_result_set_from_error (simple, error);
    }
}

static void
my_input_stream_read_async (GInputStream       *stream,
                            void               *buffer,
                            gsize               count,
                            int                 io_priority,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
  GSimpleAsyncResult *simple;
  ReadData *data = g_new (ReadData, 1);

  data->buffer = buffer;
  data->count = count;

  simple = g_simple_async_result_new (G_OBJECT (stream),
                                      callback, user_data,
                                      my_input_stream_read_async);

  g_simple_async_result_set_op_res_gpointer (simple, data, g_free);
  g_simple_async_result_run_in_thread (simple, read_thread, io_priority, cancellable);
  g_object_unref (simple);
}

static gssize
my_input_stream_read_finish (GInputStream *stream,
                             GAsyncResult *result,
                             GError      **error)
{
  GSimpleAsyncResult *simple;
  ReadData *data;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (stream),
                                                        my_input_stream_read_async),
                        -1);

  simple = G_SIMPLE_ASYNC_RESULT (result);

  if (g_simple_async_result_propagate_error (simple, error))
    return -1;

  data = g_simple_async_result_get_op_res_gpointer (simple);

  return data->size;
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

  size = my_input_stream_read_finish (G_INPUT_STREAM (source_object), res, &error);
  g_return_if_fail (size == demux.size);
  if (error)
    {
      g_warning ("error: %s", error->message);
      g_clear_error (&error);
      quit (-1);
      return;
    }

  g_debug ("looking up client %" G_GINT64_FORMAT, demux.client);
  Client *c = g_hash_table_lookup (clients, &demux.client);
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

  size = my_input_stream_read_finish (G_INPUT_STREAM (source_object), res, &error);
  if (error || size != sizeof (guint16))
    goto end;

  my_input_stream_read_async (istream,
                              &demux.buf, demux.size, G_PRIORITY_DEFAULT,
                              NULL, mux_data_read_cb, NULL);
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

  size = my_input_stream_read_finish (G_INPUT_STREAM (source_object), res, &error);
  g_debug ("read %" G_GSSIZE_FORMAT, size);
  if (error || size != sizeof (gint64))
    goto end;
  my_input_stream_read_async (istream,
                              &demux.size, sizeof (guint16), G_PRIORITY_DEFAULT,
                              NULL, mux_size_read_cb, NULL);
  return;

end:
  if (error)
    {
      g_warning ("error: %s", error->message);
      g_clear_error (&error);
    }

  quit (-3);
}

static void
start_mux_read (GInputStream *istream)
{
  my_input_stream_read_async (istream,
                              &demux.client, sizeof (gint64), G_PRIORITY_DEFAULT,
                              NULL, mux_client_read_cb, NULL);
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
  g_debug ("end read %" G_GSIZE_FORMAT, size);
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

  g_debug ("start read");
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
      if (mdns_group)
        {
          ga_entry_group_reset (mdns_group, NULL);
          mdns_service = 0;
        }
      break;

    default:
      // Do nothing
      break;
    }
}
#endif

#ifndef G_SOURCE_REMOVE
#define G_SOURCE_REMOVE FALSE
#endif
#ifndef G_SOURCE_CONTINUE
#define G_SOURCE_CONTINUE TRUE
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

#ifdef G_OS_UNIX
  gint fd = g_open (path, O_RDWR);
  if (fd == -1)
      exit (1);

  wait_for_virtio_host (fd);

  mux_ostream = g_unix_output_stream_new (fd, TRUE);
  mux_istream = g_unix_input_stream_new (fd, FALSE);
#else
  HANDLE h = CreateFile (path,
                         GENERIC_WRITE | GENERIC_READ,
                         0,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL);
  g_assert (h != INVALID_HANDLE_VALUE);

  mux_ostream = G_OUTPUT_STREAM (g_win32_output_stream_new (h, TRUE));
  mux_istream = G_INPUT_STREAM (g_win32_input_stream_new (h, TRUE));
#endif

  mux_queue = output_queue_new (G_OUTPUT_STREAM (mux_ostream));

  start_mux_read (mux_istream);
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

  switch (ctrl)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
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
  service_status_handle =
    RegisterServiceCtrlHandlerEx ("spice-webdavd", service_ctrl_handler, NULL);

  g_return_if_fail (service_status_handle != 0);

  service_status.dwServiceType = SERVICE_WIN32;
  service_status.dwCurrentState = SERVICE_RUNNING;
  service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  service_status.dwWin32ExitCode = NO_ERROR;
  service_status.dwServiceSpecificExitCode = NO_ERROR;
  service_status.dwCheckPoint = 0;
  service_status.dwWaitHint = 0;
  SetServiceStatus (service_status_handle, &service_status);

  g_main_loop_run (loop);

  service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus (service_status_handle, &service_status);
}
#endif

static GOptionEntry entries[] = {
  { "port", 'p', 0,
    G_OPTION_ARG_INT, &port,
    "Port to listen on", NULL },
  { NULL }
};

int
main (int argc, char *argv[])
{
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

  signal (SIGINT, quit);


  GSocketService *service = g_socket_service_new ();
  GInetAddress *iaddr = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);
  GSocketAddress *saddr = g_inet_socket_address_new (iaddr, port);
  g_object_unref (iaddr);

  g_socket_listener_add_address (G_SOCKET_LISTENER (service), saddr,
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

  g_signal_connect (service,
                    "incoming", G_CALLBACK (incoming_callback),
                    NULL);

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

  g_socket_service_start (service);

#ifdef WITH_AVAHI
  mdns_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);
  g_signal_connect (mdns_client, "state-changed", G_CALLBACK (mdns_state_changed), NULL);
  if (!ga_client_start (mdns_client, &error))
    {
      g_printerr ("%s\n", error->message);
      exit (1);
    }
#endif

#ifdef G_OS_WIN32
  SERVICE_TABLE_ENTRY service_table[] =
    {
      { (char *)"spice-webdavd", service_main }, { NULL, NULL }
    };
  if (!StartServiceCtrlDispatcher (service_table))
    {
      g_error ("%s", g_win32_error_message(GetLastError()));
      exit (1);
    }
#else
  g_main_loop_run (loop);
#endif

  g_main_loop_unref (loop);

  output_queue_unref (mux_queue);
  g_hash_table_unref (clients);

  return 0;
}
