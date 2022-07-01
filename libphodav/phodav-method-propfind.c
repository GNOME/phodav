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

#include "phodav-priv.h"

#include "phodav-utils.h"
#include "phodav-multistatus.h"
#include "phodav-lock.h"
#include "phodav-path.h"

typedef struct _PropFind
{
  PropFindType type;
  GHashTable  *props;
} PropFind;

static PropFind*
propfind_new (void)
{
  PropFind *pf = g_slice_new0 (PropFind);

  // TODO: a better hash for Node (name, ns)
  pf->props = g_hash_table_new (g_direct_hash, g_direct_equal);
  return pf;
}

static void
propfind_free (PropFind *pf)
{
  if (!pf)
    return;

  g_hash_table_unref (pf->props);
  g_slice_free (PropFind, pf);
}

#define PROP_SET_STATUS(Node, Status) G_STMT_START {    \
    (Node)->_private = GINT_TO_POINTER (Status);        \
  } G_STMT_END

static xmlNodePtr
prop_resourcetype (PathHandler *handler, PropFind *pf,
                   const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "resourcetype");

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    xmlNewChild (node, ns, BAD_CAST "collection", NULL);
  else if (g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR)
  {
    g_warn_if_reached ();
    status = SOUP_STATUS_NOT_FOUND;
  }

 end:
  PROP_SET_STATUS (node, status);
  return node;
}

static xmlNodePtr
prop_supportedlock (PathHandler *handler, PropFind *pf,
                    const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "supportedlock");

  if (pf->type == PROPFIND_PROPNAME)
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
  PROP_SET_STATUS (node, SOUP_STATUS_OK);
  return node;
}

static gboolean
lockdiscovery_cb (const gchar *key, Path *path, gpointer data)
{
  xmlNodePtr node = data;
  GList *l;

  g_return_val_if_fail (key, FALSE);
  g_return_val_if_fail (path, FALSE);

  for (l = path->locks; l != NULL; l = l->next)
    xmlAddChild (node, dav_lock_get_activelock_node (l->data, NULL));

  return TRUE;
}

static xmlNodePtr
prop_lockdiscovery (PathHandler *handler, PropFind *pf,
                    const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  PhodavServer *server = handler_get_server (handler);
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "lockdiscovery");

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  server_foreach_parent_path (server, path, lockdiscovery_cb, node);

end:
  PROP_SET_STATUS (node, SOUP_STATUS_OK);
  return node;
}

typedef enum {
        NODE_DATE_FORMAT_HTTP,
        NODE_DATE_FORMAT_ISO8601
} NodeDateFormat;

static void
node_add_time (xmlNodePtr node, guint64 time, NodeDateFormat format)
{
  GDateTime *date;
  gchar *text;

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
  g_free (text);
  g_date_time_unref (date);
}

static xmlNodePtr
prop_creationdate (PathHandler *handler, PropFind *pf,
                   const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "creationdate");
  guint64 time;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  time = g_file_info_get_attribute_uint64 (info,
                                           G_FILE_ATTRIBUTE_TIME_CREATED);

  /* windows seems to want this, even apache returns modified time */
  if (time == 0)
    time = g_file_info_get_attribute_uint64 (info,
                                             G_FILE_ATTRIBUTE_TIME_MODIFIED);

  if (time == 0)
    status = SOUP_STATUS_NOT_FOUND;
  else
    node_add_time (node, time, NODE_DATE_FORMAT_HTTP);

end:
  PROP_SET_STATUS (node, status);
  return node;
}

static xmlNodePtr
prop_getlastmodified (PathHandler *handler, PropFind *pf,
                      const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "getlastmodified");
  guint64 time;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  time = g_file_info_get_attribute_uint64 (info,
                                           G_FILE_ATTRIBUTE_TIME_MODIFIED);

  if (time == 0)
    status = SOUP_STATUS_NOT_FOUND;
  else
    node_add_time (node, time, NODE_DATE_FORMAT_ISO8601);

end:
  PROP_SET_STATUS (node, status);
  return node;
}

