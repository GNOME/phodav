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

#include "phodav-server.h"

#include "phodav-lock.h"
#include "phodav-multistatus.h"
#include "phodav-path.h"
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
 *
 * The DAV methods are handled synchronously from the #SoupServer
 * callbacks: a #PhodavServer must be used from a single thread, the
 * one running the main context the #SoupServer is listened from,
 * and a slow request delays the other requests.
 */

struct _PhodavServer
{
  GObject parent;
  SoupServer *server;
  GCancellable *cancellable;
  GFile *root_file;
  PathHandler *root_handler; /* weak ref */
  GHashTable *paths;
  gboolean readonly;
  gboolean constructing;
  GMainContext *expire_locks_context;
  GSource *expire_locks_source;
};

G_DEFINE_FINAL_TYPE (PhodavServer, phodav_server, G_TYPE_OBJECT)

/* Properties */
typedef enum
{
  PROP_ROOT = 1,
  PROP_ROOT_FILE,
  PROP_SERVER,
  PROP_READONLY,
} PhodavServerProps;

static GParamSpec *properties[PROP_READONLY + 1];

static void server_callback (SoupServer *server,
                             SoupServerMessage *msg,
                             const gchar *path,
                             GHashTable *query,
                             gpointer user_data);

static void request_started (SoupServer *server,
                             SoupServerMessage *message,
                             gpointer user_data);

/* how often to look for expired locks; they are also expired lazily
 * when they are used, this only reclaims the memory */
#define LOCK_EXPIRY_INTERVAL 1

static gboolean expire_locks_timeout (gpointer user_data);

/* Canonicalize a request path for use as a key in the paths table:
 * empty segments ("//") and trailing slashes are skipped, just like
 * the walk in server_foreach_parent_path() does, so that locks are
 * found again under whatever form the client used in its request. */
static gchar *
path_key (const gchar *_path)
{
  g_auto(GStrv) parts = g_strsplit (_path, "/", -1);
  g_autoptr(GString) path = g_string_new (NULL);

  for (gint i = 0; parts[i]; i++)
    {
      if (!*parts[i])
        continue;

      g_string_append_c (path, '/');
      g_string_append (path, parts[i]);
    }

  if (!path->len)
    g_string_append_c (path, '/');

  return g_string_free_and_steal (g_steal_pointer (&path));
}

Path *
server_get_path (PhodavServer *self, const gchar *_path)
{
  Path *p;
  g_autofree gchar *path = path_key (_path);

  p = g_hash_table_lookup (self->paths, path);
  if (!p)
    {
      p = g_new0 (Path, 1);
      p->path = g_steal_pointer (&path);
      g_hash_table_insert (self->paths, p->path, path_ref (p));
    }

  return p;
}

struct _PathHandler
{
  PhodavServer *self;
  GFile *file;
};

PhodavServer *
handler_get_server (PathHandler *handler)
{
  return handler->self;
}

GFile *
handler_get_file (PathHandler *handler)
{
  return handler->file;
}

GCancellable *
handler_get_cancellable (PathHandler *handler)
{
  return handler->self->cancellable;
}

gboolean
handler_get_readonly (PathHandler *handler)
{
  return handler->self->readonly;
}

static PathHandler *
path_handler_new (PhodavServer *self, GFile *file)
{
  PathHandler *h = g_new0 (PathHandler, 1);

  h->self = self;
  h->file = g_object_ref (file);
  return h;
}

static void
path_handler_free (PathHandler *h)
{
  g_object_unref (h->file);
  g_free (h);
}

static void
phodav_server_init (PhodavServer *self)
{
  self->constructing = TRUE;

  self->cancellable = g_cancellable_new ();

  self->paths = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       NULL, (GDestroyNotify) path_unref);

  /* the context this server is created on; the lock expiry sweep
   * runs there, like the rest of the server */
  self->expire_locks_context = g_main_context_ref (g_main_context_get_thread_default () ?: g_main_context_default ());
}

/* free the locks whose timeout has elapsed, and drop the empty paths
 * so the lock table does not grow without bound with the locks taken
 * over the lifetime of the server */
void
server_expire_locks (PhodavServer *self)
{
  GHashTableIter iter;
  Path *path;

  if (g_hash_table_size (self->paths) == 0)
    return;

  g_hash_table_iter_init (&iter, self->paths);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &path))
    {
      path_expire_locks (path);

      /* the path is only kept alive by the table when it has no locks */
      if (!path->locks)
        g_hash_table_iter_remove (&iter);
    }
}

