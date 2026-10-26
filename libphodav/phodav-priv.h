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

/* the depth-1 PROPFIND responses are assembled in memory before they
 * are streamed: cap the number of resources they may list, so a huge
 * collection cannot exhaust the memory of the server */
#define PHODAV_MAX_PROPFIND_ENTRIES 4096

typedef struct _DAVLock DAVLock;
typedef struct _Path Path;
typedef struct _PathHandler PathHandler;
typedef struct _PhodavBackend PhodavBackend;
typedef struct _PhodavPath PhodavPath;
typedef struct _PhodavRequest PhodavRequest;
typedef struct _PhodavStream PhodavStream;
typedef struct _PhodavMethodOps PhodavMethodOps;

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
  /* position in the lock store expiry heap, -1 when not queued */
  gint expiry_index;
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
gint phodav_path_get_fd (PhodavPath *path) G_GNUC_PURE;
gint phodav_status_from_error (const GError *error);
gboolean phodav_path_sync (GFile *file, GCancellable *cancellable, GError **error);
/* rename a staged GFile without syncing: callers sync it first */
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
 * status of every entry which could not be removed, and @removed
 * collects the paths which were removed, so the caller can update the
 * lock store on the main context after the job returns */
typedef void (*PhodavPathDeleteNotify) (const gchar *path, gint status, gpointer data);
gint phodav_path_delete_tree (PhodavPath *path, const gchar *rel_path, PhodavPathDeleteNotify notify, gpointer data, GPtrArray *removed, GCancellable *cancellable);

/* the share-relative path of @path, borrowed; NULL when @path is not
 * part of the share mounted at the base path */
const gchar *handler_get_relative_path (PathHandler *handler, const gchar *path) G_GNUC_PURE;
/* is @path part of the share mounted at the base path? */
gboolean handler_path_is_covered (PathHandler *handler, const gchar *path) G_GNUC_PURE;
/* the shared root, borrowed */
GFile *handler_get_root_file (PathHandler *handler) G_GNUC_PURE;
/* the filesystem backend selected for the shared root, borrowed */
PhodavBackend *handler_get_backend (PathHandler *handler) G_GNUC_PURE;
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

/* one-shot jobs: @run executes on a worker and must only perform the
 * blocking filesystem work; @done executes on the main context and
 * owns @data. The namespace worker is single-threaded, so mutation
 * job results are observed in the same order as the decisions which
 * queued them. The stream pool runs reads and stream pulls beside the
 * serialised mutations. */
typedef void (*PhodavTaskFunc) (gpointer data);
void phodav_server_dispatch (PhodavServer *self,
                             PhodavTaskFunc run,
                             PhodavTaskFunc done,
                             gpointer data);
void phodav_server_dispatch_stream (PhodavServer *self,
                                    PhodavTaskFunc run,
                                    PhodavTaskFunc done,
                                    gpointer data);

/* the serialised namespace: @func runs on the main context as soon as
 * no other mutating request owns it, and owns @data until it releases
 * it. Read-only methods do not take it. */
typedef void (*PhodavNamespaceFunc) (gpointer data);
void phodav_server_namespace_acquire (PhodavServer *self,
                                      PhodavNamespaceFunc func,
                                      gpointer data);
void phodav_server_namespace_release (PhodavServer *self);

void phodav_lock_store_expire_locks (PhodavServer *self);
void phodav_lock_store_remove_locks (PhodavServer *self,
                                     const gchar *path);

gint phodav_check_if (PhodavRequest *req, GList **locks);
gint phodav_check_if_refresh (PhodavRequest *req, GList **locks);

/* a DAV method is a validation, a blocking work step and a commit,
 * in that order. Validation runs on the main context before any work;
 * work runs on a worker and performs the filesystem operation; commit
 * runs on the main context after it, updates the lock store and
 * publishes the response. The request driver owns the namespace, the
 * lifecycle and the method data. */
typedef gint (*PhodavMethodValidate) (PhodavRequest *req, GError **error);
typedef gint (*PhodavMethodWork) (PhodavRequest *req, GError **error);
typedef gint (*PhodavMethodCommit) (PhodavRequest *req, GError **error);

struct _PhodavMethodOps
{
  PhodavMethodValidate validate;
  PhodavMethodWork work;
  PhodavMethodCommit commit;
};

extern const PhodavMethodOps phodav_method_get_ops;
extern const PhodavMethodOps phodav_method_put_ops;
extern const PhodavMethodOps phodav_method_propfind_ops;
extern const PhodavMethodOps phodav_method_proppatch_ops;
extern const PhodavMethodOps phodav_method_mkcol_ops;
extern const PhodavMethodOps phodav_method_delete_ops;
extern const PhodavMethodOps phodav_method_movecopy_ops;
extern const PhodavMethodOps phodav_method_lock_ops;
extern const PhodavMethodOps phodav_method_unlock_ops;

/* per-request state: the request headers are copied and the request
 * body is referenced before the method runs. The body stays shared
 * with the message, which is safe while the message is paused. The
 * response is applied once the method, or its one-shot job, has
 * settled. */
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
/* answer the request once its asynchronous work has settled; a zero
 * @status means the method already published a streamed response */
void phodav_request_complete (PhodavRequest *req, gint status, GError *error);

/* the method state the driver owns and hands to the validate, work
 * and commit steps; it is freed by the driver when the request ends */
void phodav_request_set_method_data (PhodavRequest *req,
                                     gpointer data,
                                     GDestroyNotify data_free);
gpointer phodav_request_get_method_data (PhodavRequest *req) G_GNUC_PURE;
/* a streamed-body method has received and written its whole body: run
 * its work step again, then commit */
void phodav_request_method_body_done (PhodavRequest *req);
/* the client went away or the server is stopping before the streamed
 * body completed: stop the method without running its commit */
void phodav_request_method_abort (PhodavRequest *req);
/* is a worker still using the method data? */
gboolean phodav_request_method_is_working (PhodavRequest *req) G_GNUC_PURE;

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

G_END_DECLS
