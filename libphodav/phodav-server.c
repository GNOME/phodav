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
 * The DAV methods run on worker threads, outside the main context
 * the #SoupServer is listened from: the request headers are copied and
 * the body is referenced first, and the response is applied back on
 * the main context when the method returns. The methods share a
 * private, bounded worker pool; a reader-writer lock protects the
 * filesystem decision points, while the long operations (COPY, a
 * recursive DELETE, syncing an upload) run outside it. The streaming
 * responses pull their chunks on demand from a separate pool instead
 * of holding a worker for the whole transfer.
 *
 * A #PhodavServer must still be created and disposed on the thread
 * running the main context of its #SoupServer.
 */

G_DEFINE_FINAL_TYPE (PhodavServer, phodav_server, G_TYPE_OBJECT)

/* Properties */
typedef enum
{
  PROP_ROOT = 1,
  PROP_ROOT_FILE,
  PROP_BASE_PATH,
  PROP_SERVER,
  PROP_READONLY,
  PROP_MAX_UPLOAD,
} PhodavServerProps;

static GParamSpec *properties[PROP_MAX_UPLOAD + 1];

/* how often to look for expired locks; they are also expired lazily
 * when they are used, this only reclaims the memory */
#define LOCK_EXPIRY_INTERVAL 1

/* the DAV methods are filesystem-bound: a handful of workers is enough
 * to make progress without letting requests pile up on the process
 * wide pool shared with the embedding application. The stream pulls
 * get their own pool of that size, so a download does not wait for
 * request workers blocked on the filesystem lock. */
#define PHODAV_WORKER_THREADS MAX (2, MIN (g_get_num_processors (), 8))

static gboolean expire_locks_timeout (gpointer user_data);

typedef struct _PhodavJob
{
  PhodavServer *self;
  PhodavTaskFunc run;
  PhodavTaskFunc done;
  gpointer data;
} PhodavJob;

static gboolean
phodav_job_done (gpointer data)
{
  PhodavJob *job = data;

  job->done (job->data);
  g_object_unref (job->self);
  g_free (job);

  return G_SOURCE_REMOVE;
}

static void
phodav_job_run (gpointer data, gpointer user_data)
{
  PhodavJob *job = data;

  job->run (job->data);

  g_main_context_invoke_full (job->self->expire_locks_context,
                              G_PRIORITY_DEFAULT,
                              phodav_job_done, job, NULL);
}

static void
phodav_server_dispatch_to (PhodavServer *self,
                           GThreadPool *pool,
                           PhodavTaskFunc run,
                           PhodavTaskFunc done,
                           gpointer data)
{
  PhodavJob *job = g_new0 (PhodavJob, 1);
  g_autoptr(GError) error = NULL;

  job->self = g_object_ref (self);
  job->run = run;
  job->done = done;
  job->data = data;

  if (pool && g_thread_pool_push (pool, job, &error))
    return;

  if (error)
    g_debug ("cannot queue the worker task: %s", error->message);

  /* the server was stopped: still run the completion on the main
   * context, so that the caller releases what it handed over */
  g_main_context_invoke_full (self->expire_locks_context,
                              G_PRIORITY_DEFAULT,
                              phodav_job_done, job, NULL);
}

void
phodav_server_dispatch (PhodavServer *self,
                        PhodavTaskFunc run,
                        PhodavTaskFunc done,
                        gpointer data)
{
  phodav_server_dispatch_to (self, self->pool, run, done, data);
}

void
phodav_server_dispatch_stream (PhodavServer *self,
                               PhodavTaskFunc run,
                               PhodavTaskFunc done,
                               gpointer data)
{
  phodav_server_dispatch_to (self, self->stream_pool, run, done, data);
}

void
phodav_server_read_lock (PhodavServer *self)
{
  g_rw_lock_reader_lock (&self->fs_lock);
}

void
phodav_server_read_unlock (PhodavServer *self)
{
  g_rw_lock_reader_unlock (&self->fs_lock);
}

void
phodav_server_write_lock (PhodavServer *self)
{
  g_rw_lock_writer_lock (&self->fs_lock);
}

void
phodav_server_write_unlock (PhodavServer *self)
{
  g_rw_lock_writer_unlock (&self->fs_lock);
}

