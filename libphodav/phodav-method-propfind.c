/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-multistatus.h"
#include "phodav-path.h"
#include "phodav-priv.h"
#include "phodav-utils.h"
#include "phodav-xattr.h"

typedef struct _PropFind
{
  PropFindType type;
  GHashTable *props;
} PropFind;

typedef struct _PropFindContext
{
  PathHandler *handler;
  PropFind *pf;
  const gchar *path;
  GFileInfo *info;
  GCancellable *cancellable;
} PropFindContext;

typedef ResponseProp *(*PropFunc) (const PropFindContext *ctx, xmlNsPtr ns);

/* the requested properties are collected in a table keyed on the
 * element name and namespace href, so that duplicated <prop> entries
 * are only answered once */
static guint
prop_node_hash (gconstpointer p)
{
  xmlNodePtr node = (xmlNodePtr) p;
  const gchar *href = node->ns ? (gchar *) node->ns->href : NULL;

  return g_str_hash ((gchar *) node->name) + (href ? g_str_hash (href) : 0);
}

static gboolean
prop_node_equal (gconstpointer a, gconstpointer b)
{
  xmlNodePtr na = (xmlNodePtr) a;
  xmlNodePtr nb = (xmlNodePtr) b;
  const gchar *ha = na->ns ? (gchar *) na->ns->href : NULL;
  const gchar *hb = nb->ns ? (gchar *) nb->ns->href : NULL;

  return g_strcmp0 ((gchar *) na->name, (gchar *) nb->name) == 0 &&
         g_strcmp0 (ha, hb) == 0;
}

static PropFind *
propfind_new (void)
{
  PropFind *pf = g_new0 (PropFind, 1);

  pf->props = g_hash_table_new (prop_node_hash, prop_node_equal);
  return pf;
}

static void
propfind_free (PropFind *pf)
{
  if (!pf)
    return;

  g_hash_table_unref (pf->props);
  g_free (pf);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PropFind, propfind_free)

static ResponseProp *
prop_resourcetype (const PropFindContext *ctx, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "resourcetype");

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  if (g_file_info_get_file_type (ctx->info) == G_FILE_TYPE_DIRECTORY)
    xmlNewChild (node, ns, BAD_CAST "collection", NULL);
  else if (g_file_info_get_file_type (ctx->info) != G_FILE_TYPE_REGULAR)
    {
      /* a special file is no DAV collection nor a document: the
       * property is not found for it */
      g_debug ("PROPFIND: %s is not a directory or a regular file", ctx->path);
      status = SOUP_STATUS_NOT_FOUND;
    }

end:
  return response_prop_new (node, status);
}

static ResponseProp *
prop_supportedlock (const PropFindContext *ctx, xmlNsPtr ns)
{
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "supportedlock");

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  {
    xmlNodePtr entry = xmlNewChild (node, NULL, BAD_CAST "lockentry", NULL);
    xmlNodePtr scope = xmlNewChild (entry, NULL, BAD_CAST "lockscope", NULL);
    xmlNewChild (scope, NULL, BAD_CAST "exclusive", NULL);
    xmlNodePtr type = xmlNewChild (entry, NULL, BAD_CAST "locktype", NULL);
    xmlNewChild (type, NULL, BAD_CAST "write", NULL);
  }

  {
    xmlNodePtr entry = xmlNewChild (node, NULL, BAD_CAST "lockentry", NULL);
    xmlNodePtr scope = xmlNewChild (entry, NULL, BAD_CAST "lockscope", NULL);
    xmlNewChild (scope, NULL, BAD_CAST "shared", NULL);
    xmlNodePtr type = xmlNewChild (entry, NULL, BAD_CAST "locktype", NULL);
    xmlNewChild (type, NULL, BAD_CAST "write", NULL);
  }

end:
  return response_prop_new (node, SOUP_STATUS_OK);
}

static ResponseProp *
prop_lockdiscovery (const PropFindContext *ctx, xmlNsPtr ns)
{
  PhodavServer *server = handler_get_server (ctx->handler);
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "lockdiscovery");

  if (ctx->pf->type != PROPFIND_PROPNAME)
    phodav_lock_store_lockdiscovery (server, ctx->path, node);

  return response_prop_new (node, SOUP_STATUS_OK);
}

typedef enum
{
  NODE_DATE_FORMAT_HTTP,
  NODE_DATE_FORMAT_ISO8601
} NodeDateFormat;

