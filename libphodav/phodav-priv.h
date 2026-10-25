/*
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include "config.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "phodav-server.h"

G_BEGIN_DECLS

/* the DAV methods other than PUT parse their whole request body in
 * memory; larger bodies are refused rather than risking to exhaust
 * the memory of the process (PUT streams to disk and is unaffected) */
#define PHODAV_MAX_REQUEST_BODY (1024 * 1024)

typedef struct _DAVLock DAVLock;
typedef struct _Path Path;
typedef struct _PathHandler PathHandler;
typedef struct _PhodavRequest PhodavRequest;
typedef struct _PhodavLockGuard PhodavLockGuard;
typedef struct _PhodavStream PhodavStream;

typedef enum _DAVLockScopeType
{
  DAV_LOCK_SCOPE_NONE,
  DAV_LOCK_SCOPE_EXCLUSIVE,
  DAV_LOCK_SCOPE_SHARED,
} DAVLockScopeType;

typedef enum _DAVLockType
{
  DAV_LOCK_NONE,
  DAV_LOCK_WRITE,
} DAVLockType;

typedef enum _DepthType
{
  DEPTH_ZERO,
  DEPTH_ONE,
  DEPTH_INFINITY
} DepthType;

typedef enum _PropFindType
{
  PROPFIND_ALLPROP,
  PROPFIND_PROPNAME,
  PROPFIND_PROP
} PropFindType;

/* the lock tokens are "urn:uuid:" (9 characters) followed by a
 * 36 characters UUID */
#define DAV_LOCK_TOKEN_LENGTH 45

struct _DAVLock
{
  Path *path;
  gchar token[DAV_LOCK_TOKEN_LENGTH + 1];
  DAVLockScopeType scope;
  DAVLockType type;
  DepthType depth;
  xmlNodePtr owner;
  guint64 timeout;
};

/* the state of a PhodavServer, shared by the server, lock store and
 * request dispatch implementations; private to libphodav */
struct _PhodavServer
{
  GObject parent;
  SoupServer *server;
  GCancellable *cancellable;
  GFile *root_file;
  gchar *base_path;
  PathHandler *root_handler; /* borrowed, owned by the soup handler registration */
  gchar *handler_path;       /* the path the handler is registered at */
  GHashTable *paths;
  gboolean readonly;
  guint64 max_upload;
  gboolean constructing;
  GMainContext *expire_locks_context;
  GSource *expire_locks_source;

  /* protects the lock table during a lookup or update; recursive
   * because the lock helpers call each other. It never covers a
   * filesystem operation: the value-returning lock store API only
   * exposes owned decisions and XML nodes. */
  GRecMutex lock_store_mutex;
  /* per-path operation reservations. They replace the former global
   * mutation mutex: disjoint resources can be modified concurrently,
   * while overlapping operations wait here instead of holding a
   * process-wide lock. */
  GMutex guard_mutex;
  GCond guard_cond;
  GList *guards;
};

struct _PathHandler
{
  PhodavServer *self; /* borrowed, kept alive by the request */
  GFile *file;
  gint ref_count;
};

typedef gboolean (*PathCb) (const gchar *key,
                            Path *path,
                            gpointer data);

/* the file of a request path, relative to the share root */
GFile *handler_get_file_for_path (PathHandler *handler, const gchar *path);
/* the share-relative path of @path, borrowed; NULL when @path is not
 * part of the share mounted at the base path */
const gchar *handler_get_relative_path (PathHandler *handler, const gchar *path) G_GNUC_PURE;
/* is @path part of the share mounted at the base path? */
gboolean handler_path_is_covered (PathHandler *handler, const gchar *path) G_GNUC_PURE;
/* the shared root, borrowed */
GFile *handler_get_root_file (PathHandler *handler) G_GNUC_PURE;
PhodavServer *handler_get_server (PathHandler *handler) G_GNUC_PURE;
PathHandler *path_handler_ref (PathHandler *handler);
void path_handler_unref (PathHandler *handler);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PathHandler, path_handler_unref)
guint64 handler_get_max_upload (PathHandler *handler) G_GNUC_PURE;

/* the SoupServer callbacks, registered and disconnected by the
 * server, implemented by the request dispatch */
void phodav_server_request_started (SoupServer *server,
                                    SoupServerMessage *message,
                                    gpointer user_data);
void phodav_server_callback (SoupServer *server,
                             SoupServerMessage *msg,
                             const gchar *path,
                             GHashTable *query,
                             gpointer user_data);

gboolean server_foreach_parent_path (PhodavServer *server,
                                     const gchar *path,
                                     PathCb cb,
                                     gpointer data);
DAVLock *server_path_get_lock (PhodavServer *server,
                               const gchar *path,
                               const gchar *token);
DAVLock *server_path_lookup_lock (PhodavServer *server,
                                  const gchar *path,
                                  const gchar *token);
gboolean server_path_has_other_locks (PhodavServer *self,
                                      const gchar *path,
                                      GList *locks);
gboolean server_tree_has_other_locks (PhodavServer *self,
                                      const gchar *path,
                                      GList *locks);
