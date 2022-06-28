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
#ifndef __PHODAV_UTILS_H__
#define __PHODAV_UTILS_H__

#include <libsoup/soup.h>
#include "phodav-priv.h"

G_BEGIN_DECLS

void             remove_trailing                 (gchar *str, gchar c);

DepthType        depth_from_string               (const gchar *depth);
const gchar *    depth_to_string                 (DepthType depth);
guint            timeout_from_string             (const gchar *timeout);

typedef struct _DavDoc     DavDoc;

struct _DavDoc
{
  xmlDocPtr  doc;
  xmlNodePtr root;

  GUri      *target;
  char      *path;
};

gboolean         davdoc_parse                    (DavDoc *dd, SoupServerMessage *msg,
                                                  SoupMessageBody *body,
                                                  const gchar *name);
void             davdoc_free                     (DavDoc *dd);

void             xml_node_to_string              (xmlNodePtr root, xmlChar **mem, int *size);
gboolean         xml_node_is_element             (xmlNodePtr node);
gboolean         xml_node_has_name               (xmlNodePtr node, const char *name);
gboolean         xml_node_has_name_ns            (xmlNodePtr node, const char *name,
                                                  const char *ns_href);
gboolean         xml_node_has_ns                 (xmlNodePtr node, const char *ns_href);
void             xml_node_debug                  (xmlNodePtr node);
gchar *          xml_node_get_xattr_name         (xmlNodePtr node, const gchar *prefix);

G_END_DECLS

#endif /* __PHODAV_UTILS_H__ */