static void
node_add_time (xmlNodePtr node, guint64 time, NodeDateFormat format)
{
  g_autoptr(GDateTime) date = NULL;
  g_autofree gchar *text = NULL;

  g_warn_if_fail (time != 0);
  date = g_date_time_new_from_unix_utc (time);
  switch (format)
    {
    case NODE_DATE_FORMAT_HTTP:
      text = soup_date_time_to_string (date, SOUP_DATE_HTTP);
      break;
    case NODE_DATE_FORMAT_ISO8601:
      text = g_date_time_format_iso8601 (date);
      break;
    }
  xmlAddChild (node, xmlNewText (BAD_CAST text));
}

static ResponseProp *
prop_creationdate (const PropFindContext *ctx, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "creationdate");
  guint64 time;

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  time = g_file_info_get_attribute_uint64 (ctx->info,
                                           G_FILE_ATTRIBUTE_TIME_CREATED);

  /* windows seems to want this, even apache returns modified time */
  if (time == 0)
    time = g_file_info_get_attribute_uint64 (ctx->info,
                                             G_FILE_ATTRIBUTE_TIME_MODIFIED);

  if (time == 0)
    status = SOUP_STATUS_NOT_FOUND;
  else
    node_add_time (node, time, NODE_DATE_FORMAT_ISO8601);

end:
  return response_prop_new (node, status);
}

static ResponseProp *
prop_getlastmodified (const PropFindContext *ctx, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "getlastmodified");
  guint64 time;

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  time = g_file_info_get_attribute_uint64 (ctx->info,
                                           G_FILE_ATTRIBUTE_TIME_MODIFIED);

  if (time == 0)
    status = SOUP_STATUS_NOT_FOUND;
  else
    node_add_time (node, time, NODE_DATE_FORMAT_HTTP);

end:
  return response_prop_new (node, status);
}

static ResponseProp *
prop_getcontentlength (const PropFindContext *ctx, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "getcontentlength");
  guint64 size;
  g_autofree gchar *text = NULL;

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  size = g_file_info_get_attribute_uint64 (ctx->info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
  text = g_strdup_printf ("%" G_GUINT64_FORMAT, size);
  xmlAddChild (node, xmlNewText (BAD_CAST text));

end:
  return response_prop_new (node, status);
}

static ResponseProp *
prop_getcontenttype (const PropFindContext *ctx, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "getcontenttype");
  const gchar *type;

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  type = g_file_info_get_attribute_string (ctx->info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
  if (type == NULL)
    status = SOUP_STATUS_NOT_FOUND;
  else
    xmlAddChild (node, xmlNewText (BAD_CAST type));

end:
  return response_prop_new (node, status);
}

static ResponseProp *
prop_displayname (const PropFindContext *ctx, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "displayname");
  const gchar *name;

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  name = g_file_info_get_display_name (ctx->info);
  if (name == NULL)
    status = SOUP_STATUS_NOT_FOUND;
  else
    xmlAddChild (node, xmlNewText (BAD_CAST name));

end:
  return response_prop_new (node, status);
}

static ResponseProp *
prop_getetag (const PropFindContext *ctx, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "getetag");
  g_autofree gchar *etag = NULL;

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  etag = phodav_etag_from_info (ctx->info);
  if (etag)
    {
      g_autofree gchar *tmp = g_strdup_printf ("\"%s\"", etag);
      xmlAddChild (node, xmlNewText (BAD_CAST tmp));
    }
  else
    {
      status = SOUP_STATUS_NOT_FOUND;
    }

end:
  return response_prop_new (node, status);
}

static ResponseProp *
prop_executable (const PropFindContext *ctx, xmlNsPtr ns G_GNUC_UNUSED)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (NULL, BAD_CAST "executable");
  gboolean exec;

  xmlNewNs (node, BAD_CAST "http://apache.org/dav/props/", NULL);

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  exec = g_file_info_get_attribute_boolean (ctx->info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE);
  if (g_file_info_get_file_type (ctx->info) == G_FILE_TYPE_DIRECTORY)
    exec = FALSE;

  xmlAddChild (node, xmlNewText (exec ? BAD_CAST "T" : BAD_CAST "F"));

end:
  return response_prop_new (node, status);
}

static ResponseProp *
prop_quota_available (const PropFindContext *ctx, xmlNsPtr ns)
{
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFileInfo) info = NULL;
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "quota-available-bytes");
  GCancellable *cancellable = ctx->cancellable;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *tmp = NULL;
  guint64 size;

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  file = phodav_path_resolve (ctx->handler, ctx->path, PHODAV_PATH_EXISTING,
                              &status);
  if (!file)
    goto end;

  info = g_file_query_filesystem_info (file, "filesystem::*",
                                       cancellable, &error);

  if (error)
    {
      g_debug ("PROPFIND: filesystem info of %s failed: %s", ctx->path, error->message);
      status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
      goto end;
    }

  size = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);

  tmp = g_strdup_printf ("%" G_GUINT64_FORMAT, size);
  xmlAddChild (node, xmlNewText (BAD_CAST tmp));