PhodavServer *
handler_get_server (PathHandler *handler)
{
  return handler->self;
}

/* does @path belong to the share mounted at the base path? This is
 * the same prefix test the DAV handlers are registered with: a
 * request outside it is not phodav's, whatever handler libsoup
 * routed it to. */
gboolean
handler_path_is_covered (PathHandler *handler, const gchar *path)
{
  return path && path_is_covered (path, handler->base_path);
}

/* the share-relative path of @path, borrowed: the base path is
 * stripped, so that a share mounted at "/dav" maps "/dav/hello.txt"
 * to "/hello.txt". Returns NULL when @path is not part of the share. */
const gchar *
handler_get_relative_path (PathHandler *handler, const gchar *path)
{
  const gchar *relative = path;
  gsize base_len;

  g_return_val_if_fail (path != NULL && *path == '/', NULL);

  if (!handler_path_is_covered (handler, path))
    return NULL;

  base_len = strlen (handler->base_path);
  if (base_len > 1)
    {
      relative = path + base_len;
      if (!*relative)
        relative = "/";
    }

  return relative;
}

GFile *
handler_get_root_file (PathHandler *handler)
{
  return handler->file;
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
  /* the base path may change on the main thread, but this handler is
   * replaced with its registration at the same time: keep an
   * immutable snapshot for the requests already routed to it */
  h->base_path = g_strdup (self->base_path);
  h->ref_count = 1;
  return h;
}

PathHandler *
path_handler_ref (PathHandler *h)
{
  g_atomic_int_inc (&h->ref_count);

  return h;
}

void
path_handler_unref (PathHandler *h)
{
  if (!g_atomic_int_dec_and_test (&h->ref_count))
    return;

  g_object_unref (h->file);
  g_free (h->base_path);
  g_free (h);
}

static void
phodav_server_init (PhodavServer *self)
{
  /* the DAV methods build and parse XML on worker threads */
  xmlInitParser ();

  self->constructing = TRUE;
  self->base_path = g_strdup ("/");

  self->cancellable = g_cancellable_new ();
  g_mutex_init (&self->lock_store_mutex);
  g_rw_lock_init (&self->fs_lock);

  self->paths = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       NULL, (GDestroyNotify) path_unref);

  /* the context this server is created on; the lock expiry sweep
   * runs there, like the rest of the server */
  self->expire_locks_context = g_main_context_ref (g_main_context_get_thread_default () ?: g_main_context_default ());
}

static gboolean
expire_locks_timeout (gpointer user_data)
{
  phodav_lock_store_expire_locks (PHODAV_SERVER (user_data));

  return G_SOURCE_CONTINUE;
}

