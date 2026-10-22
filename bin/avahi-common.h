/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

gboolean avahi_client_start (const gchar *name, guint port, gboolean local, gboolean tls, GError **error);
void avahi_client_stop (void);
