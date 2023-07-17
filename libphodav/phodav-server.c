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

#include "guuid.h"
#include "phodav-server.h"
#include "phodav-multistatus.h"
#include "phodav-path.h"
#include "phodav-lock.h"
#include "phodav-utils.h"

/**
 * SECTION:phodav-server
 * @title: PhodavServer
 * @short_description: A WebDAV server
 * @see_also: #SoupServer
 * @stability: Stable
 * @include: libphodav/phodav.h
 *
 * PhodavServer implements a simple WebDAV server.
 */

struct _PhodavServer
{
  GObject       parent;
  SoupServer   *server;
  GCancellable *cancellable;
  GFile        *root_file;
  PathHandler  *root_handler; /* weak ref */
  GHashTable   *paths;
  gboolean      readonly;
};

struct _PhodavServerClass
{
  GObjectClass parent_class;
};

G_DEFINE_TYPE (PhodavServer, phodav_server, G_TYPE_OBJECT)

/* Properties */
enum {
  PROP_0,
  PROP_ROOT,
  PROP_ROOT_FILE,
  PROP_SERVER,
  PROP_READONLY,
};

static void server_callback (SoupServer        *server,
                             SoupServerMessage *msg,
                             const char        *path,
                             GHashTable        *query,
                             gpointer           user_data);

static void request_started (SoupServer        *server,
                             SoupServerMessage *message,
                             gpointer           user_data);

Path *
server_get_path (PhodavServer *self, const gchar *_path)
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
  else
    {
      g_free (path);
    }

  return p;
}

struct _PathHandler
{
  PhodavServer *self;
  GFile       *file;
};

PhodavServer * G_GNUC_PURE
handler_get_server (PathHandler *handler)
{
  return handler->self;
}

GFile * G_GNUC_PURE
handler_get_file (PathHandler *handler)
{
  return handler->file;
}

GCancellable * G_GNUC_PURE
handler_get_cancellable (PathHandler *handler)
{
  return handler->self->cancellable;
}

gboolean G_GNUC_PURE
handler_get_readonly (PathHandler *handler)
{
  return handler->self->readonly;
}

static PathHandler *
path_handler_new (PhodavServer *self, GFile *file)
{
  PathHandler *h = g_slice_new0 (PathHandler);

  h->self = self;
  h->file = g_object_ref (file);
  return h;
}

static void
path_handler_free (PathHandler *h)
{
  g_object_unref (h->file);
  g_slice_free (PathHandler, h);
}

static void
phodav_server_init (PhodavServer *self)
{
  self->cancellable = g_cancellable_new ();

  self->paths = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       NULL, (GDestroyNotify) path_unref);
}

static void
update_root_handler (PhodavServer *self)
{
  PathHandler *handler;

  if (!self->root_file || !self->server)
    return;

  handler = path_handler_new (self, self->root_file);

  soup_server_add_handler (self->server, "/",
                           server_callback,
                           handler,
                           (GDestroyNotify) path_handler_free);

  self->root_handler = handler;
}

