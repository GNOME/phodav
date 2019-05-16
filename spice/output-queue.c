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
  OutputQueue  *queue;
  const guint8 *buf;
  gsize         size;
  PushedCb      cb;
  gpointer      user_data;
} OutputQueueElem;

struct _OutputQueue
{
  GObject        parent_instance;
  GOutputStream *output;
  gboolean       flushing;
  guint          idle_id;
  GQueue        *queue;
  GCancellable  *cancel;
};

G_DEFINE_TYPE (OutputQueue, output_queue, G_TYPE_OBJECT);

static void output_queue_init (OutputQueue *self)
{
  self->queue = g_queue_new ();
}

static void output_queue_finalize (GObject *obj)
{
  OutputQueue *self = OUTPUT_QUEUE (obj);

  g_warn_if_fail (g_queue_get_length (self->queue) == 0);
  g_warn_if_fail (!self->flushing);
  g_warn_if_fail (!self->idle_id);

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
    q->idle_id = g_idle_add (output_queue_idle, g_object_ref (q));

  g_free (e);
  g_object_unref (q);
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
  g_output_stream_write_all (q->output, e->buf, e->size, NULL, q->cancel, &error);
  if (e->cb)
    e->cb (q, e->user_data, error);

  if (error)
      goto end;

  q->flushing = TRUE;
  g_output_stream_flush_async (q->output, G_PRIORITY_DEFAULT, q->cancel, output_queue_flush_cb, e);

  q->idle_id = 0;
  return FALSE;

end:
  g_clear_error (&error);
  q->idle_id = 0;
  g_free (e);
  g_object_unref (q);

  return FALSE;
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
  e->queue = q;
  g_queue_push_tail (q->queue, e);

  if (!q->idle_id && !q->flushing)
    q->idle_id = g_idle_add (output_queue_idle, g_object_ref (q));
}
