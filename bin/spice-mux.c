/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/* The multiplexing of the client connections over the spice WebDAV
 * channel, normally the virtio port of the guest.
 *
 * The daemon listens for the connections of the WebDAV clients and
 * multiplexes them over the single channel, with the frames:
 *
 *   [gint64 connection id][guint16 size][data]
 *
 * The connection id is chosen by this end when the client connects,
 * with a monotonically increasing counter: the other end learns it
 * from the first frame forwarded for the connection, and echoes it
 * back. A frame with a size of 0 tells the other end that the
 * connection is gone.
 *
 * The frames read from the channel are written to their client, one
 * at a time: the next frame is only read once the previous one was
 * written, so that a slow client does not pile up frames in memory.
 * The data read from the clients is pushed to the channel through an
 * output queue, in order.
 *
 * The channel is closed when it breaks (a read error or a short read,
 * the guest went away...) or when spice_mux_stop() is called: the
 * clients are dropped, as they cannot be served without a channel,
 * and the "closed" signal is emitted. A new SpiceMux reopens the
 * channel: the clients have to reconnect. */

#include "config.h"

#include "spice-mux.h"

#ifdef G_OS_UNIX
#include <errno.h>
#include <fcntl.h>

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>
#endif

#ifdef G_OS_WIN32
#include <gio/gwin32inputstream.h>
#include <gio/gwin32outputstream.h>
#include <windows.h>
#endif

#include "output-queue.h"

/* a connected client, and the frame pushed to the channel on its
 * behalf */
typedef struct _Client
{
  guint ref_count;

  /* the frame pushed for the client: the id and the size are the
   * channel header, followed by the data read from the connection.
   * The buffer is borrowed by the output queue while the frame is
   * being written, the operation below keeps the client alive for
   * it */
  struct
  {
    gint64 id;
    guint16 size;
    guint8 buf[G_MAXUINT16];
  } frame;

  GSocketConnection *connection;
} Client;

/* an operation in flight on a client: it keeps the mux and the client
 * alive until it completes */
typedef struct _MuxOp
{
  SpiceMux *mux;
  Client *client;
} MuxOp;

struct _SpiceMux
{
  GObject parent_instance;

  GInputStream *istream;
  GOutputStream *ostream;
  GCancellable *cancellable;
  OutputQueue *queue;

  /* the connected clients, keyed on their frame id; the table owns
   * them */
  GHashTable *clients;
  gint64 next_client_id;

  /* the header and the data of the frame being read from the channel;
   * the steps of the read chain below do not overlap. The buffer is
   * heap-allocated: it does not fit in the instance, which GObject
   * limits to 64 KiB */
  gint64 read_client;
  guint16 read_size;
  guint8 *read_buf;

  /* whether the channel is closed: no frame is read or pushed anymore,
   * and the "closed" signal has been emitted */
  gboolean closed;
};

