/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2; -*- */
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

#include "phodav-utils.h"

void
xml_node_to_string (xmlNodePtr root, xmlChar **mem, int *size)
{
  xmlDocPtr doc;

  doc = xmlNewDoc (BAD_CAST "1.0");
  xmlDocSetRootElement (doc, root);
  // xmlReconciliateNs
  xmlDocDumpMemoryEnc (doc, mem, size, "utf-8");
  /* FIXME: validate document? */
  /*FIXME, pretty print?*/
  xmlFreeDoc (doc);
}

static xmlDocPtr
parse_xml (const gchar  *data,
           const goffset len,
           xmlNodePtr   *root,
           const char   *name)
{
  xmlDocPtr doc;

  doc = xmlReadMemory (data, len,
                       "request.xml",
                       NULL,
                       XML_PARSE_NONET |
                       XML_PARSE_NOWARNING |
                       XML_PARSE_NOBLANKS |
                       XML_PARSE_NSCLEAN |
                       XML_PARSE_NOCDATA |
                       XML_PARSE_COMPACT);
  if (doc == NULL)
    {
      g_debug ("Could not parse request");
      return NULL;
    }
  if (!(doc->properties & XML_DOC_NSVALID))
    {
      g_debug ("Could not parse request, NS errors");
      xmlFreeDoc (doc);
      return NULL;
    }

  *root = xmlDocGetRootElement (doc);

  if (*root == NULL || (*root)->children == NULL)
    {
      g_debug ("Empty request");
      xmlFreeDoc (doc);
      return NULL;
    }

  if (g_strcmp0 ((char *) (*root)->name, name))
    {
      g_debug ("Unexpected request");
      xmlFreeDoc (doc);
      return NULL;
    }

  return doc;
}

gboolean
davdoc_parse (DavDoc *dd, SoupMessage *msg, SoupMessageBody *body,
              const gchar *name)
{
  xmlDocPtr doc;
  xmlNodePtr root;
  SoupURI *uri;

  doc = parse_xml (body->data, body->length, &root, name);
  if (!doc)
    return FALSE;

  uri = soup_message_get_uri (msg);

  dd->doc = doc;
  dd->root = root;
  dd->target = uri;
  dd->path = g_uri_unescape_string (uri->path, "/");

  return TRUE;
}

void
davdoc_free (DavDoc *dd)
{
  if (dd->doc)
    xmlFreeDoc (dd->doc);
  g_free (dd->path);
}