gboolean server_tree_has_lock_conflict (PhodavServer *self,
                                        const gchar *path,
                                        DAVLock *lock);
void server_expire_locks (PhodavServer *self);
void server_remove_locks (PhodavServer *self,
                          const gchar *_path);
Path *server_get_path (PhodavServer *self,
                       const gchar *_path);
void server_remove_path_if_empty (PhodavServer *self,
                                  const gchar *_path);

gint phodav_check_if (PhodavRequest *req, GList **locks);
gint phodav_check_if_refresh (PhodavRequest *req, GList **locks);

gint phodav_delete_file (PhodavServer *server, const gchar *path, GFile *file, GHashTable *mstatus, GCancellable *cancellable);

/* per-request state: the request headers are copied and the request
 * body is referenced before the method runs outside the main loop.
 * The body stays shared with the message, which is safe once it is
 * complete and the message is paused. The response is applied back on
 * the main loop once the method returns. */
PhodavRequest *phodav_request_new (PathHandler *handler,
                                   SoupServerMessage *msg,
                                   const gchar *path);
PhodavRequest *phodav_request_ref (PhodavRequest *req);
void phodav_request_unref (PhodavRequest *req);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PhodavRequest, phodav_request_unref)

PathHandler *phodav_request_get_handler (PhodavRequest *req) G_GNUC_PURE;
SoupServerMessage *phodav_request_get_message (PhodavRequest *req) G_GNUC_PURE;
const gchar *phodav_request_get_method (PhodavRequest *req) G_GNUC_PURE;
const gchar *phodav_request_get_path (PhodavRequest *req) G_GNUC_PURE;
SoupMessageHeaders *phodav_request_get_request_headers (PhodavRequest *req) G_GNUC_PURE;
SoupMessageBody *phodav_request_get_request_body (PhodavRequest *req) G_GNUC_PURE;
GUri *phodav_request_get_uri (PhodavRequest *req) G_GNUC_PURE;
GCancellable *phodav_request_get_cancellable (PhodavRequest *req) G_GNUC_PURE;
SoupMessageHeaders *phodav_request_get_response_headers (PhodavRequest *req) G_GNUC_PURE;
void phodav_request_set_status (PhodavRequest *req, guint status, const gchar *reason);
guint phodav_request_get_status (PhodavRequest *req) G_GNUC_PURE;
void phodav_request_set_response (PhodavRequest *req,
                                  const gchar *content_type,
                                  SoupMemoryUse use,
                                  const gchar *body,
                                  gsize len);
void phodav_request_set_body_bytes (PhodavRequest *req, GBytes *bytes);
gboolean phodav_request_is_finished (PhodavRequest *req) G_GNUC_PURE;
gboolean phodav_request_is_server_cancelled (PhodavRequest *req) G_GNUC_PURE;

/* incremental response bodies */
PhodavStream *phodav_request_stream_start (PhodavRequest *req, guint status, const gchar *content_type);
gboolean phodav_stream_write (PhodavStream *stream, const guint8 *data, gsize len);
void phodav_stream_end (PhodavStream *stream);
void phodav_stream_abort (PhodavStream *stream);
/* the value-returning lock service */
PhodavLockGuard *phodav_lock_store_guard_acquire (PhodavServer *self, GList *paths);
void phodav_lock_store_guard_release (PhodavLockGuard *guard);
gboolean phodav_lock_store_has_token (PhodavServer *self, const gchar *path, const gchar *token);
gboolean phodav_lock_store_path_has_other_locks (PhodavServer *self, const gchar *path, GList *locks);
gboolean phodav_lock_store_tree_has_other_locks (PhodavServer *self, const gchar *path, GList *locks);
gint phodav_lock_store_add_lock (PhodavServer *self, const gchar *path, const gchar *token, DAVLockScopeType scope, DAVLockType type, DepthType depth, const xmlNodePtr owner, guint timeout);
gint phodav_lock_store_refresh_lock (PhodavServer *self, const gchar *path, GList *submitted, guint timeout, gchar **refreshed_token);
xmlNodePtr phodav_lock_store_lock_node (PhodavServer *self, const gchar *path, const gchar *token, xmlNsPtr ns);
gint phodav_lock_store_remove_lock (PhodavServer *self, const gchar *path, const gchar *token);
void phodav_lock_store_lockdiscovery (PhodavServer *self, const gchar *path, xmlNodePtr node);

gint phodav_method_get (PhodavRequest *req, GError **err);
gint phodav_method_propfind (PhodavRequest *req, GError **err);
gint phodav_method_proppatch (PhodavRequest *req, GError **err);
gint phodav_method_mkcol (PhodavRequest *req, GError **err);
gint phodav_method_delete (PhodavRequest *req, GError **err);
gint phodav_method_movecopy (PhodavRequest *req, GError **err);
gint phodav_method_lock (PhodavRequest *req, GError **err);
gint phodav_method_unlock (PhodavRequest *req, GError **err);
void phodav_method_put (PhodavRequest *req, GError **err);

G_END_DECLS