enum
{
  CLOSED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_FINAL_TYPE (SpiceMux, spice_mux, G_TYPE_OBJECT)

static void mux_client_read_cb (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void mux_size_read_cb (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void mux_data_read_cb (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void mux_pushed_client_cb (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void client_start_read (SpiceMux *mux, Client *client);

static Client *
client_ref (Client *client)
{
  client->ref_count++;

  return client;
}

static void
client_unref (gpointer client_ptr)
{
  Client *client = client_ptr;

  if (--client->ref_count > 0)
    return;

  g_debug ("Free client %" G_GINT64_FORMAT, client->frame.id);

  g_io_stream_close (G_IO_STREAM (client->connection), NULL, NULL);
  g_object_unref (client->connection);
  g_free (client);
}

static void
remove_client (SpiceMux *mux, Client *client)
{
  g_debug ("remove client %" G_GINT64_FORMAT, client->frame.id);

  g_hash_table_remove (mux->clients, &client->frame.id);
}

static MuxOp *
mux_op_new (SpiceMux *mux, Client *client)
{
  MuxOp *op = g_new (MuxOp, 1);

  op->mux = g_object_ref (mux);
  op->client = client_ref (client);

  return op;
}

static void
mux_op_free (MuxOp *op)
{
  g_object_unref (op->mux);
  client_unref (op->client);
  g_free (op);
}

/* the channel is closed: stop the multiplexing and notify the owner.
 * The clients are dropped, as they cannot be served without a channel
 * to multiplex them over; the operations still in flight are
 * cancelled, they complete with an error and release the clients. */
static void
mux_close (SpiceMux *mux)
{
  if (mux->closed)
    return;

  mux->closed = TRUE;
  g_debug ("mux channel closed");

  g_cancellable_cancel (mux->cancellable);
  g_hash_table_remove_all (mux->clients);

  /* the handlers may drop the last reference on the mux: keep it
   * alive for the emission */
  g_object_ref (mux);
  g_signal_emit (mux, signals[CLOSED], 0);
  g_object_unref (mux);
}

/* the channel broke while reading a frame: close it */
static void
mux_read_error (SpiceMux *mux, const GError *error, const gchar *what)
{
  if (error)
    {
      /* the operations cancelled by mux_close() or the shutdown
       * complete with an error too, they are not channel failures */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("error %s: %s", what, error->message);
    }
  else
    g_debug ("short read %s", what);

  mux_close (mux);
}

/* read the client id of the next frame; the read holds its own
 * reference on the mux until its callback */
static void
mux_read_header (SpiceMux *mux)
{
  g_debug ("waiting for the next frame header");
  g_input_stream_read_all_async (mux->istream,
                                 &mux->read_client, sizeof (mux->read_client),
                                 G_PRIORITY_DEFAULT, mux->cancellable,
                                 mux_client_read_cb, g_object_ref (mux));
}

static void
mux_client_read_cb (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
  g_autoptr(SpiceMux) mux = SPICE_MUX (user_data);
  g_autoptr(GError) error = NULL;
  gsize size;

  g_input_stream_read_all_finish (G_INPUT_STREAM (source_object), res, &size, &error);

  if (error || size != sizeof (mux->read_client))
    {
      mux_read_error (mux, error, "reading the client id");
      return;
    }

  g_debug ("frame header from client %" G_GINT64_FORMAT, mux->read_client);
  g_input_stream_read_all_async (mux->istream,
                                 &mux->read_size, sizeof (mux->read_size),
                                 G_PRIORITY_DEFAULT, mux->cancellable,
                                 mux_size_read_cb, g_object_ref (mux));
}

static void
mux_size_read_cb (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
  g_autoptr(SpiceMux) mux = SPICE_MUX (user_data);
  g_autoptr(GError) error = NULL;
  gsize size;

  g_input_stream_read_all_finish (G_INPUT_STREAM (source_object), res, &size, &error);

  if (error || size != sizeof (mux->read_size))
    {
      mux_read_error (mux, error, "reading the frame size");
      return;
    }

  g_debug ("frame of %u bytes from client %" G_GINT64_FORMAT,
           mux->read_size, mux->read_client);
  g_input_stream_read_all_async (mux->istream,
                                 mux->read_buf, mux->read_size,
                                 G_PRIORITY_DEFAULT, mux->cancellable,
                                 mux_data_read_cb, g_object_ref (mux));
}

static void
mux_data_read_cb (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
  g_autoptr(SpiceMux) mux = SPICE_MUX (user_data);
  g_autoptr(GError) error = NULL;
  gsize size;
  Client *client;

  g_input_stream_read_all_finish (G_INPUT_STREAM (source_object), res, &size, &error);

  if (error || size != mux->read_size)
    {
      mux_read_error (mux, error, "reading the frame data");
      return;
    }

  client = g_hash_table_lookup (mux->clients, &mux->read_client);
  g_debug ("read %" G_GSIZE_FORMAT " bytes from mux for client %" G_GINT64_FORMAT " (%p)",
           size, mux->read_client, client);

  if (client)
    {
      GOutputStream *out;

      if (mux->read_size == 0)
        {
          /* an empty frame from the host means that it closed the phodav
           * connection: close the client connection too, otherwise the
           * webdav client may keep reusing a connection the host does
           * not know anymore */
          g_debug ("closing client %" G_GINT64_FORMAT ", the host closed its connection",
                   client->frame.id);
          g_io_stream_close (G_IO_STREAM (client->connection), NULL, NULL);
          remove_client (mux, client);
          mux_read_header (mux);
          return;
        }

      out = g_io_stream_get_output_stream (G_IO_STREAM (client->connection));

      /* the frame is written to the client, and the next frame is only
       * read from the channel once this write is done */
      g_output_stream_write_all_async (out, mux->read_buf, mux->read_size,
                                       G_PRIORITY_DEFAULT, mux->cancellable,
                                       mux_pushed_client_cb, mux_op_new (mux, client));
    }
  else
    {
      g_debug ("frame for unknown client %" G_GINT64_FORMAT ", skipping",
               mux->read_client);
      /* an unknown client, it hung up before its frame was read: skip
       * the frame and read the next one */
      mux_read_header (mux);
    }
}

static void
mux_pushed_client_cb (GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
{
  MuxOp *op = user_data;
  g_autoptr(GError) error = NULL;

  g_output_stream_write_all_finish (G_OUTPUT_STREAM (source_object), res, NULL, &error);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("error pushing to client %" G_GINT64_FORMAT ": %s",
                   op->client->frame.id, error->message);
      remove_client (op->mux, op->client);
    }

  g_debug ("frame pushed to client %" G_GINT64_FORMAT " (%s)",
           op->client->frame.id, error ? "failed" : "ok");

  /* the frame was written to the client (or the write failed): read
   * the next one, unless the channel was closed in the meantime */
  if (!op->mux->closed)
    mux_read_header (op->mux);

  mux_op_free (op);
}

/* the frame read from the client was pushed to the channel: read its
 * next frame, or drop it when it hung up */
static void
mux_pushed_cb (OutputQueue *q,
               gpointer user_data,
               GError *error)
{
  MuxOp *op = user_data;

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("error pushing to mux from client %" G_GINT64_FORMAT ": %s",
                   op->client->frame.id, error->message);
      remove_client (op->mux, op->client);
      goto end;
    }

  if (op->mux->closed)
    {
      /* the channel is gone, the client cannot be served anymore */
      remove_client (op->mux, op->client);
      goto end;
    }

  g_debug ("frame from client %" G_GINT64_FORMAT " pushed to the mux",
           op->client->frame.id);

  if (op->client->frame.size == 0)
    {
      /* the final empty frame told the other end that the client is
       * gone, drop it */
      remove_client (op->mux, op->client);
      goto end;
    }

  client_start_read (op->mux, op->client);

end:
  mux_op_free (op);
}

static void
client_read_cb (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  MuxOp *op = user_data;
  g_autoptr(GError) error = NULL;
  gssize size;

  size = g_input_stream_read_finish (G_INPUT_STREAM (source_object), res, &error);
  g_debug ("read %" G_GSSIZE_FORMAT " bytes from client %" G_GINT64_FORMAT,
           size, op->client->frame.id);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("error for client %" G_GINT64_FORMAT ": %s",
                   op->client->frame.id, error->message);
      remove_client (op->mux, op->client);
      mux_op_free (op);
      return;
    }

  g_return_if_fail (size >= 0 && size <= G_MAXUINT16);
  op->client->frame.size = size;

  if (op->mux->closed)
    {
      /* the channel is gone, do not queue the frame of the client on
       * a dead channel */
      remove_client (op->mux, op->client);
      mux_op_free (op);
      return;
    }

  /* the frame is queued on the channel: the header followed by the
   * data read from the client, written out in order once the previous
   * frames are done */
  output_queue_push (op->mux->queue, (guint8 *) &op->client->frame,
                     sizeof (gint64) + sizeof (guint16) + size,
                     mux_pushed_cb, op);
}

static void
client_start_read (SpiceMux *mux, Client *client)
{
  GIOStream *iostream = G_IO_STREAM (client->connection);
  GInputStream *istream = g_io_stream_get_input_stream (iostream);

  g_debug ("start read client %" G_GINT64_FORMAT, client->frame.id);
  g_input_stream_read_async (istream,
                             client->frame.buf, G_MAXUINT16, G_PRIORITY_DEFAULT,
                             mux->cancellable, client_read_cb, mux_op_new (mux, client));
}

#ifdef G_OS_UNIX
/* the virtio port only carries data once the host side is connected:
 * HUP is set until then */
static void
wait_for_virtio_host (gint fd)
{
  GPollFD pfd = { .fd = fd, .events = G_IO_HUP | G_IO_IN | G_IO_OUT };

  while (1)
    {
      gint n;

      g_debug ("polling");
      n = g_poll (&pfd, 1, -1);
      if (n < 0 && errno == EINTR)
        continue;
      g_assert_cmpint (n, ==, 1);
      g_debug ("polling end");
      if (!(pfd.revents & G_IO_HUP))
        break;

      // FIXME: I don't see a reasonable way to wait for virtio
      // host-side to be connected..
      g_usleep (G_USEC_PER_SEC);
    }
}
#endif

static void
spice_mux_dispose (GObject *object)
{
  SpiceMux *mux = SPICE_MUX (object);

  /* the operations in flight hold references on the mux: they are
   * cancelled beforehand, by mux_close() from the channel breaking or
   * spice_mux_stop(), and this only runs once they have completed and
   * released them */
  g_cancellable_cancel (mux->cancellable);
  g_clear_pointer (&mux->clients, g_hash_table_unref);

  /* the output stream owns the channel and closes it: the input
   * stream only borrows it, release it first */
  g_clear_object (&mux->istream);
  g_clear_object (&mux->ostream);
  g_clear_object (&mux->queue);
  g_clear_object (&mux->cancellable);

  G_OBJECT_CLASS (spice_mux_parent_class)->dispose (object);
}

static void
spice_mux_finalize (GObject *object)
{
  SpiceMux *mux = SPICE_MUX (object);

  g_free (mux->read_buf);

  G_OBJECT_CLASS (spice_mux_parent_class)->finalize (object);
}

static void
spice_mux_class_init (SpiceMuxClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = spice_mux_dispose;
  gobject_class->finalize = spice_mux_finalize;

  /**
   * SpiceMux::closed:
   * @mux: the #SpiceMux
   *
   * The channel broke or spice_mux_stop() was called: the clients
   * were dropped and no frame is multiplexed anymore.
   */
  signals[CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
spice_mux_init (SpiceMux *mux)
{
  mux->clients = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                        NULL, client_unref);
  mux->read_buf = g_malloc (G_MAXUINT16);
}

/**
 * spice_mux_new:
 * @path: the path of the channel, a virtio port on Linux and macOS, a
 *   device interface on Windows
 * @error: a #GError to set on failure, or %NULL to ignore
 *
 * Opens the channel at @path and starts multiplexing the clients over
 * it, see spice_mux_add_client().
 *
 * Returns: (transfer full): a new #SpiceMux, or %NULL when the channel
 * cannot be opened
 */
SpiceMux *
spice_mux_new (const gchar *path, GError **error)
{
  SpiceMux *mux;
  GInputStream *istream;
  GOutputStream *ostream;

  g_return_val_if_fail (path != NULL, NULL);

#ifdef G_OS_UNIX
  {
    gint fd = g_open (path, O_RDWR);
    gint errsv = errno;

    if (fd == -1)
      {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                     "Failed to open %s: %s", path, g_strerror (errsv));
        return NULL;
      }

    wait_for_virtio_host (fd);

    /* the output stream owns the descriptor and closes it, the input
     * stream only borrows it */
    ostream = g_unix_output_stream_new (fd, TRUE);
    istream = g_unix_input_stream_new (fd, FALSE);
  }
#else
  {
    HANDLE handle = CreateFile (path,
                                GENERIC_WRITE | GENERIC_READ,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED,
                                NULL);

    if (handle == INVALID_HANDLE_VALUE)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "%s", g_win32_error_message (GetLastError ()));
        return NULL;
      }

    /* the output stream owns the handle and closes it, the input
     * stream only borrows it */
    ostream = G_OUTPUT_STREAM (g_win32_output_stream_new (handle, TRUE));
    istream = G_INPUT_STREAM (g_win32_input_stream_new (handle, FALSE));
  }
#endif

  mux = g_object_new (SPICE_TYPE_MUX, NULL);
  mux->istream = istream;
  mux->ostream = ostream;
  mux->cancellable = g_cancellable_new ();
  mux->queue = output_queue_new (ostream, mux->cancellable);

  mux_read_header (mux);

  return mux;
}

/**
 * spice_mux_add_client:
 * @mux: a #SpiceMux
 * @connection: the connection of the client
 *
 * Multiplexes @connection over the channel: whatever the client sends
 * is pushed to the channel, tagged with an id chosen by the mux, and
 * the frames tagged with that id are written to the client. The client
 * is dropped when its connection is closed, or with the mux.
 */
void
spice_mux_add_client (SpiceMux *mux, GSocketConnection *connection)
{
  Client *client;

  g_return_if_fail (SPICE_IS_MUX (mux));
  g_return_if_fail (G_IS_SOCKET_CONNECTION (connection));
  g_return_if_fail (!mux->closed);

  client = g_new0 (Client, 1);
  client->ref_count = 1;
  client->connection = g_object_ref (connection);

  /* the id only has to be unique among the live clients: the other
   * end of the mux channel learns it from the data forwarded to it
   * and echoes it back. A monotonically increasing counter cannot
   * collide, unlike the connection pointer it replaces, which may
   * be reused by a later connection (and does not even fit in an
   * int on Win64). */
  client->frame.id = ++mux->next_client_id;

  /* the table owns the client from now on */
  g_hash_table_insert (mux->clients, &client->frame.id, client);
  g_warn_if_fail (g_hash_table_lookup (mux->clients, &client->frame.id));

  g_debug ("new client %" G_GINT64_FORMAT, client->frame.id);
  client_start_read (mux, client);
}

/**
 * spice_mux_stop:
 * @mux: a #SpiceMux
 *
 * Stops the multiplexing and drops the clients: the operations in
 * flight are cancelled, and the #SpiceMux::closed signal is emitted.
 * This is a no-op when the channel already broke.
 */
void
spice_mux_stop (SpiceMux *mux)
{
  g_return_if_fail (SPICE_IS_MUX (mux));

  mux_close (mux);
}