end:
  return response_prop_new (node, status);
}

static ResponseProp *
prop_quota_used (const PropFindContext *ctx, xmlNsPtr ns)
{
  GFile *file = NULL;
  gint status = SOUP_STATUS_OK;
  GCancellable *cancellable = ctx->cancellable;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "quota-used-bytes");
  guint64 disk_usage = 0;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *tmp = NULL;

  if (ctx->pf->type == PROPFIND_PROPNAME)
    goto end;

  file = phodav_path_resolve (ctx->handler, ctx->path, PHODAV_PATH_EXISTING,
                              &status);
  if (!file)
    goto end;

  if (!g_file_measure_disk_usage (file,
                                  G_FILE_MEASURE_NONE,
                                  cancellable,
                                  NULL, NULL,
                                  &disk_usage, NULL, NULL,
                                  &error))
    {
      g_debug ("PROPFIND: disk usage of %s could not be measured: %s", ctx->path, error->message);
      status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
      goto end;
    }

  tmp = g_strdup_printf ("%" G_GUINT64_FORMAT, disk_usage);
  xmlAddChild (node, xmlNewText (BAD_CAST tmp));

end:
  g_clear_object (&file);
  return response_prop_new (node, status);
}

static void
prop_add (GList **stat, ResponseProp *prop)
{
  *stat = g_list_append (*stat, prop);
}

#define PROP(Name, Info) { G_STRINGIFY (Name), G_PASTE (prop_, Name), Info }
static const struct _PropList
{
  const gchar *name;
  PropFunc func;
  gboolean need_info;
  gboolean slow;

} prop_list[] = {
  PROP (resourcetype, 1),
  PROP (creationdate, 1),
  PROP (getlastmodified, 1),
  PROP (getcontentlength, 1),
  PROP (getcontenttype, 1),
  PROP (displayname, 1),
  PROP (getetag, 1),
  PROP (executable, 1),
  PROP (supportedlock, 0),
  PROP (lockdiscovery, 0),
  {
    "quota-available-bytes",
    prop_quota_available,
  },
  {
    "quota-used-bytes",
    prop_quota_used,
    FALSE,
    TRUE,
  }
};

static ResponseProp *
prop_xattr (const gchar *xattr)
{
  xmlNodePtr node;
  /* the attribute names are owned by the GFileInfo, copy before
   * splitting the string in place */
  g_autofree gchar *attr = g_strdup (xattr + 7);
  gchar *name;

  name = g_utf8_strchr (attr, -1, '#');

  if (name)
    {
      /* <ns-href>#<name> */
      *name = '\0';
      node = xmlNewNode (NULL, BAD_CAST name + 1);
      xmlNewNs (node, BAD_CAST attr, NULL);
    }
  else
    node = xmlNewNode (NULL, BAD_CAST attr);

  return response_prop_new (node, SOUP_STATUS_OK);
}

/* GIO escapes non-ASCII bytes in extended attribute values, which breaks
 * UTF-8 property values. Read the raw value when possible and fall back
 * to the escaped GIO value otherwise. */
static gchar *
xattr_get_value (GFile *file, GFileInfo *info, const gchar *xattr)
{
#ifdef HAVE_SYS_XATTR_H
  g_autofree gchar *path = g_file_get_path (file);

  if (path)
    {
      /* the "xattr::" namespace lists user attributes only */
      g_autofree gchar *name = g_strdup_printf ("user.%s", xattr + strlen ("xattr::"));
      gssize len = phodav_xattr_get (path, name, NULL, 0);

      if (len > 0)
        {
          g_autofree gchar *value = g_malloc (len + 1);

          if (phodav_xattr_get (path, name, value, len) == len)
            {
              value[len] = '\0';

              if (g_utf8_validate (value, len, NULL))
                return g_steal_pointer (&value);
            }
        }
    }
#endif

  return g_strdup (g_file_info_get_attribute_string (info, xattr));
}