static gboolean
expire_locks_timeout (gpointer user_data)
{
  server_expire_locks (PHODAV_SERVER (user_data));

  return G_SOURCE_CONTINUE;
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

  /* the construct properties are set at this point */
  self->constructing = FALSE;

  /* a #SoupServer given at construction is used as is, so that it
   * can be pre-configured with listeners, TLS or auth domains;
   * otherwise create a plain one */
  if (!self->server)
    self->server = soup_server_new ("server-header", "PhodavServer",
                                    NULL);

  /* when a server was given at construction, the root handler is
   * already registered on it */
  if (!self->root_handler)
    update_root_handler (self);

  g_signal_connect (self->server, "request-started", G_CALLBACK (request_started), self);

  {
    g_autoptr(GSource) source = g_timeout_source_new_seconds (LOCK_EXPIRY_INTERVAL);

    /* g_timeout_add_seconds() cannot be used here: it would attach
     * the sweep to the global default context instead of the one
     * this server runs on */
    g_source_set_callback (source, expire_locks_timeout, self, NULL);
    g_source_attach (source, self->expire_locks_context);
    /* the context owns the source while it is attached and dispose()
     * detaches it, keep only a borrowed pointer */
    self->expire_locks_source = source;
  }

  /* Chain up to the parent class */
  if (G_OBJECT_CLASS (phodav_server_parent_class)->constructed)
    G_OBJECT_CLASS (phodav_server_parent_class)->constructed (gobject);
}

static void
free_path_locks (gpointer key, gpointer value, gpointer user_data)
{
  Path *path = value;

  /* the locks remove themselves from the list when freed */
  while (path->locks)
    dav_lock_free (path->locks->data);
}

static void
phodav_server_dispose (GObject *gobject)
{
  PhodavServer *self = PHODAV_SERVER (gobject);

  g_clear_pointer (&self->expire_locks_source, g_source_destroy);
  g_clear_pointer (&self->expire_locks_context, g_main_context_unref);

  /* SoupServer could live longer than PhodavServer,
   * this frees the PhodavHandler passed as user_data;
   * dispose may run again, the server is already gone then */
  if (self->server)
    {
      soup_server_remove_handler (self->server, "/");
      g_signal_handlers_disconnect_by_func (self->server, request_started, self);
    }
  g_clear_object (&self->server);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->root_file);

  /* remaining locks hold references on the paths, free them first
   * so the paths are released with the table */
  if (self->paths)
    g_hash_table_foreach (self->paths, free_path_locks, NULL);
  g_clear_pointer (&self->paths, g_hash_table_unref);

  /* Chain up to the parent class */
  if (G_OBJECT_CLASS (phodav_server_parent_class)->dispose)
    G_OBJECT_CLASS (phodav_server_parent_class)->dispose (gobject);
}

static void
phodav_server_get_property (GObject *gobject,
                            guint prop_id,
                            GValue *value,
                            GParamSpec *pspec)
{
  PhodavServer *self = PHODAV_SERVER (gobject);

  switch ((PhodavServerProps) prop_id)
    {
    case PROP_ROOT:
      g_value_take_string (value, self->root_file ? g_file_get_path (self->root_file) : NULL);
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
    }
}

static void
phodav_server_set_property (GObject *gobject,
                            guint prop_id,
                            const GValue *value,
                            GParamSpec *pspec)
{
  PhodavServer *self = PHODAV_SERVER (gobject);
  const gchar *root;

  switch ((PhodavServerProps) prop_id)
    {
    case PROP_ROOT:
    case PROP_ROOT_FILE:
      /* during construction, both root properties are set in turn,
       * with their default NULL value for the one which was not
       * given; once a root file is set, do not let the redundant
       * NULL of the other one clobber it: phodav should be
       * constructed either with "root" or "root-file", not both */
      if (self->constructing && self->root_file)
        return;

      if (prop_id == PROP_ROOT)
        {
          root = g_value_get_string (value);
          g_clear_object (&self->root_file);
          if (root)
            self->root_file = g_file_new_for_path (root);
        }
      else
        {
          g_clear_object (&self->root_file);
          self->root_file = g_value_dup_object (value);
        }
      update_root_handler (self);
      break;

    case PROP_READONLY:
      self->readonly = g_value_get_boolean (value);
      break;

    case PROP_SERVER:
      /* construct-only */
      g_return_if_fail (self->server == NULL);
      self->server = g_value_dup_object (value);
      break;
    }
}

