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

#include "config.h"

#include <glib/gi18n.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include <sys/types.h>
#ifdef HAVE_ATTR_XATTR_H
#include <attr/xattr.h>
#endif

#include "guuid.h"
#include "phodav-server.h"

typedef struct _PathHandler PathHandler;

struct _PhodavServer
{
  GObject       parent;
  GMainContext *context;
  GMainLoop    *loop;
  GThread      *thread;
  SoupServer   *server;
  GCancellable *cancellable;
  gchar        *root;
  PathHandler  *root_handler; /* weak ref */
  guint         port;
  GHashTable   *paths;
};

struct _PhodavServerClass
{
  GObjectClass parent_class;
};

typedef struct _DAVLock DAVLock;
typedef struct _Path    Path;

G_DEFINE_TYPE (PhodavServer, phodav_server, G_TYPE_OBJECT)

/* Properties */
enum {
  PROP_0,
  PROP_PORT,
  PROP_ROOT,
  PROP_SERVER,
};

static void server_callback (SoupServer        *server,
                             SoupMessage       *msg,
                             const char        *path,
                             GHashTable        *query,
                             SoupClientContext *context,
                             gpointer           user_data);

static void request_started (SoupServer        *server,
                             SoupMessage       *message,
                             SoupClientContext *client,
                             gpointer           user_data);

static void dav_lock_free (DAVLock *lock);

struct _Path
{
  gchar  *path;
  GList  *locks;
  guint32 refs;
};

static Path *
path_ref (Path *path)
{
  path->refs++;

  return path;
}

static void
path_unref (Path *path)
{
  path->refs--;

  if (path->refs == 0)
    {
      g_list_free_full (path->locks, (GDestroyNotify) dav_lock_free);
      g_free (path->path);
      g_slice_free (Path, path);
    }
}

static void
path_remove_lock (Path *path, DAVLock *lock)
{
  g_return_if_fail (path);
  g_return_if_fail (lock);

  path->locks =  g_list_remove (path->locks, lock);
}

static void
path_add_lock (Path *path, DAVLock *lock)
{
  g_return_if_fail (path);
  g_return_if_fail (lock);

  path->locks = g_list_append (path->locks, lock);
}

static void
remove_trailing (gchar *str, gchar c)
{
  gsize len = strlen (str);

  while (len > 0 && str[len - 1] == c)
    len--;

  str[len] = '\0';
}

static Path *
get_path (PhodavServer *self, const gchar *_path)
{
  Path *p;
  gchar *path = g_strdup (_path);

  remove_trailing (path, '/');
  p = g_hash_table_lookup (self->paths, path);
  if (!p)
    {
      p = g_slice_new0 (Path);
      p->path = path;
      g_hash_table_insert (self->paths, p->path, path_ref (p));
    }

  return p;
}

typedef enum _LockScopeType {
  LOCK_SCOPE_NONE,
  LOCK_SCOPE_EXCLUSIVE,
  LOCK_SCOPE_SHARED,
} LockScopeType;

typedef enum _LockType {
  LOCK_NONE,
  LOCK_WRITE,
} LockType;

typedef enum _DepthType {
  DEPTH_ZERO,
  DEPTH_ONE,
  DEPTH_INFINITY
} DepthType;

struct _DAVLock
{
  Path         *path;
  gchar         token[45];
  LockScopeType scope;
  LockType      type;
  DepthType     depth;
  xmlNodePtr    owner;
  guint64       timeout;
};

static void
dav_lock_refresh_timeout (DAVLock *lock, guint timeout)
{
  if (timeout)
    lock->timeout = g_get_monotonic_time () / G_USEC_PER_SEC + timeout;
  else
    lock->timeout = 0;
}

static DAVLock *
dav_lock_new (Path *path, const gchar *token,
              LockScopeType scope, LockType type,
              DepthType depth, const xmlNodePtr owner,
              guint timeout)
{
  DAVLock *lock;

  g_return_val_if_fail (token, NULL);
  g_return_val_if_fail (strlen (token) == sizeof (lock->token), NULL);

  lock = g_slice_new0 (DAVLock);
  lock->path = path_ref (path);
  memcpy (lock->token, token, sizeof (lock->token));
  lock->scope = scope;
  lock->type = type;
  lock->depth = depth;
  if (owner)
    lock->owner = xmlCopyNode (owner, 1);

  dav_lock_refresh_timeout (lock, timeout);

  return lock;
}

static void
dav_lock_free (DAVLock *lock)
{
  g_return_if_fail (lock);

  path_remove_lock (lock->path, lock);
  path_unref (lock->path);

  if (lock->owner)
    xmlFreeNode (lock->owner);

  g_slice_free (DAVLock, lock);
}

struct _PathHandler
{
  PhodavServer *self;
  GFile       *file;
};

static PathHandler*
path_handler_new (PhodavServer *self, GFile *file)
{
  PathHandler *h = g_slice_new0 (PathHandler);

  h->self = self;
  h->file = file;
  return h;
}

static void
path_handler_free (PathHandler *h)
{
  g_object_unref (h->file);
  g_slice_free (PathHandler, h);
}

typedef struct _Response
{
  GList *props;
  gint   status;
} Response;

static Response*
response_new (GList *props, gint status)
{
  Response *r;

  g_return_val_if_fail (props != NULL || status > 0, NULL);

  r = g_slice_new0 (Response);
  r->status = status;
  r->props = props;

  return r;
}

static void
response_free (Response *h)
{
  g_list_free_full (h->props, (GDestroyNotify) xmlFreeNode);
  g_slice_free (Response, h);
}

static void
phodav_server_init (PhodavServer *self)
{
  self->cancellable = g_cancellable_new ();
  self->context = g_main_context_new ();
  self->loop = g_main_loop_new (self->context, FALSE);

  self->paths = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       NULL, (GDestroyNotify) path_unref);
}

static void
update_root_handler (PhodavServer *self)
{
  PathHandler *handler;

  if (!self->root || !self->server)
    return;

  handler = path_handler_new (self, g_file_new_for_path (self->root));

  soup_server_add_handler (self->server, NULL,
                           server_callback,
                           handler,
                           (GDestroyNotify) path_handler_free);

  self->root_handler = handler;
}

static void
phodav_server_constructed (GObject *gobject)
{
  PhodavServer *self = PHODAV_SERVER (gobject);

  self->server = soup_server_new (SOUP_SERVER_PORT, self->port,
                                  SOUP_SERVER_SERVER_HEADER, "PhodavServer ",
                                  SOUP_SERVER_ASYNC_CONTEXT, self->context,
                                  NULL);

  update_root_handler (self);

  g_signal_connect (self->server, "request-started", G_CALLBACK (request_started), self);

  /* Chain up to the parent class */
  if (G_OBJECT_CLASS (phodav_server_parent_class)->constructed)
    G_OBJECT_CLASS (phodav_server_parent_class)->constructed (gobject);
}

static void
phodav_server_dispose (GObject *gobject)
{
  PhodavServer *self = PHODAV_SERVER (gobject);

  g_clear_pointer (&self->root, g_free);
  g_clear_pointer (&self->context, g_main_context_unref);
  g_clear_pointer (&self->thread, g_thread_unref);
  g_clear_pointer (&self->paths, g_hash_table_unref);

  /* Chain up to the parent class */
  if (G_OBJECT_CLASS (phodav_server_parent_class)->dispose)
    G_OBJECT_CLASS (phodav_server_parent_class)->dispose (gobject);
}