static void
update_root_handler (PhodavServer *self)
{
  PathHandler *handler;

  /* stop() unregistered the handler for good: property changes must
   * not bring it back */
  if (self->stopped)
    return;

  /* the handler is registered at the base path, which may have
   * changed since the last registration */
  if (self->root_handler)
    {
      if (self->server && self->handler_path)
        soup_server_remove_handler (self->server, self->handler_path);

      self->root_handler = NULL;
      g_clear_pointer (&self->handler_path, g_free);
    }

  if (!self->root_file || !self->server)
    return;

  handler = path_handler_new (self, self->root_file);

  soup_server_add_handler (self->server, self->base_path,
                           phodav_server_callback,
                           handler,
                           (GDestroyNotify) path_handler_unref);

  self->root_handler = handler;
  self->handler_path = g_strdup (self->base_path);
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

  self->pool = g_thread_pool_new (phodav_job_run, NULL,
                                  PHODAV_WORKER_THREADS,
                                  TRUE, NULL);
  self->stream_pool = g_thread_pool_new (phodav_job_run, NULL,
                                         PHODAV_WORKER_THREADS,
                                         TRUE, NULL);

  g_signal_connect (self->server, "request-started", G_CALLBACK (phodav_server_request_started), self);

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
server_stop (PhodavServer *self)
{
  if (self->stopped)
    return;

  self->stopped = TRUE;

  /* first stop accepting new requests, then cancel the ones which are
   * already running; they keep their references until they complete */
  if (self->server)
    {
      if (self->handler_path)
        soup_server_remove_handler (self->server, self->handler_path);
      g_signal_handlers_disconnect_by_func (self->server, phodav_server_request_started, self);
    }
  self->root_handler = NULL;
  g_clear_pointer (&self->handler_path, g_free);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  g_clear_pointer (&self->expire_locks_source, g_source_destroy);

  /* let the tasks already queued run: they observe the cancellation
   * and release their references; dropping them here would leak their
   * data */
  if (self->pool)
    {
      g_thread_pool_free (self->pool, FALSE, FALSE);
      self->pool = NULL;
    }
  if (self->stream_pool)
    {
      g_thread_pool_free (self->stream_pool, FALSE, FALSE);
      self->stream_pool = NULL;
    }
}

static void
phodav_server_dispose (GObject *gobject)
{
  PhodavServer *self = PHODAV_SERVER (gobject);

  server_stop (self);
  g_clear_pointer (&self->expire_locks_context, g_main_context_unref);

  /* SoupServer could live longer than PhodavServer,
   * this frees the PhodavHandler passed as user_data;
   * dispose may run again, the server is already gone then */
  g_clear_pointer (&self->base_path, g_free);
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
phodav_server_finalize (GObject *gobject)
{
  PhodavServer *self = PHODAV_SERVER (gobject);

  g_rw_lock_clear (&self->fs_lock);
  g_mutex_clear (&self->lock_store_mutex);

  /* Chain up to the parent class */
  if (G_OBJECT_CLASS (phodav_server_parent_class)->finalize)
    G_OBJECT_CLASS (phodav_server_parent_class)->finalize (gobject);
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

    case PROP_BASE_PATH:
      g_value_set_string (value, self->base_path);
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

    case PROP_BASE_PATH:
      {
        const gchar *path = g_value_get_string (value);
        g_autofree gchar *canonical = phodav_path_canonicalize (path && *path ? path : "/");

        if (!phodav_path_is_safe (canonical))
          {
            g_warning ("invalid base path: %s", path ?: "");
            break;
          }

        if (g_strcmp0 (canonical, self->base_path) == 0)
          break;

        g_free (self->base_path);
        self->base_path = g_steal_pointer (&canonical);
        update_root_handler (self);
      }
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
      update_root_handler (self);
      break;
    }
}

static void
phodav_server_class_init (PhodavServerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = phodav_server_dispose;
  gobject_class->finalize = phodav_server_finalize;
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
   * PhodavServer:base-path:
   *
   * The path prefix the DAV handlers are registered at on the #SoupServer.
   *
   * The default, "/", serves the share at the server root. When the
   * server is shared with other handlers, set a distinct prefix (for
   * example "/dav") that no other handler covers: phodav filters the
   * requests it processes by that prefix, as libsoup does not expose
   * the handler routing before the request body is received.
   *
   * Since: 3.1
   **/
  properties[PROP_BASE_PATH] =
    g_param_spec_string ("base-path",
                         NULL, NULL,
                         "/",
                         G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS);

  /**
   * PhodavServer:server:
   *
   * The underlying #SoupServer.
   *
   * When constructing a #PhodavServer, an already configured
   * #SoupServer may be given, with listeners, TLS certificates or
   * auth domains set up for example. When no server is provided,
   * a new one is created.
   *
   * phodav registers its DAV handlers at #PhodavServer:base-path.
   * When the server carries handlers of the embedding application,
   * set #PhodavServer:base-path to a prefix that no other handler
   * covers, and leave that subtree to phodav.
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
 * phodav_server_stop:
 * @server: a #PhodavServer
 *
 * Stops serving DAV requests.
 *
 * The DAV handler is unregistered from the #SoupServer, the requests
 * already in progress are cancelled, and the lock expiry sweep is
 * stopped. The references held by the cancelled requests keep the
 * server alive until they complete, but no new request is accepted.
 *
 * Unlike disposing the server, this does not wait for the outstanding
 * requests, so it can be called from the main context while the server
 * is busy. It must be called on the thread running the main context of
 * the #SoupServer. A stopped server cannot be restarted.
 *
 * Since: 3.1
 **/
void
phodav_server_stop (PhodavServer *self)
{
  g_return_if_fail (PHODAV_IS_SERVER (self));

  server_stop (self);
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
