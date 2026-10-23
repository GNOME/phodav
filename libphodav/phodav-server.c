/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
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
  PathHandler *root_handler; /* borrowed, owned by the soup handler registration */
  GHashTable *paths;
  gboolean readonly;
  guint64 max_upload;
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
  PROP_MAX_UPLOAD,
} PhodavServerProps;

static GParamSpec *properties[PROP_MAX_UPLOAD + 1];

static void early_server_callback (SoupServer *server,
                                   SoupServerMessage *msg,
                                   const gchar *path,
                                   GHashTable *query,
                                   gpointer user_data);

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

/* The lock table is keyed on the canonical resource path, so that the
 * locks are found again under whatever spelling of the path the client
 * used in its request. server_foreach_parent_path() walks the same
 * canonical keys, so the two cannot disagree on the canonical form. */
Path *
server_get_path (PhodavServer *self, const gchar *_path)
{
  Path *p;
  g_autofree gchar *path = phodav_path_canonicalize (_path);

  p = g_hash_table_lookup (self->paths, path);
  if (!p)
    {
      p = g_new0 (Path, 1);
      p->path = g_steal_pointer (&path);
      g_hash_table_insert (self->paths, p->path, path_ref (p));
    }

  return p;
}

/* drop the path from the lock table when it holds no lock anymore;
 * called when a LOCK request is rolled back, so that a failed request
 * does not leave an empty entry behind */
