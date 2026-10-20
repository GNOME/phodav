/*
 * Copyright (C) 2014 Red Hat, Inc.
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

typedef gboolean (*PathCb) (const gchar *key,
                            Path *path,
                            gpointer data);

GFile *handler_get_file (PathHandler *handler) G_GNUC_PURE;
GCancellable *handler_get_cancellable (PathHandler *handler) G_GNUC_PURE;
PhodavServer *handler_get_server (PathHandler *handler) G_GNUC_PURE;
gboolean handler_get_readonly (PathHandler *handler) G_GNUC_PURE;

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
gboolean server_tree_has_lock_conflict (PhodavServer *self,
                                        const gchar *path,
                                        DAVLock *lock);
void server_expire_locks (PhodavServer *self);
Path *server_get_path (PhodavServer *self,
                       const gchar *_path);

gint phodav_check_if (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GList **locks);
gint phodav_check_if_refresh (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GList **locks);

gint phodav_delete_file (const gchar *path, GFile *file, GHashTable *mstatus, GCancellable *cancellable);

gint phodav_method_get (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
gint phodav_method_propfind (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
gint phodav_method_proppatch (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
gint phodav_method_mkcol (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
gint phodav_method_delete (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
gint phodav_method_movecopy (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
gint phodav_method_lock (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
gint phodav_method_unlock (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
void phodav_method_put (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);

G_END_DECLS
