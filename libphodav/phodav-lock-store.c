/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-path.h"
#include "phodav-priv.h"
#include "phodav-utils.h"

/* The lock table is keyed on the canonical resource path, so that the
 * locks are found again under whatever spelling of the path the client
 * used in its request. server_foreach_parent_path() walks the same
 * canonical keys, so the two cannot disagree on the canonical form. */
Path *
server_get_path (PhodavServer *self, const gchar *_path)
{
  Path *p;
  g_autofree gchar *path = phodav_path_canonicalize (_path);

  g_rec_mutex_lock (&self->lock_store_mutex);

  p = g_hash_table_lookup (self->paths, path);
  if (!p)
    {
      p = g_new0 (Path, 1);
      p->path = g_steal_pointer (&path);
      g_hash_table_insert (self->paths, p->path, path_ref (p));
    }

  g_rec_mutex_unlock (&self->lock_store_mutex);

  return p;
}

/* drop the path from the lock table when it holds no lock anymore;
 * called when a LOCK request is rolled back, so that a failed request
 * does not leave an empty entry behind */
void
server_remove_path_if_empty (PhodavServer *self, const gchar *_path)
{
  g_autofree gchar *path = phodav_path_canonicalize (_path);
  Path *p;

  g_rec_mutex_lock (&self->lock_store_mutex);

  p = g_hash_table_lookup (self->paths, path);
  if (!p)
    goto out;

  path_expire_locks (p);
  if (!p->locks)
    g_hash_table_remove (self->paths, path);

out:
  g_rec_mutex_unlock (&self->lock_store_mutex);
}

/* free the locks whose timeout has elapsed, and drop the empty paths
 * so the lock table does not grow without bound with the locks taken
 * over the lifetime of the server */
void
server_expire_locks (PhodavServer *self)
{
  GHashTableIter iter;
  Path *path;

  g_rec_mutex_lock (&self->lock_store_mutex);

  if (g_hash_table_size (self->paths) == 0)
    goto out;

  g_hash_table_iter_init (&iter, self->paths);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &path))
    {
      path_expire_locks (path);

      /* the path is only kept alive by the table when it has no locks */
      if (!path->locks)
        g_hash_table_iter_remove (&iter);
    }

out:
  g_rec_mutex_unlock (&self->lock_store_mutex);
}

/* destroy the locks rooted on a deleted resource and its members, and
 * drop the paths which are no longer needed */
void
server_remove_locks (PhodavServer *self, const gchar *_path)
{
  GHashTableIter iter;
  Path *path;
  g_autofree gchar *root = phodav_path_canonicalize (_path);

  g_rec_mutex_lock (&self->lock_store_mutex);

  if (g_hash_table_size (self->paths) == 0)
    goto out;

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

out:
  g_rec_mutex_unlock (&self->lock_store_mutex);
}

gboolean
server_foreach_parent_path (PhodavServer *self, const gchar *path, PathCb cb, gpointer data)
{
  g_autofree gchar *canonical = phodav_path_canonicalize (path);
  gboolean ret = FALSE;
  gchar *key = NULL;
  Path *p;
  const gchar *segment = canonical + 1;

  g_rec_mutex_lock (&self->lock_store_mutex);

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
  g_rec_mutex_unlock (&self->lock_store_mutex);
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

/* does the tree rooted at @_path contain a lock which is not among
 * the submitted @locks? Operations which modify or replace a whole
 * collection (DELETE, MOVE, or a COPY/MOVE overwriting a collection)
 * must submit the tokens of the locks held on its members too
 * (RFC 4918 sections 7.1 and 9.6.1). */
gboolean
server_tree_has_other_locks (PhodavServer *self, const gchar *_path, GList *locks)
{
  g_autofree gchar *root = phodav_path_canonicalize (_path);
  GHashTableIter iter;
  Path *path;
  gboolean found = FALSE;

  g_rec_mutex_lock (&self->lock_store_mutex);

  if (g_hash_table_size (self->paths) == 0)
    goto out;

  g_hash_table_iter_init (&iter, self->paths);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &path))
    {
      GList *l;

      path_expire_locks (path);

      if (!path_is_covered (path->path, root))
        continue;

      for (l = path->locks; l != NULL; l = l->next)
        if (!locks_submitted_has (locks, l->data))
          {
            found = TRUE;
            goto out;
          }
    }

