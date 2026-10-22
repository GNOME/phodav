/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/* The multiplexing channel of the spice WebDAV daemon, normally the
 * virtio port of the guest. */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define SPICE_TYPE_MUX spice_mux_get_type ()
G_DECLARE_FINAL_TYPE (SpiceMux, spice_mux, SPICE, MUX, GObject)

SpiceMux *spice_mux_new (const gchar *path, GError **error);
void spice_mux_add_client (SpiceMux *mux, GSocketConnection *connection);
void spice_mux_stop (SpiceMux *mux);

G_END_DECLS