void
server_remove_path_if_empty (PhodavServer *self, const gchar *_path)
{
  g_autofree gchar *path = phodav_path_canonicalize (_path);
  Path *p = g_hash_table_lookup (self->paths, path);

  if (!p)
    return;

  path_expire_locks (p);
  if (!p->locks)
    g_hash_table_remove (self->paths, path);
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

/* the request paths begin with a "/" and are relative to the share
 * root, so that the file is looked up under the root, not the
 * filesystem root */
GFile *
handler_get_file_for_path (PathHandler *handler, const gchar *path)
{
  g_return_val_if_fail (path != NULL && *path == '/', NULL);

  return g_file_get_child (handler->file, path + 1);
}

GFile *
handler_get_root_file (PathHandler *handler)
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

guint64
handler_get_max_upload (PathHandler *handler)
{
  return handler->self->max_upload;
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

/* destroy the locks rooted on a deleted resource and its members, and
 * drop the paths which are no longer needed */
void
server_remove_locks (PhodavServer *self, const gchar *_path)
{
  GHashTableIter iter;
  Path *path;
  g_autofree gchar *root = phodav_path_canonicalize (_path);

  if (g_hash_table_size (self->paths) == 0)
    return;

  g_hash_table_iter_init (&iter, self->paths);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &path))
    {
      if (!path_is_covered (path->path, root))
        continue;

      /* the locks remove themselves from the list when freed */
      while (path->locks)
        dav_lock_free (path->locks->data);

      /* the path is only kept alive by the table when it has no locks */
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

  /* the early handler only tags the messages which libsoup routes
   * to the phodav root handler, like the regular one below: a
   * request routed to another handler of a user-provided
   * #SoupServer, at a more specific path, runs neither of them */
  soup_server_add_early_handler (self->server, "/",
                                 early_server_callback,
                                 NULL, NULL);

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

  /* the root handlers are registered as soon as the root and the
   * server are both known; the registration in set_property() may
   * have happened already, registering again only replaces them */
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

    case PROP_MAX_UPLOAD:
      g_value_set_uint64 (value, self->max_upload);
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

    case PROP_MAX_UPLOAD:
      self->max_upload = g_value_get_uint64 (value);
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
   * auth domains set up for example. phodav registers its DAV
   * handlers at "/" only: the requests routed to another handler
   * of @server, at a more specific path, are left to it, except
   * the authenticated ones on a server with an auth domain, which
   * phodav cannot tell the routing of. When no server is provided,
   * a new one is created.
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

  /**
   * PhodavServer:max-upload:
   *
   * The maximum size of a PUT request body, in bytes. An upload
   * which would be larger is refused with 413 Request Entity Too
   * Large, and the file it was writing is discarded rather than
   * left truncated. The default, 0, does not limit the uploads.
   *
   * Since: 3.1
   **/
  properties[PROP_MAX_UPLOAD] =
    g_param_spec_uint64 ("max-upload",
                         NULL, NULL,
                         0, G_MAXUINT64, 0,
                         G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (properties), properties);
}

gboolean
server_foreach_parent_path (PhodavServer *self, const gchar *path, PathCb cb, gpointer data)
{
  g_autofree gchar *canonical = phodav_path_canonicalize (path);
  gboolean ret = FALSE;
  gchar *key = NULL;
  Path *p;
  const gchar *segment = canonical + 1;

  /* canonical is "/" or "/segment[/segment...]"; visit each prefix
   * from the first segment down to the whole path */
  while (*segment)
    {
      const gchar *slash = strchr (segment, '/');
      gsize len = slash ? (gsize) (slash - canonical) : strlen (canonical);
      g_autofree gchar *partial = g_strndup (canonical, len);

      if (g_hash_table_lookup_extended (self->paths, partial,
                                        (gpointer *) &key, (gpointer *) &p))
        {
          if (!cb (key, p, data))
            goto end;
        }

      if (!slash)
        break;

      segment = slash + 1;
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

/* look for the lock with @token on @path or any of its parents */
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
    g_debug ("Invalid lock token %s for %s", token, path);

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
  g_autofree gchar *path = phodav_path_canonicalize (_path);
  TreeLockCheck d = { .path = path, .lock = lock, .conflict = FALSE };

  g_hash_table_foreach (self->paths, tree_check_lock_cb, &d);

  return d.conflict;
}

typedef struct _DavMethod DavMethod;

/* the DAV methods handled by the server, in the order of the Allow
 * header of the OPTIONS answer.
 *
 * The mutating methods, which modify the shared files, are answered
 * with FORBIDDEN when the server is read-only. UNLOCK is not one of
 * them: it only releases a lock, and does not modify the files.
 *
 * A method with a headers_handler streams its request body out
 * while it is received: it is started from got_headers(), before
 * its body arrives, and it is answered from its got-body callback,
 * once the file it writes is committed (PUT answers its success
 * status only then). It has no handler: server_callback() runs
 * none for it either. It is listed like the other methods, so that
 * the Allow header covers it. */
struct _DavMethod
{
  const gchar *name;
  gboolean mutating;
  gint (*handler) (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
  void (*headers_handler) (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
};

static const DavMethod dav_methods[] = {
  { "GET", FALSE, phodav_method_get, NULL },
  { "HEAD", FALSE, phodav_method_get, NULL },
  { "PUT", TRUE, NULL, phodav_method_put },
  { "PROPFIND", FALSE, phodav_method_propfind, NULL },
  { "PROPPATCH", TRUE, phodav_method_proppatch, NULL },
  { "MKCOL", TRUE, phodav_method_mkcol, NULL },
  { "DELETE", TRUE, phodav_method_delete, NULL },
  { "MOVE", TRUE, phodav_method_movecopy, NULL },
  { "COPY", TRUE, phodav_method_movecopy, NULL },
  { "LOCK", TRUE, phodav_method_lock, NULL },
  { "UNLOCK", FALSE, phodav_method_unlock, NULL },
};

static const DavMethod *
dav_method_lookup (const gchar *method)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (dav_methods); i++)
    if (g_strcmp0 (method, dav_methods[i].name) == 0)
      return &dav_methods[i];

  return NULL;
}

/* the comma separated names of the methods, for the Allow header */
static gchar *
dav_methods_allow (void)
{
  g_autoptr(GString) allow = g_string_sized_new (128);
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (dav_methods); i++)
    {
      if (i > 0)
        g_string_append (allow, ", ");
      g_string_append (allow, dav_methods[i].name);
    }

  return g_string_free_and_steal (g_steal_pointer (&allow));
}

/* the DAV requests operate on the resources under the share root:
 * their path must be absolute (it begins with a "/"), but relative
 * to the root (no second "/" may follow the first one, or the path
 * would read as absolute on Windows too), carry no dot-dot segment,
 * and their URI must carry no fragment. The dot segments are
 * removed when the request URI is parsed, and the ones surviving
 * the path decoding are refused by libsoup: checking them here too
 * guards the traversal on its own, should the URI handling change.
 * A request which does not qualify is answered with 400 BAD
 * REQUEST. */
static gboolean
request_path_is_valid (SoupServerMessage *msg, const gchar *path)
{
  GUri *uri = soup_server_message_get_uri (msg);

  if (!phodav_path_is_safe (path))
    g_debug ("invalid request path: %s", path ?: "");
  else if (!(uri && g_uri_get_fragment (uri) == NULL))
    g_debug ("using fragments in query is not supported");
  else
    return TRUE;

  soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
  return FALSE;
}

/* a body which is not delimited by a Content-Length cannot be
 * checked before it is received: refuse it as soon as it grows over
 * the parse limit, so that a client cannot make the server
 * accumulate an unbounded request body in memory */
static void
got_chunk (SoupServerMessage *msg,
           GBytes *chunk,
           gpointer user_data)
{
  SoupMessageBody *body = soup_server_message_get_request_body (msg);

  /* the accumulated length includes the chunk being signaled, the
   * body is appended before the signal is emitted */
  if (body->length <= PHODAV_MAX_REQUEST_BODY)
    return;

  g_debug ("refusing the oversized request body");

  g_signal_handlers_disconnect_by_func (msg, got_chunk, user_data);

  /* stop accumulating the rest of the body: it is drained, and the
   * request is answered with 413 rather than parsed */
  soup_message_body_set_accumulate (body, FALSE);
  soup_server_message_set_status (msg, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, NULL);
}

/* the qdata key of the messages routed to the phodav root handler,
 * set by its early handler */
#define EARLY_HANDLER_KEY "phodav-early-handler"

/* the early handler of the root path: it does no processing of its
 * own, it only tags the message, so that got_headers() can tell
 * the requests which libsoup will dispatch to the phodav root
 * handler from the ones routed to another handler of a
 * user-provided #SoupServer, at a more specific path. */
static void
early_server_callback (SoupServer *server, SoupServerMessage *msg, const gchar *path, GHashTable *query, gpointer user_data)
{
  g_object_set_qdata (G_OBJECT (msg), g_quark_from_static_string (EARLY_HANDLER_KEY), GUINT_TO_POINTER (1));
}

static void
got_headers (SoupServerMessage *msg,
             gpointer user_data)
{
  PhodavServer *self = user_data;
  GUri *uri = soup_server_message_get_uri (msg);
  const gchar *path = g_uri_get_path (uri);
  const gchar *method = soup_server_message_get_method (msg);
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  const DavMethod *m = dav_method_lookup (method);

  /* the requests already answered (an authentication challenge, an
   * invalid path...) are not phodav's to handle */
  if (soup_server_message_get_status (msg) != 0)
    return;

  /* without a shared root, no handler is registered: the requests
   * are answered by libsoup, phodav has nothing to do with them */
  if (!self->root_handler)
    return;

  /* the early handler tagged the requests which libsoup routes to
   * the phodav root handler. It never runs for the requests
   * accepted by an auth domain though: libsoup returns from its
   * headers processing on a successful authentication, before
   * calling the early handlers. Handle those here too, told by the
   * Authorization header the client sent: on a server with an
   * auth domain, the routing of an authenticated request cannot be
   * told apart, and phodav handles them all */
  if (g_object_get_qdata (G_OBJECT (msg), g_quark_from_static_string (EARLY_HANDLER_KEY)) == NULL &&
      soup_message_headers_get_one (request_headers, "Authorization") == NULL)
    return;

  /* a method which streams its request body out while it is
   * received (PUT) is started from here, before the body arrives:
   * it is answered from its got-body callback, not by
   * server_callback() */
  if (m && m->headers_handler)
    {
      g_autoptr(GError) err = NULL;

      /* the request is validated like server_callback() validates
       * the ones of the other methods; a refused client may still
       * be sending its body: have it drained, not accumulated */
      if (!request_path_is_valid (msg, path))
        {
          soup_message_body_set_accumulate (soup_server_message_get_request_body (msg), FALSE);
          return;
        }

      /* a read-only server refuses the mutating methods before
       * their upload is started, like server_callback() does for
       * the other ones */
      if (self->readonly && m->mutating)
        {
          soup_message_body_set_accumulate (soup_server_message_get_request_body (msg), FALSE);
          soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN, NULL);
          return;
        }

      m->headers_handler (self->root_handler, msg, path, &err);

      if (err)
        g_debug ("%s %s: %s", method, path, err->message);

      return;
    }

  /* the other methods have their whole request body parsed in
   * memory: refuse an unreasonable Content-Length before the body
   * is received (RFC 9110 section 15.5.15), and drain it without
   * accumulating it */
  if (soup_message_headers_get_content_length (request_headers) > PHODAV_MAX_REQUEST_BODY)
    {
      g_debug ("refusing the oversized request body");
      soup_message_body_set_accumulate (soup_server_message_get_request_body (msg), FALSE);
      soup_server_message_set_status (msg, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, NULL);
      return;
    }

  /* a chunked body has no Content-Length to check in advance: watch
   * it while it is received */
  if (soup_message_headers_get_encoding (request_headers) == SOUP_ENCODING_CHUNKED)
    g_signal_connect (msg, "got-chunk", G_CALLBACK (got_chunk), self);
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

  const gchar *method = soup_server_message_get_method (msg);
  const DavMethod *m = dav_method_lookup (method);
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  SoupMessageHeaders *response_headers;
  g_autoptr(GHashTable) params = NULL;

  g_debug ("%s %s HTTP/1.%d %s %s", method, path, soup_server_message_get_http_version (msg),
           soup_message_headers_get_one (request_headers, "X-Litmus") ?: "",
           soup_message_headers_get_one (request_headers, "X-Litmus-Second") ?: "");

  /* got_headers() may have already rejected the request (oversized
   * body, or a streamed method refused before its upload was
   * started); do not overwrite its status with the result of the
   * method handler */
  if (soup_server_message_get_status (msg) != 0)
    return;

  /* a streamed method was started from got_headers(), before its
   * request body was received, and is answered from its got-body
   * callback, once the file it writes is committed: there is no
   * handler to run for it here, and its answer must not be
   * overwritten */
  if (m && m->headers_handler)
    return;

  if (!request_path_is_valid (msg, path))
    return;

  params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (params, g_strdup ("charset"), g_strdup ("utf-8"));
  response_headers = soup_server_message_get_response_headers (msg);
  soup_message_headers_set_content_type (response_headers,
                                         "text/xml", params);

  if (method == SOUP_METHOD_OPTIONS)
    {
      g_autofree gchar *allow = dav_methods_allow ();

      soup_message_headers_append (response_headers, "DAV", "1,2");

      /* according to http://code.google.com/p/sabredav/wiki/Windows */
      soup_message_headers_append (response_headers, "MS-Author-Via", "DAV");

      soup_message_headers_append (response_headers, "Allow", allow);
      status = SOUP_STATUS_OK;
    }
  else
    {
      if (m == NULL)
        {
          /* rather than warning, answer that the method is not implemented */
          g_debug ("unsupported method %s", method);
        }
      else if (handler_get_readonly (handler) && m->mutating)
        status = SOUP_STATUS_FORBIDDEN;
      else
        status = m->handler (handler, msg, path, &err);
    }

  soup_server_message_set_status (msg, status, NULL);

  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
  if (err)
    g_debug ("%s %s: %s", method, path, err->message);
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
