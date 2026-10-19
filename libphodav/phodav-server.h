/*
  Copyright (C) 2013 Red Hat, Inc.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, see <http://www.gnu.org/licenses/>.
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
 * #PhodavServer:root or the #PhodavServer:root-file property.
 */
G_DECLARE_FINAL_TYPE (PhodavServer, phodav_server, PHODAV, SERVER, GObject)

PhodavServer *phodav_server_new (const gchar *root);
PhodavServer *phodav_server_new_for_root_file (GFile *root);
SoupServer *phodav_server_get_soup_server (PhodavServer *server);

G_END_DECLS
