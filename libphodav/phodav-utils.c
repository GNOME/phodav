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
remove_trailing (gchar *str, gchar c)
{
  gsize len = strlen (str);

  while (len > 0 && str[len - 1] == c)
    len--;

  str[len] = '\0';
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
davdoc_parse (DavDoc *dd, SoupServerMessage *msg, SoupMessageBody *body,
              const gchar *name)
{
  xmlDocPtr doc;
  xmlNodePtr root;
  GUri *uri;

  doc = parse_xml (body->data, body->length, &root, name);
  if (!doc)
    return FALSE;

  uri = soup_server_message_get_uri (msg);

  dd->doc = doc;
  dd->root = root;
  dd->target = uri;
  dd->path = g_uri_unescape_string (g_uri_get_path (uri), "/");

  return TRUE;
}

void
davdoc_free (DavDoc *dd)
{
  if (dd->doc)
    xmlFreeDoc (dd->doc);
  g_free (dd->path);
}

DepthType
depth_from_string (const gchar *depth)
{
  if (!depth)
    return DEPTH_INFINITY;
  else if (!g_strcmp0 (depth, "0"))
    return DEPTH_ZERO;
  else if (!g_strcmp0 (depth, "1"))
    return DEPTH_ONE;
  else if (!g_strcmp0 (depth, "infinity"))
    return DEPTH_INFINITY;

  g_warning ("Invalid depth: %s", depth);
  return DEPTH_INFINITY;
}

guint
timeout_from_string (const gchar *timeout)
{
  if (!timeout ||
      !g_strcmp0 (timeout, "Infinite"))
    return 0;

  if (!g_ascii_strncasecmp (timeout, "Second-", 7))
    return g_ascii_strtoull (timeout + 7, NULL, 10);

  g_return_val_if_reached (0);
}

const gchar *
depth_to_string (DepthType depth)
{
  if (depth == DEPTH_INFINITY)
    return "infinity";
  if (depth == DEPTH_ZERO)
    return "0";
  if (depth == DEPTH_ONE)
    return "1";

  g_return_val_if_reached (NULL);
}

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

void
xml_node_debug (xmlNodePtr node)
{
  g_debug ("%s ns:%s", node->name, node->ns ? (gchar *) node->ns->href : "");
}

gboolean
xml_node_has_ns (xmlNodePtr node, const char *ns_href)
{
  return node->ns && node->ns->href &&
    !g_strcmp0 ((gchar *) node->ns->href, ns_href);

}

gboolean
xml_node_has_name_ns (xmlNodePtr node, const char *name, const char *ns_href)
{
  gboolean has_name;
  gboolean has_ns;

  g_return_val_if_fail (node != NULL, FALSE);

  has_name = has_ns = TRUE;

  if (name)
    has_name = !g_strcmp0 ((gchar *) node->name, name);

  if (ns_href)
    has_ns = xml_node_has_ns (node, ns_href);

  return has_name && has_ns;
}

gboolean
xml_node_has_name (xmlNodePtr node, const char *name)
{
  g_return_val_if_fail (node != NULL, FALSE);

  return xml_node_has_name_ns (node, name, "DAV:");
}

gboolean G_GNUC_PURE
xml_node_is_element (xmlNodePtr node)
{
  return node->type == XML_ELEMENT_NODE && node->name != NULL;
}

gchar *
xml_node_get_xattr_name (xmlNodePtr node, const gchar *prefix)
{
  const gchar *ns = node->ns ? (gchar *) node->ns->href : NULL;
  const gchar *name = (gchar *) node->name;

  if (!name)
    return NULL;

  if (ns)
    return g_strdup_printf ("%s%s#%s", prefix, ns, name);
  else
    return g_strdup_printf ("%s%s", prefix, name);
}
