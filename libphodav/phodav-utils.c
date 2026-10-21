/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
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

  /* got_headers() refuses the oversized bodies while they are
   * received, the chunked ones included; refusing one here too is a
   * last resort, do not parse it */
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

/* does an If-Match / If-None-Match header value (RFC 9110 section 13.1)
 * match the current @etag of the resource, which exists per @exists? */
gboolean
etag_header_matches (const gchar *header, const gchar *etag, gboolean exists)
{
  g_auto(GStrv) etags = NULL;

  if (g_strcmp0 (header, "*") == 0)
    return exists;

  if (!etag)
    return FALSE;

  /* a comma separated list of quoted entity tags; the comparison is
   * weak, so the weakness markers are ignored. (GIO etags do not
   * contain commas, splitting on them is unambiguous.) */
  etags = g_strsplit (header, ",", -1);
  for (guint i = 0; etags[i]; i++)
    {
      const gchar *h = g_strstrip (etags[i]);
      gsize len = strlen (h);

      if (g_str_has_prefix (h, "W/"))
        {
          h += 2;
          len -= 2;
        }

      if (len >= 2 && h[0] == '"' && h[len - 1] == '"')
        {
          h++;
          len -= 2;
        }

      if (strlen (etag) == len && strncmp (etag, h, len) == 0)
        return TRUE;
    }

  return FALSE;
}

/* format @time as an HTTP-date (RFC 9110 section 5.6.7) */
gchar *
http_time_to_string (guint64 time)
{
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_utc ((gint64) time);

  return date ? soup_date_time_to_string (date, SOUP_DATE_HTTP) : NULL;
}

/* do the If-None-Match / If-Modified-Since preconditions of a GET or
 * HEAD request hold, i.e. does the client hold a current
 * representation? RFC 9110 sections 13.1.2 and 13.1.3 */
gboolean
http_request_not_modified (SoupMessageHeaders *headers,
                           const gchar *etag,
                           guint64 mtime)
{
  const gchar *condition;

  condition = soup_message_headers_get_one (headers, "If-None-Match");
  if (condition)
    {
      /* If-None-Match takes precedence over If-Modified-Since, and
       * the resource is known to exist, since its validators come
       * from a successful query */
      return etag_header_matches (condition, etag, TRUE);
    }

  condition = soup_message_headers_get_one (headers, "If-Modified-Since");
  if (condition && mtime != 0)
    {
      g_autoptr(GDateTime) date = soup_date_time_new_from_http_string (condition);

      /* the representation is current when its last modification
       * is not after the condition date */
      if (date)
        return (gint64) mtime <= g_date_time_to_unix (date);
    }

  return FALSE;
}

/* does the If-Range precondition hold for the current representation,
 * i.e. may a client range be applied to it? RFC 9110 section 13.1.5 */
static gboolean
if_range_matches (const gchar *if_range, const gchar *etag, guint64 mtime)
{
  g_autoptr(GDateTime) date = NULL;
  gsize len;

  if (*if_range == '"' || g_str_has_prefix (if_range, "W/"))
    {
      /* an entity-tag is compared strongly, a weak one never matches */
      if (g_str_has_prefix (if_range, "W/"))
        return FALSE;

      len = strlen (if_range);
      return etag != NULL &&
             len >= 2 && if_range[0] == '"' && if_range[len - 1] == '"' &&
             strlen (etag) == len - 2 && strncmp (etag, if_range + 1, len - 2) == 0;
    }

  date = soup_date_time_new_from_http_string (if_range);
  if (!date)
    return FALSE;

  /* a date matches when the representation was not modified after it */
  return mtime != 0 && (gint64) mtime <= g_date_time_to_unix (date);
}

HttpRangeType
http_request_range (SoupMessageHeaders *headers,
                    const gchar *etag,
                    guint64 mtime,
                    guint64 size,
                    guint64 *first,
                    guint64 *last)
{
  const gchar *range, *if_range, *p;
  g_autofree gchar *spec = NULL;
  gchar *end;
  guint64 v;

  range = soup_message_headers_get_one (headers, "Range");
  if (!range)
    return HTTP_RANGE_NONE;

  /* a zero-length representation has no satisfiable range, but
   * serving its empty body is friendlier than 416 */
  if (size == 0)
    return HTTP_RANGE_NONE;

  /* the range only applies while the client still holds the
   * representation it ranges into */
  if_range = soup_message_headers_get_one (headers, "If-Range");
  if (if_range && !if_range_matches (if_range, etag, mtime))
    return HTTP_RANGE_NONE;

  /* an unknown range unit is ignored, RFC 9110 section 14.2 */
  if (g_ascii_strncasecmp (range, "bytes=", 6) != 0)
    {
      g_debug ("ignoring the unsupported Range unit: %s", range);
      return HTTP_RANGE_NONE;
    }

  spec = g_strstrip (g_strdup (range + 6));

  /* only single ranges are served, a request for more falls back
   * to the whole representation */
  if (strchr (spec, ',') != NULL)
    {
      g_debug ("ignoring the multi-range request: %s", range);
      return HTTP_RANGE_NONE;
    }

  if (spec[0] == '-')
    {
      /* a suffix range: the last @v bytes */
      v = g_ascii_strtoull (spec + 1, &end, 10);
      if (end == spec + 1 || *end != '\0')
        return HTTP_RANGE_NONE;

      /* RFC 9110 section 14.1.1 */
      if (v == 0)
        return HTTP_RANGE_UNSATISFIABLE;

      *first = size > v ? size - v : 0;
      *last = size - 1;
      return HTTP_RANGE_PARTIAL;
    }

  v = g_ascii_strtoull (spec, &end, 10);
  if (end == spec || *end != '-')
    return HTTP_RANGE_NONE;

  *first = v;

  p = end + 1;
  if (*p == '\0')
    {
      /* to the end of the representation */
      *last = size - 1;
    }
  else
    {
      v = g_ascii_strtoull (p, &end, 10);
      if (end == p || *end != '\0')
        return HTTP_RANGE_NONE;

      /* a last byte below the first one is invalid, ignore the
       * whole header, RFC 9110 section 14.1.1 */
      if (v < *first)
        return HTTP_RANGE_NONE;

      *last = v;
    }

  if (*first >= size)
    return HTTP_RANGE_UNSATISFIABLE;

  /* a last byte beyond the end is clamped to it */
  if (*last >= size)
    *last = size - 1;

  return HTTP_RANGE_PARTIAL;
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
  xmlDocDumpMemoryEnc (doc, mem, size, "utf-8");
  xmlFreeDoc (doc);
}

void
xml_node_debug (xmlNodePtr node)
{
  g_debug ("%s ns:%s", node->name, node->ns ? (gchar *) node->ns->href : "");
}

static gboolean
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
