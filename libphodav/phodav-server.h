/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <glib-object.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define PHODAV_TYPE_SERVER phodav_server_get_type ()

/**
 * PhodavServer:
 *
 * A WebDAV server implementation, see RFC 4918.
 *
 * The directory to share is set at construction time, with either the
 * #PhodavServer:root or the #PhodavServer:root-file property. An
 * already configured #SoupServer may also be given at construction
 * time with the #PhodavServer:server property, otherwise a new one
 * is created.
 */
G_DECLARE_FINAL_TYPE (PhodavServer, phodav_server, PHODAV, SERVER, GObject)

PhodavServer *phodav_server_new (const gchar *root);
PhodavServer *phodav_server_new_for_root_file (GFile *root);
SoupServer *phodav_server_get_soup_server (PhodavServer *server);

G_END_DECLS
