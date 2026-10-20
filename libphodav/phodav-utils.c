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

#include "config.h"

#include "phodav-utils.h"

void
remove_trailing (gchar *str, gchar c)
{
  gsize len = strlen (str);

  while (len > 0 && str[len - 1] == c)
    len--;

  str[len] = '\0';
}

gboolean
path_has_dot_dot_segment (const gchar *path)
{
  const gchar *p = path;

  while (*p)
    {
      const gchar *end = p;

      while (*end && !G_IS_DIR_SEPARATOR (*end))
        end++;

      if (end - p == 2 && p[0] == '.' && p[1] == '.')
        return TRUE;

      p = *end ? end + 1 : end;
    }

  return FALSE;
}

static xmlDocPtr
parse_xml (const gchar *data,
           const goffset len,
           xmlNodePtr *root,
           const gchar *name)
{
  /* parse with a private context, so that the libxml2 error
   * details are available when the document is rejected */
  g_autoptr(xmlParserCtxt) ctxt = xmlNewParserCtxt ();
  g_autoptr(xmlDoc) doc = NULL;

  if (ctxt == NULL)
    {
      g_debug ("Could not create the parser context");
      return NULL;
    }

  doc = xmlCtxtReadMemory (ctxt, data, len,
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
      const xmlError *last = &ctxt->lastError;

      /* the libxml2 messages end with a newline */
      g_autofree gchar *message = g_strdup (last->message ? (gchar *) last->message : "");

      g_strchomp (message);
      g_debug ("Could not parse request: %s", *message ? message : "no error details");
      return NULL;
    }

  if (!(doc->properties & XML_DOC_NSVALID))
    {
      g_debug ("Could not parse request, NS errors");
      return NULL;
    }

  *root = xmlDocGetRootElement (doc);

  if (*root == NULL || (*root)->children == NULL)
    {
      g_debug ("Empty request");
      return NULL;
    }

  if (g_strcmp0 ((char *) (*root)->name, name) != 0)
    {
      g_debug ("Unexpected request");
      return NULL;
    }

  return g_steal_pointer (&doc);
}

gboolean
davdoc_parse (DavDoc *dd, SoupServerMessage *msg, SoupMessageBody *body, const gchar *name)
{
  xmlDocPtr doc;
  xmlNodePtr root;
  GUri *uri;

  /* a chunked body has no Content-Length for got_headers() to check,
   * refuse it here rather than parsing it */
  if (body->length > PHODAV_MAX_REQUEST_BODY)
    {
      g_debug ("Refusing to parse the oversized request body (%" G_GOFFSET_FORMAT " bytes)", body->length);
      return FALSE;
    }

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

gboolean
depth_from_string (const gchar *depth, DepthType *out)
{
  if (!depth)
    {
      /* RFC 4918 section 9.1: a request without a Depth header is
       * treated as if "infinity" was submitted */
      *out = DEPTH_INFINITY;
      return TRUE;
    }
  else if (g_strcmp0 (depth, "0") == 0)
    {
      *out = DEPTH_ZERO;
      return TRUE;
    }
  else if (g_strcmp0 (depth, "1") == 0)
    {
      *out = DEPTH_ONE;
      return TRUE;
    }
  else if (g_strcmp0 (depth, "infinity") == 0)
    {
      *out = DEPTH_INFINITY;
      return TRUE;
    }

  g_debug ("Invalid depth: %s", depth);
  return FALSE;
}

gboolean
path_is_covered (const gchar *path, const gchar *root)
{
  gsize len = strlen (root);

  if (g_strcmp0 (path, root) == 0)
    return TRUE;

  /* a lock on the root covers everything */
  if (g_strcmp0 (root, "/") == 0)
    return path[0] == '/';

  return g_str_has_prefix (path, root) && path[len] == '/';
}

guint
timeout_from_string (const gchar *timeout)
{
  if (!timeout ||
      g_strcmp0 (timeout, "Infinite") == 0)
    return 0;

  if (g_ascii_strncasecmp (timeout, "Second-", 7) == 0)
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
xml_node_to_string (xmlNodePtr root, xmlChar **mem, gint *size)
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
xml_node_has_ns (xmlNodePtr node, const gchar *ns_href)
{
  return node->ns && node->ns->href &&
         !g_strcmp0 ((gchar *) node->ns->href, ns_href);
}

gboolean
xml_node_has_name_ns (xmlNodePtr node, const gchar *name, const gchar *ns_href)
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
xml_node_has_name (xmlNodePtr node, const gchar *name)
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