out:
  g_rec_mutex_unlock (&self->lock_store_mutex);

  return found;
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

  g_rec_mutex_lock (&self->lock_store_mutex);
  g_hash_table_foreach (self->paths, tree_check_lock_cb, &d);
  g_rec_mutex_unlock (&self->lock_store_mutex);

  return d.conflict;
}

/* -------------------------------------------------------------------------
 * The value-returning lock service
 *
 * The queries below return decisions or owned XML, never borrowed
 * Path/DAVLock pointers. A guard reserves one or more paths (and their
 * subtrees) for the duration of a filesystem operation; overlapping
 * guards wait on guard_cond instead of serializing every request.
 */

struct _PhodavLockGuard
{
  PhodavServer *self;
  GList *paths; /* canonical paths */
};

static gboolean
guard_overlaps (PhodavLockGuard *guard, GList *paths)
{
  GList *g;
  GList *p;

  for (g = guard->paths; g; g = g->next)
    for (p = paths; p; p = p->next)
      if (path_is_covered (g->data, p->data) ||
          path_is_covered (p->data, g->data))
        return TRUE;

  return FALSE;
}

PhodavLockGuard *
phodav_lock_store_guard_acquire (PhodavServer *self, GList *paths)
{
  PhodavLockGuard *guard;
  GList *l;

  guard = g_new0 (PhodavLockGuard, 1);
  guard->self = self;
  for (l = paths; l; l = l->next)
    guard->paths = g_list_prepend (guard->paths,
                                   phodav_path_canonicalize (l->data));

  g_mutex_lock (&self->guard_mutex);
  for (;;)
    {
      gboolean conflict = FALSE;

      for (l = self->guards; l; l = l->next)
        if (guard_overlaps (l->data, guard->paths))
          {
            conflict = TRUE;
            break;
          }

      if (!conflict)
        break;

      g_cond_wait (&self->guard_cond, &self->guard_mutex);
    }

  self->guards = g_list_prepend (self->guards, guard);
  g_mutex_unlock (&self->guard_mutex);

  return guard;
}

void
phodav_lock_store_guard_release (PhodavLockGuard *guard)
{
  PhodavServer *self;

  if (!guard)
    return;

  self = guard->self;
  g_mutex_lock (&self->guard_mutex);
  self->guards = g_list_remove (self->guards, guard);
  g_cond_broadcast (&self->guard_cond);
  g_mutex_unlock (&self->guard_mutex);

  g_list_free_full (guard->paths, g_free);
  g_free (guard);
}

gboolean
phodav_lock_store_has_token (PhodavServer *self, const gchar *path, const gchar *token)
{
  g_rec_mutex_lock (&self->lock_store_mutex);
  gboolean found = server_path_get_lock (self, path, token) != NULL;
  g_rec_mutex_unlock (&self->lock_store_mutex);

  return found;
}

gboolean
phodav_lock_store_path_has_other_locks (PhodavServer *self, const gchar *path, GList *locks)
{
  g_rec_mutex_lock (&self->lock_store_mutex);
  gboolean found = server_path_has_other_locks (self, path, locks);
  g_rec_mutex_unlock (&self->lock_store_mutex);

  return found;
}

gboolean
phodav_lock_store_tree_has_other_locks (PhodavServer *self, const gchar *path, GList *locks)
{
  g_rec_mutex_lock (&self->lock_store_mutex);
  gboolean found = server_tree_has_other_locks (self, path, locks);
  g_rec_mutex_unlock (&self->lock_store_mutex);

  return found;
}

