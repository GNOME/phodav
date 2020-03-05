/*
 * Copyright (C) 2019 Red Hat, Inc.
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
#include <config.h>

#include "output-queue.h"

typedef struct _OutputQueueElem
{
  const guint8 *buf;
  gsize         size;
  PushedCb      cb;
  gpointer      user_data;
} OutputQueueElem;

struct _OutputQueue
{
  GObject        parent_instance;
  GOutputStream *output;
  gboolean       writing;
  GQueue        *queue;
  GCancellable  *cancel;
};

G_DEFINE_TYPE (OutputQueue, output_queue, G_TYPE_OBJECT);

static void output_queue_kick (OutputQueue *q);

static void output_queue_init (OutputQueue *self)
{
  self->queue = g_queue_new ();
}

static void output_queue_finalize (GObject *obj)
{
  OutputQueue *self = OUTPUT_QUEUE (obj);

  g_queue_free_full (self->queue, g_free);
  g_object_unref (self->output);
  g_object_unref (self->cancel);

  G_OBJECT_CLASS (output_queue_parent_class)->finalize (obj);
}

static void output_queue_class_init (OutputQueueClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = output_queue_finalize;
}

OutputQueue* output_queue_new (GOutputStream *output, GCancellable *cancel)
{
  OutputQueue *self = g_object_new (OUTPUT_TYPE_QUEUE, NULL);
  self->output = g_object_ref (output);
  self->cancel = g_object_ref (cancel);
  return self;
}

static void
write_cb (GObject *source_object,
          GAsyncResult *res,
          gpointer user_data)
{
  OutputQueue *q = user_data;
  OutputQueueElem *e;
  GError *err = NULL;

  e = g_queue_pop_head (q->queue);
  g_output_stream_write_all_finish (G_OUTPUT_STREAM (source_object), res, NULL, &err);

  if (e->cb)
    e->cb (q, e->user_data, err);

  g_free (e);
  q->writing = FALSE;
  if (!err)
    output_queue_kick (q);
  g_clear_error (&err);
  g_object_unref (q);
}

static void
output_queue_kick (OutputQueue *q)
{
  OutputQueueElem *e;

  if (!q || q->writing || g_queue_is_empty (q->queue))
    return;

  e = g_queue_peek_head (q->queue);
  q->writing = TRUE;
  g_output_stream_write_all_async (q->output, e->buf, e->size,
    G_PRIORITY_DEFAULT, q->cancel, write_cb, g_object_ref (q));
}

void
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
  g_queue_push_tail (q->queue, e);

  output_queue_kick (q);
}
