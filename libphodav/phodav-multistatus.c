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
#include "phodav-multistatus.h"

Response *
response_new (GList *props, gint status)
{
  Response *r;

  g_return_val_if_fail (props != NULL || status > 0, NULL);

  r = g_slice_new0 (Response);
  r->status = status;
  r->props = props;

  return r;
}

void
response_free (Response *h)
{
  g_list_free_full (h->props, (GDestroyNotify) xmlFreeNode);
  g_slice_free (Response, h);
}

static gchar*
status_to_string (gint status)
{
  return g_strdup_printf ("HTTP/1.1 %d %s",
                          status, soup_status_get_phrase (status));
}

static xmlNodePtr
status_node_new (xmlNsPtr ns, gint status)
{
  xmlNodePtr node;
  gchar *text;

  text = status_to_string (status);
  node = xmlNewNode (ns, BAD_CAST "status");
  xmlAddChild (node, xmlNewText (BAD_CAST text));
  g_free (text);

  return node;
}

static void
add_propstat (xmlNodePtr parent, xmlNsPtr ns, SoupMessage *msg,
              const gchar *path, GList *props)
{
  xmlNodePtr node, propstat, prop = NULL, stnode = NULL;
  GList *s;
  gint status = -1;

  /* better if sorted by status */
  for (s = props; s != NULL; s = s->next)
    {
      node = s->data;
      if (GPOINTER_TO_INT (node->_private) != status)
        {
          status = GPOINTER_TO_INT (node->_private);
          if (stnode)
            xmlAddChild (propstat, stnode);

          stnode = status_node_new (ns, status);
          propstat = xmlNewChild (parent, ns, BAD_CAST "propstat", NULL);
          prop = xmlNewChild (propstat, ns, BAD_CAST "prop", NULL);
        }
      g_return_if_fail (prop != NULL);
      xmlAddChild (prop, node);
      s->data = NULL;
    }

  if (stnode)
    xmlAddChild (propstat, stnode);
}

gint
set_response_multistatus (SoupMessage *msg,
                          GHashTable  *mstatus)
{
  xmlChar *mem = NULL;
  int size;
  xmlNodePtr root;
  GHashTableIter iter;
  Response *resp;
  gchar *path, *text;
  xmlNsPtr ns;

  root = xmlNewNode (NULL, BAD_CAST "multistatus");
  ns = xmlNewNs (root, BAD_CAST "DAV:", BAD_CAST "D");
  xmlSetNs (root, ns);

  g_hash_table_iter_init (&iter, mstatus);
  while (g_hash_table_iter_next (&iter, (gpointer *) &path, (gpointer *) &resp))
    {
      xmlNodePtr response;
      SoupURI *new_uri;

      response = xmlNewChild (root, ns, BAD_CAST "response", NULL);
      new_uri = soup_uri_new_with_base (soup_message_get_uri (msg), path);
      text = soup_uri_to_string (new_uri, FALSE);
      xmlNewChild (response, ns, BAD_CAST "href", BAD_CAST text);
      g_free (text);
      soup_uri_free (new_uri);

      if (resp->props)
        add_propstat (response, ns, msg, path, resp->props);
      else if (resp->status)
        xmlAddChild (response, status_node_new (ns, resp->status));
    }

  xml_node_to_string (root, &mem, &size);
  soup_message_set_response (msg, "application/xml",
                             SOUP_MEMORY_TAKE, (gchar *) mem, size);

  return SOUP_STATUS_MULTI_STATUS;
}
