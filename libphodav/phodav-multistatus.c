/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-multistatus.h"

#include "phodav-utils.h"

ResponseProp *
response_prop_new (xmlNodePtr node, gint status)
{
  ResponseProp *prop;

  g_return_val_if_fail (node != NULL, NULL);

  prop = g_new0 (ResponseProp, 1);
  prop->node = node;
  prop->status = status;

  return prop;
}

void
response_prop_free (ResponseProp *prop)
{
  if (!prop)
    return;

  if (prop->node)
    xmlFreeNode (prop->node);

  g_free (prop);
}

static gint
response_prop_compare (gconstpointer a, gconstpointer b)
{
  const ResponseProp *pa = a;
  const ResponseProp *pb = b;

  return pa->status - pb->status;
}

Response *
response_new (GList *props, gint status)
{
  Response *r;

  g_return_val_if_fail (props != NULL || status > 0, NULL);

  r = g_new0 (Response, 1);
  r->status = status;
  /* the response groups the properties by status */
  r->props = g_list_sort (props, response_prop_compare);

  return r;
}

void
response_free (Response *h)
{
  g_list_free_full (h->props, (GDestroyNotify) response_prop_free);
  g_free (h);
}

static gchar *
status_to_string (gint status)
{
  return g_strdup_printf ("HTTP/1.1 %d %s",
                          status, soup_status_get_phrase (status));
}

static xmlNodePtr
status_node_new (xmlNsPtr ns, gint status)
{
  xmlNodePtr node;
  g_autofree gchar *text = NULL;

  text = status_to_string (status);
  node = xmlNewNode (ns, BAD_CAST "status");
  xmlAddChild (node, xmlNewText (BAD_CAST text));

  return node;
}

static void
add_propstat (xmlNodePtr parent, xmlNsPtr ns, GList *props)
{
  xmlNodePtr node, propstat, prop = NULL, stnode = NULL;
  GList *s;
  gint status = -1;

  /* the response_new() list is sorted by status */
  for (s = props; s != NULL; s = s->next)
    {
      ResponseProp *response_prop = s->data;

      node = response_prop->node;
      if (response_prop->status != status)
        {
          status = response_prop->status;
          if (stnode)
            xmlAddChild (propstat, stnode);

          stnode = status_node_new (ns, status);
          propstat = xmlNewChild (parent, ns, BAD_CAST "propstat", NULL);
          prop = xmlNewChild (propstat, ns, BAD_CAST "prop", NULL);
        }
      g_return_if_fail (prop != NULL);
      xmlAddChild (prop, node);
      /* the response propstat owns the node now */
      response_prop->node = NULL;
    }

  if (stnode)
    xmlAddChild (propstat, stnode);
}

gint
set_response_multistatus (PhodavRequest *req,
                          GHashTable *mstatus)
{
  xmlChar *mem = NULL;
  gint size;
  xmlNodePtr root;
  GHashTableIter iter;
  Response *resp;
  gchar *path;
  xmlNsPtr ns;

  root = xmlNewNode (NULL, BAD_CAST "multistatus");
  ns = xmlNewNs (root, BAD_CAST "DAV:", BAD_CAST "D");
  xmlSetNs (root, ns);

  g_hash_table_iter_init (&iter, mstatus);
  while (g_hash_table_iter_next (&iter, (gpointer *) &path, (gpointer *) &resp))
    {
      xmlNodePtr response;

      response = xmlNewChild (root, ns, BAD_CAST "response", NULL);

      /* the key is the decoded resource path: escape it byte-wise
       * for the href, where a client would otherwise parse the
       * characters of the names as URI delimiters ("?" starts a
       * query, "#" a fragment...). libxml2 escapes the XML special
       * characters of the text on its own, when the document is
       * serialized. */
      g_autofree gchar *escaped = g_uri_escape_bytes ((const guint8 *) path, strlen (path),
                                                      G_URI_RESERVED_CHARS_ALLOWED_IN_PATH);
      g_autoptr(GUri) new_uri = g_uri_parse_relative (phodav_request_get_uri (req), escaped,
                                                      SOUP_HTTP_URI_FLAGS, NULL);
      g_autofree gchar *text = new_uri ? g_uri_to_string (new_uri) : g_strdup (escaped);
      xmlNodePtr href = xmlNewChild (response, ns, BAD_CAST "href", NULL);

      /* xmlNewText() takes the raw name: the XML special characters
       * are escaped when the document is serialized */
      xmlAddChild (href, xmlNewText (BAD_CAST text));

      if (resp->props)
        add_propstat (response, ns, resp->props);
      else if (resp->status)
        xmlAddChild (response, status_node_new (ns, resp->status));
    }

  xml_node_to_string (root, &mem, &size);
  phodav_request_set_response (req, "application/xml",
                               SOUP_MEMORY_TAKE, (gchar *) mem, size);

  return SOUP_STATUS_MULTI_STATUS;
}