static xmlNodePtr
prop_getcontentlength (PathHandler *handler, PropFind *pf,
                       const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "getcontentlength");
  guint64 size;
  gchar *text;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  size = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
  text = g_strdup_printf ("%" G_GUINT64_FORMAT, size);
  xmlAddChild (node, xmlNewText (BAD_CAST text));
  g_free (text);

end:
  PROP_SET_STATUS (node, status);
  return node;
}

static xmlNodePtr
prop_getcontenttype (PathHandler *handler, PropFind *pf,
                     const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "getcontenttype");
  const gchar *type;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  type = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
  if (type == NULL)
    status = SOUP_STATUS_NOT_FOUND;
  else
    xmlAddChild (node, xmlNewText (BAD_CAST type));

end:
  PROP_SET_STATUS (node, status);
  return node;
}

static xmlNodePtr
prop_displayname (PathHandler *handler, PropFind *pf,
                  const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "displayname");
  const gchar *name;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  name = g_file_info_get_display_name (info);
  if (name == NULL)
    status = SOUP_STATUS_NOT_FOUND;
  else
    xmlAddChild (node, xmlNewText (BAD_CAST name));

end:
  PROP_SET_STATUS (node, status);
  return node;
}

static xmlNodePtr
prop_getetag (PathHandler *handler, PropFind *pf,
              const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "getetag");
  const gchar *etag;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  etag = g_file_info_get_etag (info);
  if (etag)
    {
      gchar *tmp = g_strdup_printf ("\"%s\"", etag);
      xmlAddChild (node, xmlNewText (BAD_CAST tmp));
      g_free (tmp);
    }
  else
    {
      status = SOUP_STATUS_NOT_FOUND;
    }

end:
  PROP_SET_STATUS (node, status);
  return node;
}

static xmlNodePtr
prop_executable (PathHandler *handler, PropFind *pf,
                 const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (NULL, BAD_CAST "executable");
  gboolean exec;

  xmlNewNs (node, BAD_CAST "http://apache.org/dav/props/", NULL);

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  exec = g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE);
  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    exec = FALSE;

  xmlAddChild (node, xmlNewText (exec ?  BAD_CAST "T" : BAD_CAST "F"));

end:
  PROP_SET_STATUS (node, status);
  return node;
}

static xmlNodePtr
prop_quota_available (PathHandler *handler, PropFind *pf,
                      const gchar *path, GFileInfo *_info, xmlNsPtr ns)
{
  GFile *file;
  GFileInfo *info = NULL;
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "quota-available-bytes");
  GCancellable *cancellable = handler_get_cancellable(handler);
  GError *error = NULL;
  gchar *tmp = NULL;
  guint64 size;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  info = g_file_query_filesystem_info (file, "filesystem::*",
                                       cancellable, &error);
  g_object_unref (file);

  if (error)
    {
      g_warning ("Filesystem info error: %s", error->message);
      status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
      goto end;
    }

  size = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);

  tmp = g_strdup_printf ("%" G_GUINT64_FORMAT, size);
  xmlAddChild (node, xmlNewText (BAD_CAST tmp));

end:
  if (info)
    g_object_unref (info);

  g_free (tmp);
  PROP_SET_STATUS (node, status);
  return node;
}

static xmlNodePtr
prop_quota_used (PathHandler *handler, PropFind *pf,
                 const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  GFile *file = NULL;
  gint status = SOUP_STATUS_OK;
  GCancellable *cancellable = handler_get_cancellable(handler);
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "quota-used-bytes");
  guint64 disk_usage = 0;
  GError *error = NULL;
  gchar *tmp = NULL;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  if (!g_file_measure_disk_usage (file,
                                  G_FILE_MEASURE_NONE,
                                  cancellable,
                                  NULL, NULL,
                                  &disk_usage, NULL, NULL,
                                  &error))
    {
      g_warning ("Filesystem info error: %s", error->message);
      status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
      goto end;
    }

  tmp = g_strdup_printf ("%" G_GUINT64_FORMAT, disk_usage);
  xmlAddChild (node, xmlNewText (BAD_CAST tmp));

end:
  g_clear_object (&file);
  g_free (tmp);
  PROP_SET_STATUS (node, status);
  return node;
}