static void
phodav_server_constructed (GObject *gobject)
{
  PhodavServer *self = PHODAV_SERVER (gobject);

  self->server = soup_server_new ("server-header", "PhodavServer ",
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

  /* SoupServer could live longer than PhodavServer,
   * this frees the PhodavHandler passed as user_data */
  soup_server_remove_handler (self->server, "/");
  g_signal_handlers_disconnect_by_func (self->server, request_started, self);
  g_clear_object (&self->server);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->root_file);
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
    case PROP_ROOT:
      g_value_take_string (value, self->root_file ?
        g_file_get_path (self->root_file) : NULL);
      break;

    case PROP_ROOT_FILE:
      g_value_set_object (value, self->root_file);
      break;

    case PROP_SERVER:
      g_value_set_object (value, self->server);
      break;

    case PROP_READONLY:
      g_value_set_boolean (value, self->readonly);
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
  const gchar *root;

  /* do not overwrite the root file during construction,
   * phodav should be constructed either with "root" or "root-file", not both */
  if (!self->server && self->root_file)
    return;

  switch (prop_id)
    {
    case PROP_ROOT:
      root = g_value_get_string (value);
      g_clear_object (&self->root_file);
      if (root)
        self->root_file = g_file_new_for_path (root);
      update_root_handler (self);
      break;

    case PROP_ROOT_FILE:
      g_clear_object (&self->root_file);
      self->root_file = g_value_dup_object (value);
      update_root_handler (self);
      break;

    case PROP_READONLY:
      self->readonly = g_value_get_boolean (value);
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

  /**
   * PhodavServer:root:
   *
   * This property is interconnected with the property #PhodavServer:root-file.
   * Changing #PhodavServer:root affects #PhodavServer:root-file and vice versa.
   *
   * You can use either of them or none to construct a new #PhodavServer,
   * you should not use both.
   **/
  g_object_class_install_property
    (gobject_class, PROP_ROOT,
    g_param_spec_string ("root",
                         "Root path",
                         "Root path",
                         NULL,
                         G_PARAM_CONSTRUCT |
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS));

/**
   * PhodavServer:root-file:
   *
   * A more generic version of #PhodavServer:root,
   * see it for more details.
   *
   * Since: 2.5
   **/
  g_object_class_install_property
    (gobject_class, PROP_ROOT_FILE,
     g_param_spec_object ("root-file",
                          "Root file",
                          "Root file",
                          G_TYPE_FILE,
                          G_PARAM_CONSTRUCT |
                          G_PARAM_READWRITE |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property
    (gobject_class, PROP_SERVER,
     g_param_spec_object ("server",
                          "Soup Server",
                          "Soup Server",
                          SOUP_TYPE_SERVER,
                          G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property
    (gobject_class, PROP_READONLY,
     g_param_spec_boolean ("read-only",
                           "Read-only access",
                           "Read-only access",
                           FALSE,
                           G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));
}

gboolean
server_foreach_parent_path (PhodavServer *self, const gchar *path, PathCb cb, gpointer data)
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

DAVLock *
server_path_get_lock (PhodavServer *self, const gchar *path, const gchar *token)
{
  PathGetLock p = { .token = token };
  gboolean success = !server_foreach_parent_path (self, path,
                                                  _path_get_lock, (gpointer) &p);

  if (!success)
    g_message ("Invalid lock token %s for %s", token, path);

  return p.lock;
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

gboolean
server_path_has_other_locks (PhodavServer *self, const gchar *path, GList *locks)
{
  return !server_foreach_parent_path (self, path, other_lock_exists, locks);
}

static void
got_headers (SoupServerMessage *msg,
             gpointer     user_data)
{
  PhodavServer *self = user_data;
  GUri *uri = soup_server_message_get_uri (msg);
  const gchar *path = g_uri_get_path (uri);
  GError *err = NULL;

  if (soup_server_message_get_method (msg) == SOUP_METHOD_PUT)
    phodav_method_put (self->root_handler, msg, path, &err);

  if (err)
    {
      g_warning ("error: %s", err->message);
      g_clear_error (&err);
    }
}

static void
request_started (SoupServer        *server,
                 SoupServerMessage *message,
                 gpointer           user_data)
{
  PhodavServer *self = user_data;

  g_signal_connect (message, "got-headers", G_CALLBACK (got_headers), self);
}

static void
server_callback (SoupServer *server, SoupServerMessage *msg,
                 const char *path, GHashTable *query,
                 gpointer user_data)
{
  GError *err = NULL;
  PathHandler *handler = user_data;
  gint status = SOUP_STATUS_NOT_IMPLEMENTED;
  GUri *uri = soup_server_message_get_uri (msg);
  const char *method = soup_server_message_get_method (msg);
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  SoupMessageHeaders *response_headers;
  GHashTable *params;

  g_debug ("%s %s HTTP/1.%d %s %s", method, path, soup_server_message_get_http_version (msg),
           soup_message_headers_get_one (request_headers, "X-Litmus") ? : "",
           soup_message_headers_get_one (request_headers, "X-Litmus-Second") ? : "");

  if (!(path && *path == '/'))
    {
      g_debug ("path must begin with /");
      return;
    }
  if (g_path_is_absolute (path + 1))
    {
      g_debug ("path cannot be absolute");
      return;
    }

  if (!(uri && g_uri_get_fragment (uri) == NULL))
    {
      g_debug ("using fragments in query is not supported");
      return;
    }

  params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (params, g_strdup ("charset"), g_strdup ("utf-8"));
  response_headers = soup_server_message_get_response_headers (msg);
  soup_message_headers_set_content_type (response_headers,
                                         "text/xml", params);
  g_hash_table_destroy (params);

  if (handler->self->readonly &&
      (method == SOUP_METHOD_PROPPATCH ||
       method == SOUP_METHOD_MKCOL ||
       method == SOUP_METHOD_DELETE ||
       method == SOUP_METHOD_MOVE ||
       method == SOUP_METHOD_COPY ||
       method == SOUP_METHOD_LOCK))
      status = SOUP_STATUS_FORBIDDEN;
  else if (method == SOUP_METHOD_OPTIONS)
    {
      soup_message_headers_append (response_headers, "DAV", "1,2");

      /* according to http://code.google.com/p/sabredav/wiki/Windows */
      soup_message_headers_append (response_headers, "MS-Author-Via", "DAV");

      soup_message_headers_append (response_headers, "Allow",
                                   "GET, HEAD, PUT, PROPFIND, PROPPATCH, MKCOL, DELETE, MOVE, COPY, LOCK, UNLOCK");
      status = SOUP_STATUS_OK;
    }
  else if (method == SOUP_METHOD_GET ||
           method == SOUP_METHOD_HEAD)
    status = phodav_method_get (handler, msg, path, &err);
  else if (method == SOUP_METHOD_PROPFIND)
    status = phodav_method_propfind (handler, msg, path, &err);
  else if (method == SOUP_METHOD_PROPPATCH)
    status = phodav_method_proppatch (handler, msg, path, &err);
  else if (method == SOUP_METHOD_MKCOL)
    status = phodav_method_mkcol (handler, msg, path, &err);
  else if (method == SOUP_METHOD_DELETE)
    status = phodav_method_delete (handler, msg, path, &err);
  else if (method == SOUP_METHOD_MOVE ||
           method == SOUP_METHOD_COPY)
    status = phodav_method_movecopy (handler, msg, path, &err);
  else if (method == SOUP_METHOD_LOCK)
    status = phodav_method_lock (handler, msg, path, &err);
  else if (method == SOUP_METHOD_UNLOCK)
    status = phodav_method_unlock (handler, msg, path, &err);
  else
    g_warn_if_reached ();

  soup_server_message_set_status (msg, status, NULL);

  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
  if (err)
    {
      g_warning ("error: %s", err->message);
      g_clear_error (&err);
    }
}

/**
 * phodav_server_get_soup_server:
 * @server: a %PhodavServer
 *
 * Returns the underlying %SoupServer, if any.
 *
 * Returns: the associated %SoupServer or %NULL
 **/
SoupServer *
phodav_server_get_soup_server (PhodavServer *self)
{
  g_return_val_if_fail (PHODAV_IS_SERVER (self), NULL);

  return self->server;
}

/**
 * phodav_server_new:
 * @root: (allow-none): Root path.
 *
 * Creates a new #PhodavServer.
 * This is equivalent to constructing a #GFile from @root
 * and using phodav_server_new_for_root_file().
 *
 * Returns: a new #PhodavServer
 **/
PhodavServer *
phodav_server_new (const gchar *root)
{
  return g_object_new (PHODAV_TYPE_SERVER,
                       "root", root,
                       NULL);
}

/**
 * phodav_server_new_for_root_file:
 * @root: (nullable): a #GFile specifying the root
 *
 * Creates a new #PhodavServer and publishes the files under @root.
 *
 * Returns: (transfer full): a new #PhodavServer
 *
 * Since: 2.5
 **/
PhodavServer *
phodav_server_new_for_root_file (GFile *root)
{
  return g_object_new (PHODAV_TYPE_SERVER,
                       "root-file",
                       root,
                       NULL);
}