static void
phodav_server_class_init (PhodavServerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = phodav_server_dispose;
  gobject_class->constructed = phodav_server_constructed;
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
  properties[PROP_ROOT] =
    g_param_spec_string ("root",
                         NULL, NULL,
                         NULL,
                         G_PARAM_CONSTRUCT |
                           G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS);

  /**
   * PhodavServer:root-file:
   *
   * A more generic version of #PhodavServer:root,
   * see it for more details.
   *
   * Since: 2.5
   **/
  properties[PROP_ROOT_FILE] =
    g_param_spec_object ("root-file",
                         NULL, NULL,
                         G_TYPE_FILE,
                         G_PARAM_CONSTRUCT |
                           G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS);

  /**
   * PhodavServer:server:
   *
   * The underlying #SoupServer.
   *
   * When constructing a #PhodavServer, an already configured
   * #SoupServer may be given, with listeners, TLS certificates or
   * auth domains set up for example. When no server is provided, a
   * new one is created.
   *
   * Since: 3.1
   **/
  properties[PROP_SERVER] =
    g_param_spec_object ("server",
                         NULL, NULL,
                         SOUP_TYPE_SERVER,
                         G_PARAM_READWRITE |
                           G_PARAM_CONSTRUCT_ONLY |
                           G_PARAM_STATIC_STRINGS);

  /**
   * PhodavServer:read-only:
   *
   * Whether the shared folder is read-only.
   **/
  properties[PROP_READONLY] =
    g_param_spec_boolean ("read-only",
                          NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                            G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (properties), properties);
}

gboolean
server_foreach_parent_path (PhodavServer *self, const gchar *path, PathCb cb, gpointer data)
{
  g_auto(GStrv) pathv = NULL;
  g_autofree gchar *partial = g_strdup ("/");
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
  return ret;
}

typedef struct _PathGetLock
{
  const gchar *token;
  DAVLock *lock;
} PathGetLock;

static gboolean
_path_get_lock (const gchar *key, Path *path, gpointer data)
{
  PathGetLock *d = data;
  GList *l;

  path_expire_locks (path);

  for (l = path->locks; l != NULL; l = l->next)
    {
      DAVLock *lock = l->data;

      if (g_strcmp0 (lock->token, d->token) == 0)
        {
          d->lock = lock;
          return FALSE;
        }
    }

  return TRUE;
}

/* look for the lock with @token on @path or any of its parents,
 * without logging when it is not found */
DAVLock *
server_path_lookup_lock (PhodavServer *self, const gchar *path, const gchar *token)
{
  PathGetLock p = { .token = token };

  server_foreach_parent_path (self, path, _path_get_lock, &p);

  return p.lock;
}

DAVLock *
server_path_get_lock (PhodavServer *self, const gchar *path, const gchar *token)
{
  DAVLock *lock = server_path_lookup_lock (self, path, token);

  if (!lock)
    g_message ("Invalid lock token %s for %s", token, path);

  return lock;
}