static gint
node_compare_int (xmlNodePtr a,
                  xmlNodePtr b)
{
  return GPOINTER_TO_INT (a->_private) - GPOINTER_TO_INT (b->_private);
}

static void
prop_add (GList **stat, xmlNodePtr node)
{
  *stat = g_list_insert_sorted (*stat, node, (GCompareFunc) node_compare_int);
}

#define PROP(Name, Info) { G_STRINGIFY (Name), G_PASTE (prop_, Name), Info }
static const struct _PropList
{
  const gchar *name;
  xmlNodePtr (*func) (PathHandler *, PropFind *, const gchar *, GFileInfo *, xmlNsPtr);
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
  { "quota-available-bytes", prop_quota_available, },
  { "quota-used-bytes", prop_quota_used, FALSE, TRUE, }
};

static xmlNodePtr
prop_xattr (gchar *xattr)
{
  xmlNodePtr node;
  gchar *ns = xattr + 7;
  gchar *name = g_utf8_strchr (ns, -1, '#');

  if (name)
    {
      *name = '\0';
      name = name + 1;
    }
  else
    {
      name = ns;
      ns = NULL;
    }

  node = xmlNewNode (NULL, BAD_CAST name);
  if (ns)
    xmlNewNs (node, BAD_CAST ns, NULL);

  PROP_SET_STATUS (node, SOUP_STATUS_OK);
  return node;
}

#define FILE_QUERY "standard::*,time::*,access::*,etag::*,xattr::*"
static GList*
propfind_populate (PathHandler *handler, const gchar *path,
                   PropFind *pf, GFileInfo *info,
                   xmlNsPtr ns)
{
  GHashTableIter iter;
  xmlNodePtr node;
  GList *stat = NULL;
  int i;

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
          prop_add (&stat, prop_list[i].func (handler, pf, path, info, ns));
        }

      if (info)
        {
          gchar **attrs = g_file_info_list_attributes (info, "xattr");

          for (i = 0; attrs[i]; i++)
            {
              node = prop_xattr(attrs[i]);
              prop_add (&stat, node);
            }

          g_strfreev (attrs);
        }

      goto end;
    }

  g_hash_table_iter_init (&iter, pf->props);
  while (g_hash_table_iter_next (&iter, (gpointer *) &node, NULL))
    {
      for (i = 0; i < G_N_ELEMENTS (prop_list); i++)
        {
          if (xml_node_has_name (node, prop_list[i].name)) {
            node = prop_list[i].func (handler, pf, path, info, ns);
            break;
          }
        }

      if (i == G_N_ELEMENTS (prop_list))
        {
          gchar *xattr = xml_node_get_xattr_name (node, "xattr::");
          node = xmlCopyNode (node, 2);
          const gchar *val = NULL;

          if (xattr)
            {
              val = g_file_info_get_attribute_string (info, xattr);
              g_free (xattr);
            }

          if (val)
            {
              xmlAddChild (node, xmlNewText (BAD_CAST val));
              PROP_SET_STATUS (node, SOUP_STATUS_OK);
            }
          else
            {
              xml_node_debug (node);
              PROP_SET_STATUS (node, SOUP_STATUS_NOT_FOUND);
            }
        }

      prop_add (&stat, node);
    }

end:
  return stat;
}

static gint
propfind_query_zero (PathHandler *handler, PropFind *pf,
                     const gchar *path, GHashTable *path_resp,
                     xmlNsPtr     ns)
{
  GCancellable *cancellable = handler_get_cancellable(handler);
  GError *err = NULL;
  GFileInfo *info = NULL;
  GFile *file;
  GList *stat = NULL;
  gint status = SOUP_STATUS_OK;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  info = g_file_query_info (file, FILE_QUERY,
                            G_FILE_QUERY_INFO_NONE, cancellable, &err);
  g_object_unref (file);
  if (err)
    {
      if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_warning ("queryinfo: %s", err->message);
      g_clear_error (&err);
      return SOUP_STATUS_NOT_FOUND;
    }

  stat = propfind_populate (handler, path, pf, info, ns);
  g_hash_table_insert (path_resp, g_strdup (path),
                       response_new (stat, 0));
  g_clear_object (&info);

  return status;
}

