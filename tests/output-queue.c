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

/* Unit tests for the write queue used by spice-webdavd to serialize
 * the output of its clients into the multiplexing channel. */

#include <gio/gio.h>
#include <glib.h>

#include "output-queue.h"

typedef struct
{
  GMainLoop *loop;
  guint callbacks;
  GError *error;
} QueueData;

static void
pushed_cb (OutputQueue *queue, gpointer user_data, GError *error)
{
  QueueData *data = user_data;

  data->callbacks++;
  if (error)
    data->error = g_error_copy (error);
  g_main_loop_quit (data->loop);
}

static void
wait_for_callbacks (QueueData *data, guint n)
{
  while (data->callbacks < n)
    g_main_context_iteration (NULL, TRUE);
}

/* a stream which rejects every write; the queue falls back to the
 * synchronous write operation of this class */
#define TYPE_FAILING_OUTPUT_STREAM (failing_output_stream_get_type ())
G_DECLARE_FINAL_TYPE (FailingOutputStream, failing_output_stream, FAILING, OUTPUT_STREAM, GOutputStream)

struct _FailingOutputStream
{
  GOutputStream parent_instance;
};

G_DEFINE_TYPE (FailingOutputStream, failing_output_stream, G_TYPE_OUTPUT_STREAM)

static gssize
failing_output_stream_write (GOutputStream *stream, const void *buffer, gsize count, GCancellable *cancellable, GError **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "write me not");
  return -1;
}

static void
failing_output_stream_init (FailingOutputStream *self)
{
}

static void
failing_output_stream_class_init (FailingOutputStreamClass *klass)
{
  GOutputStreamClass *ostream_class = G_OUTPUT_STREAM_CLASS (klass);

  ostream_class->write_fn = failing_output_stream_write;
}

static void
test_queue (void)
{
  g_autoptr(GOutputStream) output = NULL;
  g_autoptr(GCancellable) cancel = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(OutputQueue) queue = NULL;
  QueueData data = { NULL, 0, NULL };

  loop = g_main_loop_new (NULL, FALSE);
  data.loop = loop;

  output = g_memory_output_stream_new_resizable ();
  cancel = g_cancellable_new ();
  queue = output_queue_new (output, cancel);

  /* NULL is a valid callback */
  output_queue_push (queue, (const guint8 *) "one", 3, NULL, NULL);
  output_queue_push (queue, (const guint8 *) "two", 3, pushed_cb, &data);
  /* the writes are serialized, this one waits for the previous */
  output_queue_push (queue, (const guint8 *) "three", 5, pushed_cb, &data);

  wait_for_callbacks (&data, 2);
  g_assert_no_error (data.error);

  /* the buffers were written in order, without interleaving */
  g_assert_true (g_output_stream_close (output, NULL, NULL));
  {
    g_autoptr(GBytes) contents = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (output));

    g_assert_cmpmem (g_bytes_get_data (contents, NULL), g_bytes_get_size (contents),
                     "onetwothree", 11);
  }

  /* no pending write left, the queue is idle */
  g_assert_false (g_output_stream_has_pending (output));
}

static void
test_queue_error (void)
{
  g_autoptr(GOutputStream) output = NULL;
  g_autoptr(GCancellable) cancel = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(OutputQueue) queue = NULL;
  QueueData data = { NULL, 0, NULL };

  loop = g_main_loop_new (NULL, FALSE);
  data.loop = loop;

  output = g_object_new (TYPE_FAILING_OUTPUT_STREAM, NULL);
  cancel = g_cancellable_new ();
  queue = output_queue_new (output, cancel);

  output_queue_push (queue, (const guint8 *) "data", 4, pushed_cb, &data);

  wait_for_callbacks (&data, 1);

  /* the write error is reported to the callback */
  g_assert_nonnull (data.error);
  g_assert_error (data.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&data.error);
}

gint
main (gint argc, gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/output-queue/order", test_queue);
  g_test_add_func ("/output-queue/error", test_queue_error);

  return g_test_run ();
}