static gboolean
other_lock_exists (const gchar *key, Path *path, gpointer data)
{
  GList *locks = data;
  GList *l;

  path_expire_locks (path);

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

typedef struct _TreeLockCheck
{
  const gchar *path; /* canonical path of the new lock root */
  DAVLock *lock;     /* the lock about to be taken */
  gboolean conflict;
} TreeLockCheck;

static void
tree_check_lock_cb (gpointer key, gpointer value, gpointer user_data)
{
  TreeLockCheck *d = user_data;
  Path *path = value;
  GList *l;

  path_expire_locks (path);

  /* the lock root itself and its ancestors are checked by
   * server_foreach_parent_path() in try_add_lock() */
  if (g_strcmp0 (path->path, d->path) == 0 ||
      !path_is_covered (path->path, d->path))
    return;

  for (l = path->locks; l != NULL; l = l->next)
    {
      DAVLock *other = l->data;

      /* RFC 4918 section 6.1: an exclusive lock conflicts with any
       * other kind of lock, whether direct or indirect */
      if (other->scope == DAV_LOCK_SCOPE_EXCLUSIVE ||
          d->lock->scope == DAV_LOCK_SCOPE_EXCLUSIVE)
        d->conflict = TRUE;
    }
}

/* does a depth-infinity lock on @path conflict with a lock on one of
 * its members? (RFC 4918 section 7.4: such a LOCK request must fail
 * with 423 when the members are locked in a conflicting manner) */
gboolean
server_tree_has_lock_conflict (PhodavServer *self, const gchar *_path, DAVLock *lock)
{
  g_autofree gchar *path = path_key (_path);
  TreeLockCheck d = { .path = path, .lock = lock, .conflict = FALSE };

  g_hash_table_foreach (self->paths, tree_check_lock_cb, &d);

  return d.conflict;
}

static void
got_headers (SoupServerMessage *msg,
             gpointer user_data)
{
  PhodavServer *self = user_data;
  GUri *uri = soup_server_message_get_uri (msg);
  const gchar *path = g_uri_get_path (uri);
  g_autoptr(GError) err = NULL;
  const gchar *method = soup_server_message_get_method (msg);
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);

  /* libsoup handlers ran first, the request may have been rejected
   * already (invalid path, authentication...) */
  if (soup_server_message_get_status (msg) != 0)
    return;

  if (method == SOUP_METHOD_PUT)
    {
      if (!(path && *path == '/') || g_path_is_absolute (path + 1))
        {
          g_debug ("invalid PUT path: %s", path);
          soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
          return;
        }

      phodav_method_put (self->root_handler, msg, path, &err);

      if (err)
        g_warning ("error: %s", err->message);

      return;
    }

  /* unlike PUT, the other methods have their request body parsed in
   * memory: refuse an unreasonable Content-Length before the body is
   * received (RFC 9110 section 15.5.15), and drain it without
   * accumulating it */
  if (soup_message_headers_get_content_length (request_headers) > PHODAV_MAX_REQUEST_BODY)
    {
      g_debug ("refusing the oversized request body");
      soup_message_body_set_accumulate (soup_server_message_get_request_body (msg), FALSE);
      soup_server_message_set_status (msg, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, NULL);
    }
}

static void
request_started (SoupServer *server,
                 SoupServerMessage *message,
                 gpointer user_data)
{
  PhodavServer *self = user_data;

  g_signal_connect (message, "got-headers", G_CALLBACK (got_headers), self);
}

static void
server_callback (SoupServer *server, SoupServerMessage *msg, const gchar *path, GHashTable *query, gpointer user_data)
{
  g_autoptr(GError) err = NULL;
  PathHandler *handler = user_data;
  gint status = SOUP_STATUS_NOT_IMPLEMENTED;

  GUri *uri = soup_server_message_get_uri (msg);
  const gchar *method = soup_server_message_get_method (msg);
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  SoupMessageHeaders *response_headers;
  g_autoptr(GHashTable) params = NULL;

  g_debug ("%s %s HTTP/1.%d %s %s", method, path, soup_server_message_get_http_version (msg),
           soup_message_headers_get_one (request_headers, "X-Litmus") ?: "",
           soup_message_headers_get_one (request_headers, "X-Litmus-Second") ?: "");

  if (!(path && *path == '/'))
    {
      g_debug ("path must begin with /");
      soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
      return;
    }
  if (g_path_is_absolute (path + 1))
    {
      g_debug ("path cannot be absolute");
      soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
      return;
    }

  if (!(uri && g_uri_get_fragment (uri) == NULL))
    {
      g_debug ("using fragments in query is not supported");
      soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
      return;
    }

  params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (params, g_strdup ("charset"), g_strdup ("utf-8"));
  response_headers = soup_server_message_get_response_headers (msg);
  soup_message_headers_set_content_type (response_headers,
                                         "text/xml", params);

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
    {
      /* rather than warning, answer that the method is not implemented */
      g_debug ("unsupported method %s", method);
      status = SOUP_STATUS_NOT_IMPLEMENTED;
    }

  soup_server_message_set_status (msg, status, NULL);

  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
  if (err)
    g_warning ("error: %s", err->message);
}

/**
 * phodav_server_get_soup_server:
 * @server: a %PhodavServer
 *
 * Returns the underlying %SoupServer, if any.
 *
 * Returns: (transfer none) (nullable): the associated %SoupServer or %NULL
 **/
SoupServer *
phodav_server_get_soup_server (PhodavServer *self)
{
  g_return_val_if_fail (PHODAV_IS_SERVER (self), NULL);

  return self->server;
}

/**
 * phodav_server_new:
 * @root: (nullable): Root path.
 *
 * Creates a new #PhodavServer.
 * This is equivalent to constructing a #GFile from @root
 * and using phodav_server_new_for_root_file().
 *
 * Returns: (transfer full): a new #PhodavServer
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