static void
phodav_server_get_property (GObject    *gobject,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  PhodavServer *self = PHODAV_SERVER (gobject);

  switch (prop_id)
    {
    case PROP_PORT:
      g_value_set_uint (value, phodav_server_get_port (self));
      break;

    case PROP_ROOT:
      g_value_set_string (value, self->root);
      break;

    case PROP_SERVER:
      g_value_set_object (value, self->server);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
phodav_server_set_property (GObject      *gobject,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  PhodavServer *self = PHODAV_SERVER (gobject);

  switch (prop_id)
    {
    case PROP_PORT:
      self->port = g_value_get_uint (value);
      break;

    case PROP_ROOT:
      g_free (self->root);
      self->root = g_value_dup_string (value);
      update_root_handler (self);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
phodav_server_class_init (PhodavServerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose      = phodav_server_dispose;
  gobject_class->constructed  = phodav_server_constructed;
  gobject_class->get_property = phodav_server_get_property;
  gobject_class->set_property = phodav_server_set_property;

  g_object_class_install_property
    (gobject_class, PROP_PORT,
    g_param_spec_uint ("port",
                       "Port",
                       "Port",
                       0, G_MAXINT16, 0,
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_READWRITE |
                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property
    (gobject_class, PROP_ROOT,
    g_param_spec_string ("root",
                         "Root path",
                         "Root path",
                         NULL,
                         G_PARAM_CONSTRUCT |
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property
    (gobject_class, PROP_SERVER,
     g_param_spec_object ("server",
                          "Server",
                          "Server",
                          SOUP_TYPE_SERVER,
                          G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));
}

static void
node_debug (xmlNodePtr node)
{
  g_debug ("%s ns:%s", node->name, node->ns ? (gchar *) node->ns->href : "");
}

static gboolean
node_has_ns (xmlNodePtr node, const char *ns_href)
{
  return node->ns && node->ns->href &&
         !g_strcmp0 ((gchar *) node->ns->href, ns_href);

}

static gboolean
node_has_name_ns (xmlNodePtr node, const char *name, const char *ns_href)
{
  gboolean has_name;
  gboolean has_ns;

  g_return_val_if_fail (node != NULL, FALSE);

  has_name = has_ns = TRUE;

  if (name)
    has_name = !g_strcmp0 ((gchar *) node->name, name);

  if (ns_href)
    has_ns = node_has_ns (node, ns_href);

  return has_name && has_ns;
}

static gboolean
node_has_name (xmlNodePtr node, const char *name)
{
  g_return_val_if_fail (node != NULL, FALSE);

  return node_has_name_ns (node, name, "DAV:");
}

static gboolean
node_is_element (xmlNodePtr node)
{
  return node->type == XML_ELEMENT_NODE && node->name != NULL;
}

static LockScopeType
parse_lockscope (xmlNodePtr rt)
{
  xmlNodePtr node;

  for (node = rt->children; node; node = node->next)
    if (node_is_element (node))
      break;

  if (node == NULL)
    return LOCK_SCOPE_NONE;

  if (!g_strcmp0 ((char *) node->name, "exclusive"))
    return LOCK_SCOPE_EXCLUSIVE;
  else if (!g_strcmp0 ((char *) node->name, "shared"))
    return LOCK_SCOPE_SHARED;
  else
    return LOCK_SCOPE_NONE;
}

static const gchar *
lockscope_to_string (LockScopeType type)
{
  if (type == LOCK_SCOPE_EXCLUSIVE)
    return "exclusive";
  else if (type == LOCK_SCOPE_SHARED)
    return "shared";

  g_return_val_if_reached (NULL);
}

static LockType
parse_locktype (xmlNodePtr rt)
{
  xmlNodePtr node;

  for (node = rt->children; node; node = node->next)
    if (node_is_element (node))
      break;

  if (node == NULL)
    return LOCK_NONE;

  if (!g_strcmp0 ((char *) node->name, "write"))
    return LOCK_WRITE;
  else
    return LOCK_NONE;
}

static const gchar *
locktype_to_string (LockType type)
{
  if (type == LOCK_WRITE)
    return "write";

  g_return_val_if_reached (NULL);
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


static int
compare_strings (gconstpointer a, gconstpointer b)
{
  const char **sa = (const char * *) a;
  const char **sb = (const char * *) b;

  return g_strcmp0 (*sa, *sb);
}

static GString *
get_directory_listing (GFile *file, GCancellable *cancellable, GError **err)
{
  GString *listing;
  GPtrArray *entries;
  GFileEnumerator *e;
  gchar *escaped;
  gchar *name;
  gint i;

  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE,
                                 cancellable, err);
  g_return_val_if_fail (e != NULL, NULL);

  entries = g_ptr_array_new ();
  while (1)
    {
      GFileInfo *info = g_file_enumerator_next_file (e, cancellable, err);
      gboolean dir;

      if (!info)
        break;

      dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

      escaped = g_markup_printf_escaped ("%s%s",
                                         g_file_info_get_name (info), dir ? "/" : "");
      g_ptr_array_add (entries, escaped);
      g_object_unref (info);
    }
  g_file_enumerator_close (e, cancellable, NULL);
  g_clear_object (&e);

  g_ptr_array_sort (entries, compare_strings);

  listing = g_string_new ("<html>\r\n");
  name = g_file_get_basename (file);
  escaped = g_markup_escape_text (name, -1);
  g_free (name);
  g_string_append_printf (listing, "<head><title>Index of %s</title></head>\r\n", escaped);
  g_string_append_printf (listing, "<body><h1>Index of %s</h1>\r\n<p>\r\n", escaped);
  g_free (escaped);
  for (i = 0; i < entries->len; i++)
    {
      g_string_append_printf (listing, "<a href=\"%s\">%s</a><br/>\r\n",
                              (gchar *) entries->pdata[i],
                              (gchar *) entries->pdata[i]);
      g_free (entries->pdata[i]);
    }
  g_string_append (listing, "</p></body>\r\n</html>\r\n");

  g_ptr_array_free (entries, TRUE);
  return listing;
}

static gint
do_get_file (SoupMessage *msg, GFile *file,
             GCancellable *cancellable, GError **err)
{
  GError *error = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;
  GFileInfo *info;
  const gchar *etag;

  info = g_file_query_info (file, "standard::*,etag::*",
                            G_FILE_QUERY_INFO_NONE, cancellable, &error);
  if (!info)
    goto end;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      GString *listing;

      listing = get_directory_listing (file, cancellable, err);
      soup_message_set_response (msg, "text/html; charset=utf-8",
                                 SOUP_MEMORY_TAKE,
                                 listing->str, listing->len);
      g_string_free (listing, FALSE);
      status = SOUP_STATUS_OK;
      goto end;
    }

  etag = g_file_info_get_etag (info);
  g_warn_if_fail (etag != NULL);

  if (etag)
    {
      gchar *tmp = g_strdup_printf ("\"%s\"", etag);
      soup_message_headers_append (msg->response_headers, "ETag", tmp);
      g_free (tmp);
    }

  soup_message_headers_set_content_type (msg->response_headers,
                                         g_file_info_get_content_type (info), NULL);

  if (msg->method == SOUP_METHOD_GET)
    {
      GMappedFile *mapping;
      SoupBuffer *buffer;
      gchar *path = g_file_get_path (file);

      mapping = g_mapped_file_new (path, FALSE, NULL);
      g_free (path);
      if (!mapping)
        {
          status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
          goto end;
        }

      buffer = soup_buffer_new_with_owner (g_mapped_file_get_contents (mapping),
                                           g_mapped_file_get_length (mapping),
                                           mapping, (GDestroyNotify) g_mapped_file_unref);
      soup_message_body_append_buffer (msg->response_body, buffer);
      soup_buffer_free (buffer);
      status = SOUP_STATUS_OK;
    }
  else if (msg->method == SOUP_METHOD_HEAD)
    {
      gchar *length;

      /* We could just use the same code for both GET and
       * HEAD (soup-message-server-io.c will fix things up).
       * But we'll optimize and avoid the extra I/O.
       */
      length = g_strdup_printf ("%" G_GUINT64_FORMAT, g_file_info_get_size (info));
      soup_message_headers_append (msg->response_headers, "Content-Length", length);

      g_free (length);
      status = SOUP_STATUS_OK;
    }
  else
    g_warn_if_reached ();

end:
  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_debug ("getfile: %s", error->message);
          g_clear_error (&error);
        }
      else
        g_propagate_error (err, error);
    }

  g_clear_object (&info);
  return status;
}

typedef struct _DavDoc     DavDoc;
typedef struct _DDPropfind DDPropfind;

struct _DavDoc
{
  xmlDocPtr  doc;
  xmlNodePtr root;

  SoupURI   *target;
  char      *path;
};

struct _DDPropfind
{
  DavDoc    *dd;

  xmlNodePtr prop_node;
};

static gboolean
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

static void
davdoc_free (DavDoc *dd)
{
  if (dd->doc)
    xmlFreeDoc (dd->doc);
  g_free (dd->path);
}

typedef enum _PropFindType {
  PROPFIND_ALLPROP,
  PROPFIND_PROPNAME,
  PROPFIND_PROP
} PropFindType;

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

static gboolean
parse_prop (xmlNodePtr node, GHashTable *props)
{
  for (node = node->children; node; node = node->next)
    {
      if (!node_is_element (node))
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
      if (!node_is_element (node))
        continue;

      if (node_has_name (node, "allprop"))
        {
          pf->type = PROPFIND_ALLPROP;
          goto end;
        }
      else if (node_has_name (node, "propname"))
        {
          pf->type = PROPFIND_PROPNAME;
          goto end;
        }
      else if (node_has_name (node, "prop"))
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

static void
node_to_string (xmlNodePtr root, xmlChar **mem, int *size)
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

#define PROP_SET_STATUS(Node, Status) G_STMT_START {     \
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
  else if (!g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR)
    {
      g_warn_if_reached ();
      status = SOUP_STATUS_NOT_FOUND;
    }

end:
  PROP_SET_STATUS (node, status);
  return node;
}

static gchar*
status_to_string (gint status)
{
  return g_strdup_printf ("HTTP/1.1 %d %s",
                          status, soup_status_get_phrase (status));
}

static gint
node_compare_int (xmlNodePtr a,
                  xmlNodePtr b)
{
  return GPOINTER_TO_INT (a->_private) - GPOINTER_TO_INT (b->_private);
}

static void
node_add_time (xmlNodePtr node, guint64 time, SoupDateFormat format)
{
  SoupDate *date;
  gchar *text;

  g_warn_if_fail (time != 0);
  date = soup_date_new_from_time_t (time);
  text = soup_date_to_string (date, format);
  xmlAddChild (node, xmlNewText (BAD_CAST text));
  g_free (text);
  soup_date_free (date);
}

static DepthType
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

static guint
timeout_from_string (const gchar *timeout)
{
  if (!timeout ||
      !g_strcmp0 (timeout, "Infinite"))
    return 0;

  if (!g_ascii_strncasecmp (timeout, "Second-", 7))
    return g_ascii_strtoull (timeout + 7, NULL, 10);

  g_return_val_if_reached (0);
}

static const gchar *
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

static gchar *
node_get_xattr_name (xmlNodePtr node, const gchar *prefix)
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

static xmlNodePtr
get_activelock_node (const DAVLock *lock,
                     xmlNsPtr       ns)
{
  xmlNodePtr node, active;

  active = xmlNewNode (ns, BAD_CAST "activelock");

  node = xmlNewChild (active, ns, BAD_CAST "locktype", NULL);
  xmlNewChild (node, ns, BAD_CAST locktype_to_string (lock->type), NULL);
  node = xmlNewChild (active, ns, BAD_CAST "lockscope", NULL);
  xmlNewChild (node, ns, BAD_CAST lockscope_to_string (lock->scope), NULL);
  node = xmlNewChild (active, ns, BAD_CAST "depth", NULL);
  xmlAddChild (node, xmlNewText (BAD_CAST depth_to_string (lock->depth)));

  if (lock->owner)
    xmlAddChild (active, xmlCopyNode (lock->owner, 1));

  node = xmlNewChild (active, ns, BAD_CAST "locktoken", NULL);
  node = xmlNewChild (node, ns, BAD_CAST "href", NULL);
  xmlAddChild (node, xmlNewText (BAD_CAST lock->token));

  node = xmlNewChild (active, ns, BAD_CAST "lockroot", NULL);
  node = xmlNewChild (node, ns, BAD_CAST "href", NULL);
  xmlAddChild (node, xmlNewText (BAD_CAST lock->path->path));
  if (lock->timeout)
    {
      gchar *tmp = g_strdup_printf ("Second-%" G_GINT64_FORMAT, lock->timeout -
                                    g_get_monotonic_time () / G_USEC_PER_SEC);
      node = xmlNewChild (active, ns, BAD_CAST "timeout", NULL);
      xmlAddChild (node, xmlNewText (BAD_CAST tmp));
      g_free (tmp);
    }

  return active;
}

typedef gboolean (* PathCb) (const gchar *key,
                             Path        *path,
                             gpointer     data);

static gboolean
foreach_parent_path (PhodavServer *self, const gchar *path, PathCb cb, gpointer data)
{
  gchar **pathv, *partial = g_strdup ("/");
  gboolean ret = FALSE;
  gchar *key = NULL;
  Path *p;
  gint i;

  pathv = g_strsplit (path, "/", -1);

  for (i = 0; pathv[i]; i++)
    {
      if (!*pathv[i])
        continue;

      gchar *tmp = g_build_path ("/", partial, pathv[i], NULL);
      g_free (partial);
      partial = tmp;

      if (g_hash_table_lookup_extended (self->paths, partial,
                                        (gpointer *) &key, (gpointer *) &p))
        {
          if (!cb (key, p, data))
            goto end;
        }
    }

  ret = TRUE;

end:
  g_strfreev (pathv);
  g_free (partial);

  return ret;
}

typedef struct _PathGetLock
{
  const gchar *token;
  DAVLock     *lock;
} PathGetLock;

static gboolean
_path_get_lock (const gchar *key, Path *path, gpointer data)
{
  PathGetLock *d = data;
  GList *l;

  for (l = path->locks; l != NULL; l = l->next)
    {
      DAVLock *lock = l->data;

      if (!g_strcmp0 (lock->token, d->token))
        {
          d->lock = lock;
          return FALSE;
        }
    }

  return TRUE;
}

static DAVLock *
path_get_lock (PhodavServer *self, const gchar *path, const gchar *token)
{
  PathGetLock p = { .token = token };
  gboolean success = !foreach_parent_path (self, path,
                                           _path_get_lock, (gpointer) &p);

  if (!success)
    g_message ("Invalid lock token %s for %s", token, path);

  return p.lock;
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
    xmlAddChild (node, get_activelock_node (l->data, NULL));

  return TRUE;
}

static xmlNodePtr
prop_lockdiscovery (PathHandler *handler, PropFind *pf,
                    const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  PhodavServer *self = handler->self;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "lockdiscovery");

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  foreach_parent_path (self, path, lockdiscovery_cb, node);

end:
  PROP_SET_STATUS (node, SOUP_STATUS_OK);
  return node;
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
    node_add_time (node, time, SOUP_DATE_HTTP);

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
    node_add_time (node, time, SOUP_DATE_ISO8601);

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
  GFileInfo *info = NULL;
  gint status = SOUP_STATUS_OK;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "quota-available-bytes");
  PhodavServer *self = handler->self;
  GError *error = NULL;
  gchar *tmp = NULL;
  guint64 size;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  info = g_file_query_filesystem_info (handler->file, "filesystem::*",
                                       self->cancellable, &error);
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

#if GLIB_CHECK_VERSION (2, 38, 0)
static xmlNodePtr
prop_quota_used (PathHandler *handler, PropFind *pf,
                 const gchar *path, GFileInfo *info, xmlNsPtr ns)
{
  gint status = SOUP_STATUS_OK;
  PhodavServer *self = handler->self;
  xmlNodePtr node = xmlNewNode (ns, BAD_CAST "quota-used-bytes");
  guint64 disk_usage = 0;
  GError *error = NULL;
  gchar *tmp = NULL;

  if (pf->type == PROPFIND_PROPNAME)
    goto end;

  if (!g_file_measure_disk_usage (handler->file,
                                  G_FILE_MEASURE_NONE,
                                  self->cancellable,
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
  g_free (tmp);
  PROP_SET_STATUS (node, status);
  return node;
}
#endif

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
#if GLIB_CHECK_VERSION (2, 38, 0)
  { "quota-used-bytes", prop_quota_used, FALSE, TRUE, }
#endif
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
          if (node_has_name (node, prop_list[i].name)) {
            node = prop_list[i].func (handler, pf, path, info, ns);
            break;
          }
        }

      if (i == G_N_ELEMENTS (prop_list))
        {
          gchar *xattr = node_get_xattr_name (node, "xattr::");
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
              node_debug (node);
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
  PhodavServer *self = handler->self;
  GError *err = NULL;
  GFileInfo *info = NULL;
  GFile *file;
  GList *stat = NULL;
  gint status = SOUP_STATUS_OK;

  file = g_file_get_child (handler->file, path + 1);
  info = g_file_query_info (file, FILE_QUERY,
                            G_FILE_QUERY_INFO_NONE, self->cancellable, &err);
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
  PhodavServer *self = handler->self;
  GError *err = NULL;
  GFile *file;
  GFileEnumerator *e;
  gint status;

  status = propfind_query_zero (handler, pf, path, path_resp, ns);
  if (status != SOUP_STATUS_OK)
    return status;

  file = g_file_get_child (handler->file, path + 1);
  e = g_file_enumerate_children (file, FILE_QUERY, G_FILE_QUERY_INFO_NONE,
                                 self->cancellable, &err);
  g_object_unref (file);
  if (!e)
    goto end;

  while (1)
    {
      GList *stat;
      GFileInfo *info = g_file_enumerator_next_file (e, self->cancellable, &err);
      if (!info)
        break;

      gchar *escape = g_markup_escape_text (g_file_info_get_name (info), -1);
      stat = propfind_populate (handler, path, pf, info, ns);
      g_hash_table_insert (path_resp, g_build_path ("/", path, escape, NULL),
                           response_new (stat, 0));
      g_free (escape);
      g_object_unref (info);
    }

  g_file_enumerator_close (e, self->cancellable, NULL);
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

static gint
response_multistatus (SoupMessage *msg,
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

  node_to_string (root, &mem, &size);
  soup_message_set_response (msg, "application/xml",
                             SOUP_MEMORY_TAKE, (gchar *) mem, size);

  return SOUP_STATUS_MULTI_STATUS;
}

static gint
method_propfind (PathHandler *handler, SoupMessage *msg,
                 const char *path, GError **err)
{
  PropFind *pf = NULL;
  DepthType depth;
  GHashTable *mstatus = NULL;   // path -> statlist
  DavDoc doc = {0, };
  gint status = SOUP_STATUS_NOT_FOUND;
  xmlNsPtr ns = NULL;

  depth = depth_from_string (soup_message_headers_get_one (msg->request_headers, "Depth"));
  if (!msg->request_body || !msg->request_body->length)
    {
      /* Win kludge: http://code.google.com/p/sabredav/wiki/Windows */
      pf = propfind_new ();
      pf->type = PROPFIND_ALLPROP;
    }
  else
    {
      if (!davdoc_parse (&doc, msg, msg->request_body, "propfind"))
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

  status = response_multistatus (msg, mstatus);

end:
  davdoc_free (&doc);
  propfind_free (pf);
  if (mstatus)
    g_hash_table_unref (mstatus);
  if (ns)
    xmlFreeNs(ns);
  return status;
}

static gint
set_attr (PhodavServer *self, GFile *file, xmlNodePtr attrnode,
          GFileAttributeType type, gchar *mem)
{
  gchar *attrname;
  gint status = SOUP_STATUS_OK;
  GError *error = NULL;

  if (type == G_FILE_ATTRIBUTE_TYPE_INVALID)
    {
      attrname = node_get_xattr_name (attrnode, "user.");
      g_return_val_if_fail (attrname, SOUP_STATUS_BAD_REQUEST);

      /* https://bugzilla.gnome.org/show_bug.cgi?id=720506 */
      gchar *path = g_file_get_path (file);
#ifdef HAVE_ATTR_XATTR_H
      removexattr (path, attrname);
#else
      g_debug ("fixme");
#endif
      g_free (path);
    }
  else
    {
      attrname = node_get_xattr_name (attrnode, "xattr::");
      g_return_val_if_fail (attrname, SOUP_STATUS_BAD_REQUEST);

      g_file_set_attribute (file, attrname, type, mem,
                            G_FILE_QUERY_INFO_NONE, self->cancellable, &error);
    }

  g_free (attrname);

  if (error)
    {
      g_warning ("failed to set property: %s", error->message);
      g_clear_error (&error);
      status = SOUP_STATUS_NOT_FOUND;
    }

  return status;
}

static xmlBufferPtr
node_children_to_string (xmlNodePtr node)
{
  xmlBufferPtr buf = xmlBufferCreate ();

  for (node = node->children; node; node = node->next)
    xmlNodeDump (buf, node->doc, node, 0, 0);

  return buf;
}

static gint
prop_set (PhodavServer *self, SoupMessage *msg,
          GFile *file, xmlNodePtr parent, xmlNodePtr *attr,
          gboolean remove)
{
  xmlNodePtr node, attrnode;
  gint type = G_FILE_ATTRIBUTE_TYPE_INVALID;
  gint status;

  for (node = parent->children; node; node = node->next)
    {
      if (!node_is_element (node))
        continue;

      if (node_has_name (node, "prop"))
        {
          xmlBufferPtr buf = NULL;

          attrnode = node->children;
          if (!node_is_element (attrnode))
            continue;

          if (!remove)
            {
              *attr = xmlCopyNode (attrnode, 2);

              buf = node_children_to_string (attrnode);
              type = G_FILE_ATTRIBUTE_TYPE_STRING;
            }

          status = set_attr (self, file, attrnode, type, (gchar *) xmlBufferContent (buf));

          if (buf)
            xmlBufferFree (buf);

          return status;
        }
    }

  g_return_val_if_reached (SOUP_STATUS_BAD_REQUEST);
}

static gint
method_get (PathHandler *handler, SoupMessage *msg, const char *path, GError **err)
{
  GFile *file;
  PhodavServer *self = handler->self;
  gint status;

  file = g_file_get_child (handler->file, path + 1);
  status = do_get_file (msg, file, self->cancellable, err);
  g_object_unref (file);

  return status;
}

static gint
put_start (SoupMessage *msg, GFile *file,
           GFileOutputStream **output, GCancellable *cancellable,
           GError **err)
{
  GFileOutputStream *s = NULL;
  gchar *etag = NULL;
  gboolean created = TRUE;
  SoupMessageHeaders *headers = msg->request_headers;
  gint status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

  if (g_file_query_exists (file, cancellable))
    created = FALSE;

  if (soup_message_headers_get_list (headers, "If-Match"))
    g_warn_if_reached ();
  else if (soup_message_headers_get_list (headers, "If-None-Match"))
    g_warn_if_reached ();
  else if (soup_message_headers_get_list (headers, "Expect"))
    g_warn_if_reached ();

  s = g_file_replace (file, etag, FALSE, G_FILE_CREATE_PRIVATE, cancellable, err);
  if (!s)
    goto end;

  status = created ? SOUP_STATUS_CREATED : SOUP_STATUS_OK;

end:
  *output = s;
  return status;
}

typedef struct _LockSubmitted
{
  gchar *path;
  gchar *token;
} LockSubmitted;

static LockSubmitted *
lock_submitted_new (const gchar *path, const gchar *token)
{
  LockSubmitted *l;

  g_return_val_if_fail (path, NULL);
  g_return_val_if_fail (token, NULL);

  l = g_slice_new (LockSubmitted);

  l->path = g_strdup (path);
  l->token = g_strdup (token);

  remove_trailing (l->path, '/');

  return l;
}

static void
lock_submitted_free (LockSubmitted *l)
{
  g_free (l->path);
  g_free (l->token);
  g_slice_free (LockSubmitted, l);
}

static gboolean
locks_submitted_has (GList *locks, DAVLock *lock)
{
  GList *l;

  for (l = locks; l != NULL; l = l->next)
    {
      LockSubmitted *sub = l->data;
      if (!g_strcmp0 (sub->path, lock->path->path) &&
          !g_strcmp0 (sub->token, lock->token))
        return TRUE;
    }

  g_message ("missing lock: %s %s", lock->path->path, lock->token);

  return FALSE;
}

static gboolean
other_lock_exists (const gchar *key, Path *path, gpointer data)
{
  GList *locks = data;
  GList *l;

  for (l = path->locks; l != NULL; l = l->next)
    {
      DAVLock *lock = l->data;
      if (!locks_submitted_has (locks, lock))
        return FALSE;
    }

  return TRUE;
}

static gboolean
path_has_other_locks (PhodavServer *self, const gchar *path, GList *locks)
{
  return !foreach_parent_path (self, path, other_lock_exists, locks);
}

typedef struct _IfState
{
  gchar   *cur;
  gchar   *path;
  GList   *locks;
  gboolean error;
} IfState;

static gboolean
eat_whitespaces (IfState *state)
{
  while (*state->cur && strchr (" \f\n\r\t\v", *state->cur))
    state->cur++;

  return !*state->cur;
}

static gboolean
next_token (IfState *state, const gchar *token)
{
  eat_whitespaces (state);

  return g_str_has_prefix (state->cur, token);
}

static gboolean
accept_token (IfState *state, const gchar *token)
{
  gboolean success = next_token (state, token);

  if (success)
    state->cur += strlen (token);

  return success;
}

static const gchar*
accept_ref (IfState *state)
{
  gchar *url, *end;

  if (!accept_token (state, "<"))
    return FALSE;

  url = state->cur;
  end = strchr (state->cur, '>');
  if (end)
    {
      *end = '\0';
      state->cur = end + 1;
      return url;
    }

  return NULL;
}

static gchar*
accept_etag (IfState *state)
{
  GString *str = NULL;
  gboolean success = FALSE;

  str = g_string_sized_new (strlen (state->cur));

  if (!accept_token (state, "["))
    goto end;

  if (!accept_token (state, "\""))
    goto end;

  while (*state->cur)
    {
      if (*state->cur == '"')
        break;
      else if (*state->cur == '\\')
        state->cur++;

      g_string_append_c (str, *state->cur);
      state->cur++;
    }

  if (!accept_token (state, "\""))
    goto end;

  if (!accept_token (state, "]"))
    goto end;

  success = TRUE;

end:
  return g_string_free (str, !success);
}

static gboolean
check_token (PathHandler *handler, const gchar *path, const gchar *token)
{
  PhodavServer *self = handler->self;

  g_debug ("check %s for %s", token, path);

  if (!g_strcmp0 (token, "DAV:no-lock"))
    return FALSE;

  return !!path_get_lock (self, path, token);
}

static gboolean
check_etag (PathHandler *handler, const gchar *path, const gchar *etag)
{
  PhodavServer *self = handler->self;
  GFile *file = NULL;
  GFileInfo *info = NULL;
  GError *error = NULL;
  const gchar *fetag;
  gboolean success = FALSE;

  g_debug ("check etag %s for %s", etag, path);

  file = g_file_get_child (handler->file, path + 1);
  info = g_file_query_info (file, "etag::*",
                            G_FILE_QUERY_INFO_NONE, self->cancellable, &error);
  if (!info)
    goto end;

  fetag = g_file_info_get_etag (info);
  g_warn_if_fail (fetag != NULL);

  success = !g_strcmp0 (etag, fetag);

end:
  if (error)
    {
      g_warning ("check_etag error: %s", error->message);
      g_clear_error (&error);
    }

  g_clear_object (&info);
  g_clear_object (&file);

  return success;
}

static gboolean
eval_if_condition (PathHandler *handler, IfState *state)
{
  gboolean success = FALSE;

  if (next_token (state, "<"))
    {
      const gchar *token = accept_ref (state);
      LockSubmitted *l = lock_submitted_new (state->path, token);

      state->locks = g_list_append (state->locks, l);

      success = check_token (handler, state->path, token);
    }
  else if (next_token (state, "["))
    {
      gchar *etag = accept_etag (state);

      success = check_etag (handler, state->path, etag);
      g_free (etag);
    }
  else
    g_warn_if_reached ();

  return success;
}

static gboolean
eval_if_not_condition (PathHandler *handler, IfState *state)
{
  gboolean not = FALSE;
  gboolean res;

  if (accept_token (state, "Not"))
    not = TRUE;

  res = eval_if_condition (handler, state);

  return not ? !res : res;
}

static gboolean
eval_if_list (PathHandler *handler, IfState *state)
{
  gboolean success;

  g_return_val_if_fail (accept_token (state, "("), FALSE);

  success = eval_if_not_condition (handler, state);

  while (!accept_token (state, ")"))
    success &= eval_if_not_condition (handler, state);

  return success;
}

static gboolean
eval_if_lists (PathHandler *handler, IfState *state)
{
  gboolean success = FALSE;

  g_return_val_if_fail (next_token (state, "("), FALSE);

  while (next_token (state, "("))
    success |= eval_if_list (handler, state);

  return success;
}

static gboolean
eval_if_tag (PathHandler *handler, IfState *state)
{
  SoupURI *uri;
  const gchar *path;
  const gchar *ref = accept_ref (state);

  g_return_val_if_fail (ref != NULL, FALSE);

  uri = soup_uri_new (ref);
  path = soup_uri_get_path (uri);
  g_free (state->path);
  state->path = g_strdup (path);
  soup_uri_free (uri);

  return eval_if_lists (handler, state);
}


static gboolean
eval_if (PathHandler *handler, IfState *state)
{
  gboolean success = FALSE;

  if (next_token (state, "<")) {
    while (!eat_whitespaces (state))
      success |= eval_if_tag (handler, state);
  } else {
    while (!eat_whitespaces (state))
      success |= eval_if_lists (handler, state);
  }

  return success;
}

static gint
check_if (PathHandler *handler, SoupMessage *msg, const gchar *path, GList **locks)
{
  PhodavServer *self = handler->self;
  gboolean success = TRUE;
  gint status;
  gchar *str = g_strdup (soup_message_headers_get_one (msg->request_headers, "If"));
  IfState state = { .cur = str, .path = g_strdup (path) };
  gboolean copy = msg->method == SOUP_METHOD_COPY;

  if (!str)
    goto end;

  if (eval_if (handler, &state))
    {
      *locks = state.locks;
    }
  else
    {
      g_list_free_full (state.locks, (GDestroyNotify) lock_submitted_free);
      success = FALSE;
    }

end:
  status = success ? SOUP_STATUS_OK
           : str ? SOUP_STATUS_PRECONDITION_FAILED : SOUP_STATUS_LOCKED;

  if (success && !copy && path_has_other_locks (self, path, *locks))
    status = SOUP_STATUS_LOCKED;

  g_free (str);
  g_free (state.path);
  return status;
}

static gint
method_proppatch (PathHandler *handler, SoupMessage *msg,
                  const char *path, GError **err)
{
  PhodavServer *self = handler->self;
  GFile *file = NULL;
  GHashTable *mstatus = NULL;   // path -> statlist
  DavDoc doc = {0, };
  xmlNodePtr node = NULL, attr = NULL;
  GList *props = NULL, *submitted = NULL;
  gint status;

  if (!davdoc_parse (&doc, msg, msg->request_body, "propertyupdate"))
    {
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }

  status = check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  file = g_file_get_child (handler->file, path + 1);
  mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) response_free);

  node = doc.root;
  for (node = node->children; node; node = node->next)
    {
      if (!node_is_element (node))
        continue;

      if (node_has_name (node, "set"))
        status = prop_set (self, msg, file, node, &attr, FALSE);
      else if (node_has_name (node, "remove"))
        status = prop_set (self, msg, file, node, &attr, TRUE);
      else
        g_warn_if_reached ();

      if (attr)
        {
          attr->_private = GINT_TO_POINTER (status);
          props = g_list_append (props, attr);
        }
    }

  g_hash_table_insert (mstatus, g_strdup (path),
                       response_new (props, 0));

  if (g_hash_table_size (mstatus) > 0)
    status = response_multistatus (msg, mstatus);

end:
  davdoc_free (&doc);
  if (mstatus)
    g_hash_table_unref (mstatus);
  g_clear_object (&file);

  return status;
}

static gint
do_mkcol_file (SoupMessage *msg, GFile *file,
               GCancellable *cancellable, GError **err)
{
  GError *error = NULL;
  gint status = SOUP_STATUS_CREATED;

  if (!g_file_make_directory (file, cancellable, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        status = SOUP_STATUS_CONFLICT;
      else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        status = SOUP_STATUS_METHOD_NOT_ALLOWED;
      else
        {
          status = SOUP_STATUS_FORBIDDEN;
          g_propagate_error (err, error);
          error = NULL;
        }

      g_clear_error (&error);
    }

  return status;
}

static gint
method_mkcol (PathHandler *handler, SoupMessage *msg,
              const char *path, GError **err)
{
  GFile *file = NULL;
  PhodavServer *self = handler->self;
  gint status;
  GList *submitted = NULL;

  if (msg->request_body && msg->request_body->length)
    {
      status = SOUP_STATUS_UNSUPPORTED_MEDIA_TYPE;
      goto end;
    }

  status = check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  file = g_file_get_child (handler->file, path + 1);
  status = do_mkcol_file (msg, file, self->cancellable, err);

end:
  g_clear_object (&file);
  return status;
}

static gint
error_to_status (GError *err)
{
  if (g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    return SOUP_STATUS_NOT_FOUND;
  if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    return SOUP_STATUS_NOT_FOUND;

  return SOUP_STATUS_FORBIDDEN;
}

static gint
do_delete_file (const gchar *path, GFile *file,
                GHashTable *mstatus,
                GCancellable *cancellable)
{
  GError *error = NULL;
  GFileEnumerator *e;
  gint status = SOUP_STATUS_NO_CONTENT;

  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE,
                                 cancellable, NULL);
  if (e)
    {
      while (1)
        {
          GFileInfo *info = g_file_enumerator_next_file (e, cancellable, &error);
          if (!info)
            break;
          GFile *del = g_file_get_child (file, g_file_info_get_name (info));
          gchar *escape = g_markup_escape_text (g_file_info_get_name (info), -1);
          gchar *del_path = g_build_path ("/", path, escape, NULL);
          do_delete_file (del_path, del, mstatus, cancellable);
          g_object_unref (del);
          g_object_unref (info);
          g_free (escape);
          g_free (del_path);
        }

      g_file_enumerator_close (e, cancellable, NULL);
      g_clear_object (&e);
    }

  if (!g_file_delete (file, cancellable, &error) && mstatus)
    {
      status = error_to_status (error);

      g_hash_table_insert (mstatus, g_strdup (path),
                           response_new (NULL, status));
    }

  if (error)
    {
      g_debug ("ignored del error: %s", error->message);
      g_clear_error (&error);
    }

  return status;
}

static gint
method_delete (PathHandler *handler, SoupMessage *msg,
               const char *path, GError **err)
{
  PhodavServer *self = handler->self;
  GFile *file = NULL;
  GHashTable *mstatus = NULL;
  gint status;
  GList *submitted = NULL;

  /* depth = depth_from_string(soup_message_headers_get_one (msg->request_headers, "Depth")); */
  /* must be == infinity with collection */

  status = check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  file = g_file_get_child (handler->file, path + 1);
  mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) response_free);

  status = do_delete_file (path, file, mstatus, self->cancellable);
  if (status == SOUP_STATUS_NO_CONTENT)
    if (g_hash_table_size (mstatus) > 0)
      status = response_multistatus (msg, mstatus);

end:
  if (mstatus)
    g_hash_table_unref (mstatus);
  g_clear_object (&file);

  return status;
}

static gboolean
do_copy_r (GFile *src, GFile *dest, GFileCopyFlags flags,
           GCancellable *cancellable, GError **err)
{
  GFileEnumerator *e = NULL;
  gboolean success = FALSE;
  GFile *src_child = NULL;
  GFile *dest_child = NULL;

  if (!g_file_make_directory_with_parents (dest, cancellable, err))
    goto end;

  e = g_file_enumerate_children (src, "standard::*", G_FILE_QUERY_INFO_NONE,
                                 cancellable, err);
  if (!e)
    goto end;

  while (1)
    {
      GFileInfo *info = g_file_enumerator_next_file (e, cancellable, err);
      if (!info)
        break;

      src_child = g_file_get_child (src, g_file_info_get_name (info));
      dest_child = g_file_get_child (dest, g_file_info_get_name (info));
      gboolean isdir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
      g_object_unref (info);

      if (isdir)
        {
          if (!do_copy_r (src_child, dest_child, flags, cancellable, err))
            goto end;
        }
      else if (!g_file_copy (src_child, dest_child, flags, cancellable, NULL, NULL, err))
        goto end;

      g_clear_object (&src_child);
      g_clear_object (&dest_child);
    }

  success = TRUE;

end:
  g_clear_object (&e);
  g_clear_object (&src_child);
  g_clear_object (&dest_child);

  return success;
}

static gint
do_movecopy_file (SoupMessage *msg, GFile *file,
                  GFile *dest, const gchar *dest_path,
                  GCancellable *cancellable, GError **err)
{
  GError *error = NULL;
  gboolean overwrite;
  DepthType depth;
  gint status = SOUP_STATUS_PRECONDITION_FAILED;
  gboolean copy = msg->method == SOUP_METHOD_COPY;
  GFileCopyFlags flags = G_FILE_COPY_ALL_METADATA;
  gboolean retry = FALSE;
  gboolean exists;

  depth = depth_from_string (soup_message_headers_get_one (msg->request_headers, "Depth"));
  overwrite = !!g_strcmp0 (
    soup_message_headers_get_one (msg->request_headers, "Overwrite"), "F");
  if (overwrite)
    flags |= G_FILE_COPY_OVERWRITE;
  exists = g_file_query_exists (dest, cancellable);

again:
  switch (depth)
    {
    case DEPTH_INFINITY:
    case DEPTH_ZERO: {
        copy
        ? g_file_copy (file, dest, flags, cancellable, NULL, NULL, &error)
        : g_file_move (file, dest, flags, cancellable, NULL, NULL, &error);

        if (overwrite && !retry &&
            (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY) ||
             g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE)) &&
            do_delete_file (dest_path, dest, NULL, cancellable) == SOUP_STATUS_NO_CONTENT)
          {
            g_clear_error (&error);
            retry = TRUE;
            goto again;
          }
        else if (!overwrite &&
                 g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
          {
            g_clear_error (&error);
            goto end;
          }
        else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE))
          {
            g_clear_error (&error);
            if (copy)
              {
                if (depth == DEPTH_INFINITY)
                  do_copy_r (file, dest, flags, cancellable, &error);
                else
                  g_file_make_directory_with_parents (dest, cancellable, &error);
              }
          }
        else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
          {
            status = SOUP_STATUS_CONFLICT;
            g_clear_error (&error);
            goto end;
          }

        break;
      }

    default:
      g_warn_if_reached ();
    }

  if (error)
    g_propagate_error (err, error);
  else
    status = exists ? SOUP_STATUS_NO_CONTENT : SOUP_STATUS_CREATED;

end:
  return status;
}

static gint
method_movecopy (PathHandler *handler, SoupMessage *msg,
                 const char *path, GError **err)
{
  GFile *file = NULL, *dest_file = NULL;
  PhodavServer *self = handler->self;
  SoupURI *dest_uri = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;
  const gchar *dest;
  gchar *udest;
  GList *submitted = NULL;

  dest = soup_message_headers_get_one (msg->request_headers, "Destination");
  if (!dest)
    goto end;
  dest_uri = soup_uri_new (dest);
  dest = soup_uri_get_path (dest_uri);
  if (!dest || !*dest)
    goto end;

  status = check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  if (path_has_other_locks (self, dest, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  udest = g_uri_unescape_string (dest + 1, NULL);
  dest_file = g_file_get_child (handler->file, udest);
  g_free (udest);

  file = g_file_get_child (handler->file, path + 1);
  status = do_movecopy_file (msg, file, dest_file, dest,
                             self->cancellable, err);

end:
  if (dest_uri)
    soup_uri_free (dest_uri);
  g_clear_object (&file);
  g_clear_object (&dest_file);
  g_list_free_full (submitted, (GDestroyNotify) lock_submitted_free);
  return status;
}

static gboolean G_GNUC_PURE
check_lock (const gchar *key, Path *path, gpointer data)
{
  DAVLock *lock = data;
  DAVLock *other = NULL;
  GList *l;

  for (l = path->locks; l; l = l->next)
    {
      other = l->data;
      if (other->scope == LOCK_SCOPE_EXCLUSIVE)
        return FALSE;
    }

  if (other && lock->scope == LOCK_SCOPE_EXCLUSIVE)
    return FALSE;

  return TRUE;
}

static gboolean
try_add_lock (PhodavServer *self, const gchar *path, DAVLock *lock)
{
  Path *p;

  if (!foreach_parent_path (self, path, check_lock, lock))
    return FALSE;

  p = get_path (self, path);
  path_add_lock (p, lock);

  return TRUE;
}

static gboolean
lock_ensure_file (PathHandler *handler, const char *path,
                  GCancellable *cancellable, GError **err)
{
  GError *e = NULL;
  GFileOutputStream *stream = NULL;
  GFile *file = NULL;
  gboolean created = FALSE;

  file = g_file_get_child (handler->file, path + 1);
  stream = g_file_create (file, G_FILE_CREATE_NONE, cancellable, &e);
  created = !!stream;

  if (e)
    {
      if (g_error_matches (e, G_IO_ERROR, G_IO_ERROR_EXISTS))
        g_clear_error (&e);
      else
        g_propagate_error (err, e);
    }

  g_clear_object (&stream);
  g_clear_object (&file);

  return created;
}

static gint
method_lock (PathHandler *handler, SoupMessage *msg,
             const char *path, GError **err)
{
  PhodavServer *self = handler->self;
  Path *lpath = NULL;
  xmlChar *mem = NULL;
  int size = 0;
  DavDoc doc = {0, };
  xmlNodePtr node = NULL, owner = NULL, root = NULL;
  xmlNsPtr ns = NULL;
  LockScopeType scope = LOCK_SCOPE_SHARED;
  LockType type;
  DepthType depth;
  guint timeout;
  gchar *ltoken = NULL, *uuid = NULL, *token = NULL;
  DAVLock *lock = NULL;
  gint status = SOUP_STATUS_BAD_REQUEST;
  gboolean created;

  depth = depth_from_string (soup_message_headers_get_one (msg->request_headers, "Depth"));
  timeout = timeout_from_string (soup_message_headers_get_one (msg->request_headers, "Timeout"));

  if (depth != DEPTH_ZERO && depth != DEPTH_INFINITY)
    goto end;

  if (!msg->request_body->length)
    {
      const gchar *hif = soup_message_headers_get_one (msg->request_headers, "If");
      gint len = strlen (hif);

      if (len <= 4 || hif[0] != '(' || hif[1] != '<' || hif[len - 2] != '>' || hif[len - 1] != ')')
        goto end;

      token = g_strndup (hif + 2, len - 4);

      g_debug ("refresh token %s", token);
      lock = path_get_lock (self, path, token);

      if (!lock)
        goto end;

      dav_lock_refresh_timeout (lock, timeout);
      status = SOUP_STATUS_OK;
      goto body;
    }

  if (!davdoc_parse (&doc, msg, msg->request_body, "lockinfo"))
    goto end;

  node = doc.root;
  for (node = node->children; node; node = node->next)
    {
      if (!node_is_element (node))
        continue;

      if (node_has_name (node, "lockscope"))
        {
          scope = parse_lockscope (node);
          if (scope == LOCK_SCOPE_NONE)
            break;
        }
      else if (node_has_name (node, "locktype"))
        {
          type = parse_locktype (node);
          if (type == LOCK_NONE)
            break;
        }
      else if (node_has_name (node, "owner"))
        {
          if (owner == NULL)
            owner = node;
          else
            g_warn_if_reached ();
        }
    }

  g_debug ("lock depth:%d scope:%d type:%d owner:%p, timeout: %u",
           depth, scope, type, owner, timeout);

  uuid = g_uuid_random ();
  token = g_strdup_printf ("urn:uuid:%s", uuid);
  ltoken = g_strdup_printf ("<%s>", token);
  soup_message_headers_append (msg->response_headers, "Lock-Token", ltoken);

  lpath = get_path (self, path);
  lock = dav_lock_new (lpath, token, scope, type, depth, owner, timeout);
  if (!try_add_lock (self, path, lock))
    {
      g_warning ("lock failed");
      dav_lock_free (lock);
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  created = lock_ensure_file (handler, path, self->cancellable, err);
  if (*err)
    goto end;

  status = created ? SOUP_STATUS_CREATED : SOUP_STATUS_OK;

body:
  root = xmlNewNode (NULL, BAD_CAST "prop");
  ns = xmlNewNs (root, BAD_CAST "DAV:", BAD_CAST "D");
  xmlSetNs (root, ns);

  node = xmlNewChild (root, ns, BAD_CAST "lockdiscovery", NULL);
  xmlAddChild (node, get_activelock_node (lock, ns));

  node_to_string (root, &mem, &size);
  soup_message_set_response (msg, "application/xml",
                             SOUP_MEMORY_TAKE, (gchar *) mem, size);

end:
  g_free (ltoken);
  g_free (token);
  g_free (uuid);
  davdoc_free (&doc);

  return status;
}

static gchar *
remove_brackets (const gchar *str)
{
  if (!str)
    return NULL;

  gint len = strlen (str);

  if (str[0] != '<' || str[len - 1] != '>')
    return NULL;

  return g_strndup (str + 1, len - 2);
}

static gint
method_unlock (PathHandler *handler, SoupMessage *msg,
               const char *path, GError **err)
{
  PhodavServer *self = handler->self;
  DAVLock *lock;
  gint status = SOUP_STATUS_BAD_REQUEST;

  gchar *token = remove_brackets (
    soup_message_headers_get_one (msg->request_headers, "Lock-Token"));

  g_return_val_if_fail (token != NULL, SOUP_STATUS_BAD_REQUEST);

  lock = path_get_lock (self, path, token);
  if (!lock)
    return SOUP_STATUS_CONFLICT;

  dav_lock_free (lock);
  status = SOUP_STATUS_NO_CONTENT;

  g_free (token);
  return status;
}

static void
method_put_finished (SoupMessage *msg,
                     SoupBuffer  *chunk,
                     gpointer     user_data)
{
  GFileOutputStream *output = user_data;

  g_debug ("PUT finished");

  g_object_unref (output);
}

static void
method_put_got_chunk (SoupMessage *msg,
                      SoupBuffer  *chunk,
                      gpointer     user_data)
{
  GFileOutputStream *output = user_data;
  PathHandler *handler = g_object_get_data (user_data, "handler");
  PhodavServer *self = handler->self;
  GError *err = NULL;
  gsize bytes_written;

  g_debug ("PUT got chunk");

  if (!g_output_stream_write_all (G_OUTPUT_STREAM (output),
                                  chunk->data, chunk->length,
                                  &bytes_written, self->cancellable, &err))
    goto end;

end:
  if (err)
    {
      g_warning ("error: %s", err->message);
      g_clear_error (&err);
      soup_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
    }
}

static void
method_put (PathHandler *handler, const gchar *path, SoupMessage *msg, GError **err)
{
  PhodavServer *self = handler->self;
  GFile *file = NULL;
  GList *submitted = NULL;
  GFileOutputStream *output = NULL;
  gint status;

  status = check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  file = g_file_get_child (handler->file, path + 1);
  status = put_start (msg, file, &output, self->cancellable, err);
  if (*err)
    goto end;

  soup_message_body_set_accumulate (msg->request_body, FALSE);
  g_object_set_data (G_OBJECT (output), "handler", handler);
  g_signal_connect (msg, "got-chunk", G_CALLBACK (method_put_got_chunk), output);
  g_signal_connect (msg, "finished", G_CALLBACK (method_put_finished), output);

end:
  soup_message_set_status (msg, status);
  g_clear_object (&file);
}

static void
got_headers (SoupMessage *msg,
             gpointer     user_data)
{
  PhodavServer *self = user_data;
  SoupURI *uri = soup_message_get_uri (msg);
  const gchar *path = uri->path;
  GError *err = NULL;

  if (msg->method == SOUP_METHOD_PUT)
    method_put (self->root_handler, path, msg, &err);

  if (err)
    {
      g_warning ("error: %s", err->message);
      g_clear_error (&err);
    }
}

static void
request_started (SoupServer        *server,
                 SoupMessage       *message,
                 SoupClientContext *client,
                 gpointer           user_data)
{
  PhodavServer *self = user_data;

  g_signal_connect (message, "got-headers", G_CALLBACK (got_headers), self);
}

static void
server_callback (SoupServer *server, SoupMessage *msg,
                 const char *path, GHashTable *query,
                 SoupClientContext *context, gpointer user_data)
{
  GError *err = NULL;
  PathHandler *handler = user_data;
  gint status = SOUP_STATUS_NOT_IMPLEMENTED;
  SoupURI *uri = soup_message_get_uri (msg);
  GHashTable *params;

  g_debug ("%s %s HTTP/1.%d %s %s", msg->method, path, soup_message_get_http_version (msg),
           soup_message_headers_get_one (msg->request_headers, "X-Litmus") ? : "",
           soup_message_headers_get_one (msg->request_headers, "X-Litmus-Second") ? : "");

  if (!(path && *path == '/'))
    {
      g_debug ("path must begin with /");
      return;
    }
  if (!(uri && uri->fragment == NULL))
    {
      g_debug ("using fragments in query is not supported");
      return;
    }

  params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (params, g_strdup ("charset"), g_strdup ("utf-8"));
  soup_message_headers_set_content_type (msg->response_headers,
                                         "text/xml", params);
  g_hash_table_destroy (params);

  if (msg->method == SOUP_METHOD_OPTIONS)
    {
      soup_message_headers_append (msg->response_headers, "DAV", "1,2");

      /* according to http://code.google.com/p/sabredav/wiki/Windows */
      soup_message_headers_append (msg->response_headers, "MS-Author-Via", "DAV");

      soup_message_headers_append (msg->response_headers, "Allow",
                                   "GET, HEAD, PUT, PROPFIND, PROPPATCH, MKCOL, DELETE, MOVE, COPY, LOCK, UNLOCK");
      status = SOUP_STATUS_OK;
    }
  else if (msg->method == SOUP_METHOD_GET ||
           msg->method == SOUP_METHOD_HEAD)
    status = method_get (handler, msg, path, &err);
  else if (msg->method == SOUP_METHOD_PROPFIND)
    status = method_propfind (handler, msg, path, &err);
  else if (msg->method == SOUP_METHOD_PROPPATCH)
    status = method_proppatch (handler, msg, path, &err);
  else if (msg->method == SOUP_METHOD_MKCOL)
    status = method_mkcol (handler, msg, path, &err);
  else if (msg->method == SOUP_METHOD_DELETE)
    status = method_delete (handler, msg, path, &err);
  else if (msg->method == SOUP_METHOD_MOVE ||
           msg->method == SOUP_METHOD_COPY)
    status = method_movecopy (handler, msg, path, &err);
  else if (msg->method == SOUP_METHOD_LOCK)
    status = method_lock (handler, msg, path, &err);
  else if (msg->method == SOUP_METHOD_UNLOCK)
    status = method_unlock (handler, msg, path, &err);
  else
    g_warn_if_reached ();

  soup_message_set_status (msg, status);

  g_debug ("  -> %d %s\n", msg->status_code, msg->reason_phrase);
  if (err)
    {
      g_warning ("error: %s", err->message);
      g_clear_error (&err);
    }
}

guint
phodav_server_get_port (PhodavServer *self)
{
  g_return_val_if_fail (PHODAV_IS_SERVER (self), 0);

  return soup_server_get_port (self->server);
}

SoupServer *
phodav_server_get_soup_server (PhodavServer *self)
{
  g_return_val_if_fail (PHODAV_IS_SERVER (self), NULL);

  return self->server;
}

static gpointer
thread_func (gpointer data)
{
  PhodavServer *self = data;

  g_debug ("Starting on port %u, serving %s", phodav_server_get_port (self), self->root);

  soup_server_run_async (self->server);

  g_main_loop_run (self->loop);

  return NULL;
}

void
phodav_server_run (PhodavServer *self)
{
  g_return_if_fail (PHODAV_IS_SERVER (self));

  if (self->thread)
    return;

  g_object_ref (self);
  self->thread = g_thread_new ("phodav-server", thread_func, self);
}

void
phodav_server_quit (PhodavServer *self)
{
  g_return_if_fail (PHODAV_IS_SERVER (self));

  if (!self->thread)
    return;

  soup_server_quit (self->server);
  g_main_loop_quit (self->loop);
  g_thread_join (self->thread);
  self->thread = NULL;
  g_object_unref (self);
}

PhodavServer *
phodav_server_new (guint port, const gchar *root)
{
  return g_object_new (PHODAV_TYPE_SERVER,
                       "port", port,
                       "root", root,
                       NULL);
}
