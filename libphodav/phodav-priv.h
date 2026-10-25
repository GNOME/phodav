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
typedef struct _PhodavPath PhodavPath;
typedef struct _PhodavRequest PhodavRequest;
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

typedef enum
{
  PHODAV_PATH_EXISTING,
  PHODAV_PATH_CREATE,
} PhodavPathMode;

/* the single filesystem policy entry point: it rejects symlinked
 * components, enforces the requested existence mode, and maps any
 * failure to an HTTP status in @status. The returned handle keeps the
 * resolved object open, so later operations can use it instead of
 * looking the path up again. */
PhodavPath *phodav_path_resolve (PathHandler *handler, const gchar *path, PhodavPathMode mode, gint *status);
PhodavPath *phodav_path_ref (PhodavPath *path);
void phodav_path_unref (PhodavPath *path);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PhodavPath, phodav_path_unref)
/* the object to read or query: the held descriptor when available */
GFile *phodav_path_get_file (PhodavPath *path) G_GNUC_PURE;
/* the object to modify by name: its parent directory is held open too */
GFile *phodav_path_get_named_file (PhodavPath *path) G_GNUC_PURE;
const gchar *phodav_path_get_name (PhodavPath *path) G_GNUC_PURE;
int phodav_path_get_fd (PhodavPath *path) G_GNUC_PURE;
gint phodav_status_from_error (const GError *error);
gboolean phodav_path_sync (GFile *file, GCancellable *cancellable, GError **error);
/* rename a staged GFile without syncing: callers sync it first, outside
 * the filesystem write lock */
gboolean phodav_path_rename_files (GFile *from, GFile *to, GCancellable *cancellable, GError **error);

/* the descriptor-based operations layer: every lookup below is done
 * relative to a held handle, never by rebuilding an absolute path */
PhodavPath *phodav_path_resolve_child (PhodavPath *parent, const gchar *name, PhodavPathMode mode, gint *status);
GFileInfo *phodav_path_query_info (PhodavPath *path, const gchar *attributes, GFileQueryInfoFlags flags, GCancellable *cancellable, GError **error);
GFileInputStream *phodav_path_open (PhodavPath *path, GCancellable *cancellable, GError **error);
GFileEnumerator *phodav_path_enumerate (PhodavPath *path, const gchar *attributes, GFileQueryInfoFlags flags, GCancellable *cancellable, GError **error);
gboolean phodav_path_mkdir (PhodavPath *path, GCancellable *cancellable, GError **error);
gboolean phodav_path_unlink (PhodavPath *path, GCancellable *cancellable, GError **error);
gboolean phodav_path_unlink_child (PhodavPath *parent, const gchar *name, GCancellable *cancellable, GError **error);
gboolean phodav_path_copy (PhodavPath *from, PhodavPath *to, GFileCopyFlags flags, gboolean recursive, GCancellable *cancellable, GError **error);
gboolean phodav_path_rename (PhodavPath *from, PhodavPath *to, GCancellable *cancellable, GError **error);
gboolean phodav_path_move (PhodavPath *from, PhodavPath *to, GFileCopyFlags flags, GCancellable *cancellable, GError **error);
/* remove @path and its members; @notify receives the relative path and
 * status of every entry which could not be removed */
typedef void (*PhodavPathDeleteNotify) (const gchar *path, gint status, gpointer data);
gint phodav_path_delete_tree (PhodavServer *server, PhodavPath *path, const gchar *rel_path, PhodavPathDeleteNotify notify, gpointer data, GCancellable *cancellable);

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

/* the private worker pools: the request dispatch and the PUT commit
 * use the request pool, the streaming producers have their own so that
 * they are not queued behind requests waiting for the filesystem lock.
 * @done runs on the server context and owns @data */
typedef void (*PhodavTaskFunc) (gpointer data);
void phodav_server_dispatch (PhodavServer *self,
                             PhodavTaskFunc run,
                             PhodavTaskFunc done,
                             gpointer data);
void phodav_server_dispatch_stream (PhodavServer *self,
                                    PhodavTaskFunc run,
                                    PhodavTaskFunc done,
                                    gpointer data);

/* the single filesystem lock: mutations take it for writing, readers
 * share it */
void phodav_server_read_lock (PhodavServer *self);
void phodav_server_read_unlock (PhodavServer *self);
void phodav_server_write_lock (PhodavServer *self);
void phodav_server_write_unlock (PhodavServer *self);

void phodav_lock_store_expire_locks (PhodavServer *self);
void phodav_lock_store_remove_locks (PhodavServer *self,
                                     const gchar *path);

gint phodav_check_if (PhodavRequest *req, GList **locks);
gint phodav_check_if_refresh (PhodavRequest *req, GList **locks);

/* per-request state: the request headers are copied and the request
 * body is referenced before the method runs outside the main loop.
 * The body stays shared with the message, which is safe once it is
 * complete and the message is paused. The response is applied back on
 * the main loop once the method returns. */
PhodavRequest *phodav_request_new (PhodavServer *self,
                                   PathHandler *handler,
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
guint64 phodav_request_get_max_upload (PhodavRequest *req) G_GNUC_PURE;
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

/* incremental response bodies: the main context pulls one chunk at a
 * time from @pull, which runs on the stream pool. Returning NULL
 * without an error ends the body; an error aborts it. */
typedef GBytes *(*PhodavStreamPull) (gpointer data,
                                     GCancellable *cancellable,
                                     GError **error);
PhodavStream *phodav_request_stream_start (PhodavRequest *req,
                                           guint status,
                                           const gchar *content_type,
                                           PhodavStreamPull pull,
                                           gpointer pull_data,
                                           GDestroyNotify pull_data_free);
/* cut the connection so that a truncated body cannot be mistaken for
 * the complete representation */
void phodav_stream_abort (PhodavStream *stream);
/* the value-returning lock service */
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
