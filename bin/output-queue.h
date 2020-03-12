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

#ifndef __OUTPUT_QUEUE_H
#define __OUTPUT_QUEUE_H

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define OUTPUT_TYPE_QUEUE output_queue_get_type ()
G_DECLARE_FINAL_TYPE (OutputQueue, output_queue, OUTPUT, QUEUE, GObject);

OutputQueue* output_queue_new (GOutputStream *output, GCancellable *cancel);

typedef void (*PushedCb) (OutputQueue *q, gpointer user_data, GError *error);

void output_queue_push (OutputQueue *q, const guint8 *buf, gsize size,
                        PushedCb pushed_cb, gpointer user_data);

G_END_DECLS

#endif
