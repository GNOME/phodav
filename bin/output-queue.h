/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define OUTPUT_TYPE_QUEUE output_queue_get_type ()
G_DECLARE_FINAL_TYPE (OutputQueue, output_queue, OUTPUT, QUEUE, GObject);

OutputQueue *output_queue_new (GOutputStream *output, GCancellable *cancel);

typedef void (*PushedCb) (OutputQueue *q, gpointer user_data, GError *error);

void output_queue_push (OutputQueue *q, const guint8 *buf, gsize size, PushedCb pushed_cb, gpointer user_data);

G_END_DECLS