#define FILE_QUERY "standard::*,time::*,access::*,etag::*,unix::inode,xattr::*"
static GList *
propfind_populate (PathHandler *handler, GFile *file, const gchar *path, PropFind *pf, GFileInfo *info, xmlNsPtr ns, GCancellable *cancellable)
{
  GHashTableIter iter;
  xmlNodePtr requested;
  GList *stat = NULL;
  gint i;
  PropFindContext ctx = {
    .handler = handler,
    .pf = pf,
    .path = path,
    .info = info,
    .cancellable = cancellable,
  };

  if (pf->type == PROPFIND_ALLPROP || pf->type == PROPFIND_PROPNAME)
    {
      for (i = 0; i < G_N_ELEMENTS (prop_list); i++)
        {
          if (pf->type != PROPFIND_PROPNAME)
            {
              if (prop_list[i].need_info && !info)
                continue;
              if (prop_list[i].slow)
                continue;
            }

          /* perhaps not include the 404? */
          prop_add (&stat, prop_list[i].func (&ctx, ns));
        }

      if (info)
        {
          gchar **attrs = g_file_info_list_attributes (info, "xattr");

          for (i = 0; attrs[i]; i++)
            prop_add (&stat, prop_xattr (attrs[i]));

          g_strfreev (attrs);
        }

      goto end;
    }

  g_hash_table_iter_init (&iter, pf->props);
  while (g_hash_table_iter_next (&iter, (gpointer *) &requested, NULL))
    {
      ResponseProp *prop = NULL;

      for (i = 0; i < G_N_ELEMENTS (prop_list); i++)
        {
          if (xml_node_has_name (requested, prop_list[i].name))
            {
              prop = prop_list[i].func (&ctx, ns);
              break;
            }
        }

      if (i == G_N_ELEMENTS (prop_list))
        {
          g_autofree gchar *xattr = xml_node_get_xattr_name (requested, "xattr::");
          g_autofree gchar *val = NULL;
          xmlNodePtr node = xmlCopyNode (requested, 2);
          gint status = SOUP_STATUS_NOT_FOUND;

          if (xattr)
            val = xattr_get_value (file, info, xattr);

          if (val)
            {
              xmlAddChild (node, xmlNewText (BAD_CAST val));
              status = SOUP_STATUS_OK;
            }
          else
            xml_node_debug (node);

          prop = response_prop_new (node, status);
        }

      prop_add (&stat, prop);
    }

end:
  return stat;
}

static gint
propfind_query_zero (PhodavRequest *req, PropFind *pf, GHashTable *path_resp, xmlNsPtr ns)
{
  PathHandler *handler = phodav_request_get_handler (req);
  const gchar *path = phodav_request_get_path (req);
  GCancellable *cancellable = phodav_request_get_cancellable (req);
  g_autoptr(GError) err = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GFile) file = NULL;
  GList *stat = NULL;
  gint status = SOUP_STATUS_OK;

  file = phodav_path_resolve (handler, path, PHODAV_PATH_EXISTING, &status);
  if (!file)
    return status;

  info = g_file_query_info (file, FILE_QUERY,
                            G_FILE_QUERY_INFO_NONE, cancellable, &err);
  if (err)
    {
      if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_debug ("PROPFIND: query of %s failed: %s", path, err->message);
      return SOUP_STATUS_NOT_FOUND;
    }

  stat = propfind_populate (handler, file, path, pf, info, ns, cancellable);
  g_hash_table_insert (path_resp, g_strdup (path),
                       response_new (stat, 0));

  return status;
}

static gint
propfind_query_one (PhodavRequest *req, PropFind *pf, GHashTable *path_resp, xmlNsPtr ns)
{
  PathHandler *handler = phodav_request_get_handler (req);
  const gchar *path = phodav_request_get_path (req);
  GCancellable *cancellable = phodav_request_get_cancellable (req);
  g_autoptr(GError) err = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFileEnumerator) e = NULL;
  gint status;

  status = propfind_query_zero (req, pf, path_resp, ns);
  if (status != SOUP_STATUS_OK)
    return status;

  file = phodav_path_resolve (handler, path, PHODAV_PATH_EXISTING, &status);
  if (!file)
    return status;

  e = g_file_enumerate_children (file, FILE_QUERY,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, &err);
  if (!e)
    goto end;

  while (1)
    {
      GList *stat;
      g_autoptr(GFileInfo) info = g_file_enumerator_next_file (e, cancellable, &err);
      if (!info)
        break;

      g_autofree gchar *child_path = g_build_path ("/", path, g_file_info_get_name (info), NULL);
      g_autoptr(GFile) child = NULL;
      gint child_status;

      if ((g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK))
        continue;

      child = phodav_path_resolve (handler, child_path, PHODAV_PATH_EXISTING,
                                   &child_status);
      if (!child)
        continue;

      /* the properties of a member are queried on the member, not
       * on the collection it was listed from */
      stat = propfind_populate (handler, child, child_path, pf, info, ns, cancellable);
      /* the multistatus keys are the decoded resource paths, the
       * hrefs are escaped when the response is built */
      g_hash_table_insert (path_resp, g_steal_pointer (&child_path),
                           response_new (stat, 0));
    }

  g_file_enumerator_close (e, cancellable, NULL);

