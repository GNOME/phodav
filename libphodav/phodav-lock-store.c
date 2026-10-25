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

  if (g_hash_table_size (self->paths) == 0)
    return FALSE;

  g_hash_table_iter_init (&iter, self->paths);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &path))
    {
      GList *l;

      path_expire_locks (path);

      if (!path_is_covered (path->path, root))
        continue;

      for (l = path->locks; l != NULL; l = l->next)
        if (!locks_submitted_has (locks, l->data))
          return TRUE;
    }

  return FALSE;
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