static gboolean
check_exclusive_lock (const gchar *key, Path *path, gpointer data)
{
  DAVLock *lock = data;
  DAVLock *other = NULL;
  GList *l;

  path_expire_locks (path);

  for (l = path->locks; l; l = l->next)
    {
      other = l->data;
      if (other->scope == DAV_LOCK_SCOPE_EXCLUSIVE)
        return FALSE;
    }

  if (other && lock->scope == DAV_LOCK_SCOPE_EXCLUSIVE)
    return FALSE;

  return TRUE;
}

gint
phodav_lock_store_add_lock (PhodavServer *self, const gchar *path, const gchar *token, DAVLockScopeType scope, DAVLockType type, DepthType depth, const xmlNodePtr owner, guint timeout)
{
  Path *p;
  DAVLock *lock;
  gint status = SOUP_STATUS_LOCKED;

  g_rec_mutex_lock (&self->lock_store_mutex);

  p = server_get_path (self, path);
  lock = dav_lock_new (p, token, scope, type, depth, owner, timeout);
  if (!server_foreach_parent_path (self, path, check_exclusive_lock, lock) ||
      (depth == DEPTH_INFINITY &&
       server_tree_has_lock_conflict (self, path, lock)))
    {
      dav_lock_free (lock);
      server_remove_path_if_empty (self, path);
      goto out;
    }

  path_add_lock (p, lock);
  status = SOUP_STATUS_OK;

out:
  g_rec_mutex_unlock (&self->lock_store_mutex);
  return status;
}

gint
phodav_lock_store_refresh_lock (PhodavServer *self, const gchar *path, GList *submitted, guint timeout, gchar **refreshed_token)
{
  GList *l;
  gint status = SOUP_STATUS_BAD_REQUEST;

  g_rec_mutex_lock (&self->lock_store_mutex);

  for (l = submitted; l; l = l->next)
    {
      LockSubmitted *sub = l->data;
      DAVLock *lock = server_path_lookup_lock (self, path, sub->token);

      if (!lock)
        continue;

      dav_lock_refresh_timeout (lock, timeout);
      *refreshed_token = g_strdup (lock->token);
      status = SOUP_STATUS_OK;
      break;
    }

  g_rec_mutex_unlock (&self->lock_store_mutex);
  return status;
}

xmlNodePtr
phodav_lock_store_lock_node (PhodavServer *self, const gchar *path, const gchar *token, xmlNsPtr ns)
{
  DAVLock *lock;
  xmlNodePtr node = NULL;

  g_rec_mutex_lock (&self->lock_store_mutex);
  lock = server_path_lookup_lock (self, path, token);
  if (lock)
    node = dav_lock_get_activelock_node (lock, ns);
  g_rec_mutex_unlock (&self->lock_store_mutex);

  return node;
}

gint
phodav_lock_store_remove_lock (PhodavServer *self, const gchar *path, const gchar *token)
{
  DAVLock *lock;
  gint status = SOUP_STATUS_CONFLICT;

  g_rec_mutex_lock (&self->lock_store_mutex);
  lock = server_path_get_lock (self, path, token);
  if (lock)
    {
      dav_lock_free (lock);
      server_remove_path_if_empty (self, path);
      status = SOUP_STATUS_NO_CONTENT;
    }
  g_rec_mutex_unlock (&self->lock_store_mutex);

  return status;
}

static gboolean
lockdiscovery_cb (const gchar *key, Path *path, gpointer data)
{
  xmlNodePtr node = data;
  GList *l;

  path_expire_locks (path);

  for (l = path->locks; l != NULL; l = l->next)
    xmlAddChild (node, dav_lock_get_activelock_node (l->data, NULL));

  return TRUE;
}

void
phodav_lock_store_lockdiscovery (PhodavServer *self, const gchar *path, xmlNodePtr node)
{
  g_rec_mutex_lock (&self->lock_store_mutex);
  server_foreach_parent_path (self, path, lockdiscovery_cb, node);
  g_rec_mutex_unlock (&self->lock_store_mutex);
}