end:
  if (err && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
    g_debug ("PROPFIND: listing of %s failed: %s", path, err->message);

  return status;
}

static gboolean
parse_prop (xmlNodePtr node, GHashTable *props)
{
  gboolean found = FALSE;

  for (node = node->children; node; node = node->next)
    {
      if (!xml_node_is_element (node))
        continue;

      // only interested in ns&name
      g_hash_table_add (props, node);
      found = TRUE;
    }

  return found;
}

static PropFind *
parse_propfind (xmlNodePtr xml)
{
  PropFind *pf = propfind_new ();
  xmlNodePtr node;

  for (node = xml->children; node; node = node->next)
    {
      if (!xml_node_is_element (node))
        continue;

      if (xml_node_has_name (node, "allprop"))
        {
          pf->type = PROPFIND_ALLPROP;
          goto end;
        }
      else if (xml_node_has_name (node, "propname"))
        {
          pf->type = PROPFIND_PROPNAME;
          goto end;
        }
      else if (xml_node_has_name (node, "prop"))
        {
          pf->type = PROPFIND_PROP;
          /* a prop element without any property is a malformed
           * request (RFC 4918 section 9.1), it would produce an
           * empty multistatus response otherwise */
          if (!parse_prop (node, pf->props))
            g_clear_pointer (&pf, propfind_free);
          goto end;
        }
    }

  /* a body with no allprop, propname or prop element is malformed,
   * like one with an empty prop: the caller rejects it with 400 */
  g_debug ("PROPFIND: the request names no allprop, propname or prop element");
  g_clear_pointer (&pf, propfind_free);

end:
  return pf;
}

gint
phodav_method_propfind (PhodavRequest *req, GError **err)
{
  g_autoptr(PropFind) pf = NULL;
  DepthType depth;
  g_autoptr(GHashTable) mstatus = NULL; // path -> statlist
  g_auto(DavDoc) doc = {
    0,
  };
  gint status = SOUP_STATUS_NOT_FOUND;
  xmlNsPtr ns = NULL;
  const gchar *path = phodav_request_get_path (req);
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  SoupMessageBody *request_body = phodav_request_get_request_body (req);

  if (!depth_from_string (soup_message_headers_get_one (request_headers, "Depth"), &depth))
    {
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }

  if (depth == DEPTH_INFINITY)
    {
      /* RFC 4918 section 9.1: a server may refuse to walk the whole
       * tree of a depth-infinity PROPFIND */
      g_debug ("PROPFIND: refusing the depth-infinity request on %s", path);
      status = SOUP_STATUS_FORBIDDEN;
      goto end;
    }

  if (!request_body || !request_body->length)
    {
      /* Win kludge: http://code.google.com/p/sabredav/wiki/Windows */
      pf = propfind_new ();
      pf->type = PROPFIND_ALLPROP;
    }
  else
    {
      if (!davdoc_parse (&doc, request_body, "propfind"))
        {
          status = SOUP_STATUS_BAD_REQUEST;
          goto end;
        }

      pf = parse_propfind (doc.root);
      if (!pf)
        {
          status = SOUP_STATUS_BAD_REQUEST;
          goto end;
        }
    }

  ns = xmlNewNs (NULL, BAD_CAST "DAV:", BAD_CAST "D");
  mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) response_free);
  /* parse_propfind() only returns a PropFind with one of the three
   * request types: only the depth decides how it is answered, and the
   * depth-infinity request was refused above */
  phodav_server_read_lock (handler_get_server (phodav_request_get_handler (req)));

  if (depth == DEPTH_ZERO)
    status = propfind_query_zero (req, pf, mstatus, ns);
  else
    status = propfind_query_one (req, pf, mstatus, ns);

  phodav_server_read_unlock (handler_get_server (phodav_request_get_handler (req)));

  if (status != SOUP_STATUS_OK)
    goto end;

  status = set_response_multistatus (req, mstatus, ns);
  ns = NULL;

end:
  if (ns)
    xmlFreeNs (ns);
  return status;
}