static gint
propfind_query_one (PathHandler *handler, PropFind *pf,
                    const gchar *path, GHashTable *path_resp,
                    xmlNsPtr     ns)
{
  GCancellable *cancellable = handler_get_cancellable(handler);
  GError *err = NULL;
  GFile *file;
  GFileEnumerator *e;
  gint status;

  status = propfind_query_zero (handler, pf, path, path_resp, ns);
  if (status != SOUP_STATUS_OK)
    return status;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  e = g_file_enumerate_children (file, FILE_QUERY, G_FILE_QUERY_INFO_NONE,
                                 cancellable, &err);
  g_object_unref (file);
  if (!e)
    goto end;

  while (1)
    {
      GList *stat;
      GFileInfo *info = g_file_enumerator_next_file (e, cancellable, &err);
      if (!info)
        break;

      gchar *escape = g_markup_escape_text (g_file_info_get_name (info), -1);
      stat = propfind_populate (handler, path, pf, info, ns);
      g_hash_table_insert (path_resp, g_build_path ("/", path, escape, NULL),
                           response_new (stat, 0));
      g_free (escape);
      g_object_unref (info);
    }

  g_file_enumerator_close (e, cancellable, NULL);
  g_clear_object (&e);

end:
  if (err)
    {
      if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
        g_warning ("query: %s", err->message);
      g_clear_error (&err);
    }

  return status;
}

static gboolean
parse_prop (xmlNodePtr node, GHashTable *props)
{
  for (node = node->children; node; node = node->next)
    {
      if (!xml_node_is_element (node))
        continue;

      // only interested in ns&name
      g_hash_table_add (props, node);
    }

  return TRUE;
}

static PropFind*
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
          parse_prop (node, pf->props);
          goto end;
        }
    }

  g_warn_if_reached ();
  g_clear_pointer (&pf, propfind_free);

end:
  return pf;
}

gint
phodav_method_propfind (PathHandler *handler, SoupServerMessage *msg,
                        const char *path, GError **err)
{
  PropFind *pf = NULL;
  DepthType depth;
  GHashTable *mstatus = NULL;   // path -> statlist
  DavDoc doc = {0, };
  gint status = SOUP_STATUS_NOT_FOUND;
  xmlNsPtr ns = NULL;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  SoupMessageBody *request_body = soup_server_message_get_request_body (msg);

  depth = depth_from_string (soup_message_headers_get_one (request_headers, "Depth"));
  if (!request_body || !request_body->length)
    {
      /* Win kludge: http://code.google.com/p/sabredav/wiki/Windows */
      pf = propfind_new ();
      pf->type = PROPFIND_ALLPROP;
    }
  else
    {
      if (!davdoc_parse (&doc, msg, request_body, "propfind"))
        {
          status = SOUP_STATUS_BAD_REQUEST;
          goto end;
        }

      pf = parse_propfind (doc.root);
      if (!pf)
        goto end;
    }

  ns = xmlNewNs (NULL, BAD_CAST "DAV:", BAD_CAST "D");
  mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) response_free);
  if (pf->type == PROPFIND_PROP ||
      pf->type == PROPFIND_ALLPROP ||
      pf->type == PROPFIND_PROPNAME)
    {
      if (depth == DEPTH_ZERO)
        status = propfind_query_zero (handler, pf, path, mstatus, ns);
      else if (depth == DEPTH_ONE)
        status = propfind_query_one (handler, pf, path, mstatus, ns);
      else
        {
          status = SOUP_STATUS_FORBIDDEN;
          g_warn_if_reached ();
        }
    }
  else
    g_warn_if_reached ();

  if (status != SOUP_STATUS_OK)
    goto end;

  status = set_response_multistatus (msg, mstatus);

end:
  davdoc_free (&doc);
  propfind_free (pf);
  if (mstatus)
    g_hash_table_unref (mstatus);
  if (ns)
    xmlFreeNs(ns);
  return status;
}
