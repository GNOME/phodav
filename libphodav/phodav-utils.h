/*
 * Copyright (C) 2013 Red Hat, Inc.
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
#pragma once

#include <libsoup/soup.h>

#include "phodav-priv.h"

G_BEGIN_DECLS

void remove_trailing (gchar *str, gchar c);
gboolean path_has_dot_dot_segment (const gchar *path);

DepthType depth_from_string (const gchar *depth);
const gchar *depth_to_string (DepthType depth);
guint timeout_from_string (const gchar *timeout);

/* is @path the tree rooted at @root, or one of its members? */
gboolean path_is_covered (const gchar *path, const gchar *root);

typedef struct _DavDoc DavDoc;

struct _DavDoc
{
  xmlDocPtr doc;
  xmlNodePtr root;

  GUri *target;
  gchar *path;
};

gboolean davdoc_parse (DavDoc *dd, SoupServerMessage *msg, SoupMessageBody *body, const gchar *name);
void davdoc_free (DavDoc *dd);

void xml_node_to_string (xmlNodePtr root, xmlChar **mem, gint *size);
gboolean xml_node_is_element (xmlNodePtr node);
gboolean xml_node_has_name (xmlNodePtr node, const gchar *name);
gboolean xml_node_has_name_ns (xmlNodePtr node, const gchar *name, const gchar *ns_href);
gboolean xml_node_has_ns (xmlNodePtr node, const gchar *ns_href);
void xml_node_debug (xmlNodePtr node);
gchar *xml_node_get_xattr_name (xmlNodePtr node, const gchar *prefix);

G_END_DECLS
