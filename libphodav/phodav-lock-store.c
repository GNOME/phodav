/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-path.h"
#include "phodav-priv.h"
#include "phodav-server-priv.h"
#include "phodav-utils.h"

/* The lock store is a trie keyed on the canonical resource path
 * components. A node may hold the Path for the resource at that exact
 * path; every child is one path segment deeper. The _locked helpers
 * below assume the lock store mutex is held; the value-returning
 * service takes it once around them. */

typedef struct _PhodavPathTrieNode PhodavPathTrieNode;

struct _PhodavPathTrieNode
{
  gchar *segment; /* owned, NULL for the root */
  Path *path;     /* owned reference, NULL when the path has no entry */
  GHashTable *children;
  PhodavPathTrieNode *parent; /* borrowed */
  PhodavPathTrie *trie;       /* borrowed, the owning lock store */
};

struct _PhodavPathTrie
{
  PhodavPathTrieNode *root;
  /* min-heap of DAVLock *, keyed by the absolute timeout; the earliest
   * deadline is always at index 0 */
  GArray *expiry;
  guint lock_count;
};

static void lock_store_expire_heap_locked (PhodavServer *self);

static void
lock_store_heap_swap (PhodavPathTrie *trie, guint a, guint b)
{
  DAVLock *da = g_array_index (trie->expiry, DAVLock *, a);
  DAVLock *db = g_array_index (trie->expiry, DAVLock *, b);

  g_array_index (trie->expiry, DAVLock *, a) = db;
  g_array_index (trie->expiry, DAVLock *, b) = da;
  da->expiry_index = b;
  db->expiry_index = a;
}

static void
lock_store_heap_sift_up (PhodavPathTrie *trie, guint index)
{
  while (index > 0)
    {
      guint parent = (index - 1) / 2;
      DAVLock *lock = g_array_index (trie->expiry, DAVLock *, index);
      DAVLock *up = g_array_index (trie->expiry, DAVLock *, parent);

      if (up->timeout <= lock->timeout)
        break;

      lock_store_heap_swap (trie, index, parent);
      index = parent;
    }
}

static void
lock_store_heap_sift_down (PhodavPathTrie *trie, guint index)
{
  for (;;)
    {
      guint left = index * 2 + 1;
      guint right = left + 1;
      guint smallest;
      DAVLock *lock, *child;

      if (left >= trie->expiry->len)
        break;

      smallest = left;
      if (right < trie->expiry->len &&
          g_array_index (trie->expiry, DAVLock *, right)->timeout <
            g_array_index (trie->expiry, DAVLock *, left)->timeout)
        smallest = right;

      lock = g_array_index (trie->expiry, DAVLock *, index);
      child = g_array_index (trie->expiry, DAVLock *, smallest);
      if (lock->timeout <= child->timeout)
        break;

      lock_store_heap_swap (trie, index, smallest);
      index = smallest;
    }
}

static void
lock_store_heap_insert (PhodavPathTrie *trie, DAVLock *lock)
{
  if (lock->timeout == 0)
    {
      lock->expiry_index = -1;
      return;
    }

  g_array_append_val (trie->expiry, lock);
  lock->expiry_index = trie->expiry->len - 1;
  lock_store_heap_sift_up (trie, lock->expiry_index);
}

static void
lock_store_heap_remove (PhodavPathTrie *trie, DAVLock *lock)
{
  guint index, last;

  if (lock->expiry_index < 0)
    return;

  index = lock->expiry_index;
  last = trie->expiry->len - 1;
  lock->expiry_index = -1;

  if (index != last)
    {
      DAVLock *moved = g_array_index (trie->expiry, DAVLock *, last);

      g_array_index (trie->expiry, DAVLock *, index) = moved;
      moved->expiry_index = index;
    }

  g_array_set_size (trie->expiry, last);
  if (index < last)
    {
      lock_store_heap_sift_down (trie, index);
      lock_store_heap_sift_up (trie, index);
    }
}

static void
lock_store_free_lock_locked (PhodavPathTrie *trie, DAVLock *lock)
{
  lock_store_heap_remove (trie, lock);
  g_assert (trie->lock_count > 0);
  trie->lock_count--;
  dav_lock_free (lock);
}

static void
path_trie_node_free (gpointer data)
{
  PhodavPathTrieNode *node = data;

  /* dropping the subtree locks first lets the paths be released with
   * their trie reference */
  g_hash_table_remove_all (node->children);
  if (node->path)
    {
      while (node->path->locks)
        lock_store_free_lock_locked (node->trie, node->path->locks->data);

      path_unref (node->path);
    }

  g_hash_table_unref (node->children);
  g_free (node->segment);
  g_free (node);
}

static PhodavPathTrieNode *
path_trie_node_new (PhodavPathTrie *trie,
                    PhodavPathTrieNode *parent,
                    const gchar *segment)
{
  PhodavPathTrieNode *node = g_new0 (PhodavPathTrieNode, 1);

  node->segment = g_strdup (segment);
  node->children = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, path_trie_node_free);
  node->parent = parent;
  node->trie = trie;

  return node;
}

PhodavPathTrie *
phodav_lock_store_paths_new (void)
{
  PhodavPathTrie *trie = g_new0 (PhodavPathTrie, 1);

  trie->expiry = g_array_new (FALSE, FALSE, sizeof (DAVLock *));
  trie->root = path_trie_node_new (trie, NULL, NULL);

  return trie;
}

void
phodav_lock_store_paths_free (PhodavPathTrie *trie)
{
  if (!trie)
    return;

  path_trie_node_free (trie->root);
  g_array_unref (trie->expiry);
  g_free (trie);
}

/* canonical paths start with '/', their components are separated by
 * single '/' and the root is "/" */
static PhodavPathTrieNode *
path_trie_lookup_node (PhodavPathTrie *trie, const gchar *path, gboolean create)
{
  PhodavPathTrieNode *node = trie->root;
  const gchar *p = path;

  if (!path || *path != '/')
    return NULL;

  while (*p)
    {
      const gchar *start;
      const gchar *slash;
      g_autofree gchar *segment = NULL;
      PhodavPathTrieNode *child;

      while (*p == '/')
        p++;
      if (!*p)
        break;

      start = p;
      slash = strchr (start, '/');
      if (slash)
        {
          segment = g_strndup (start, slash - start);
          p = slash;
        }
      else
        {
          segment = g_strdup (start);
          p += strlen (p);
        }

      child = g_hash_table_lookup (node->children, segment);
      if (!child)
        {
          if (!create)
            return NULL;

          child = path_trie_node_new (node->trie, node, segment);
          g_hash_table_insert (node->children, g_strdup (segment), child);
        }

      node = child;
    }

  return node;
}

/* drop the node and its empty ancestors when it has neither a lock
 * nor a descendant anymore */
static void
path_trie_prune_node (PhodavPathTrieNode *node)
{
  while (node->parent && !node->path &&
         g_hash_table_size (node->children) == 0)
    {
      PhodavPathTrieNode *parent = node->parent;
      g_autofree gchar *segment = g_strdup (node->segment);

      g_hash_table_remove (parent->children, segment);
      node = parent;
    }
}

/* remove @path and all the paths below it */
static void
path_trie_remove_subtree (PhodavPathTrie *trie, const gchar *path)
{
  PhodavPathTrieNode *node = path_trie_lookup_node (trie, path, FALSE);

  if (!node)
    return;

  if (node->parent)
    {
      PhodavPathTrieNode *parent = node->parent;
      g_autofree gchar *segment = g_strdup (node->segment);

      g_hash_table_remove (parent->children, segment);
      path_trie_prune_node (parent);
    }
  else
    {
      /* keep the root node itself, drop everything it holds */
      g_hash_table_remove_all (node->children);
      if (node->path)
        {
          while (node->path->locks)
            lock_store_free_lock_locked (node->trie, node->path->locks->data);

          path_unref (node->path);
          node->path = NULL;
        }
    }
}

static Path *
lock_store_get_path_locked (PhodavServer *self, const gchar *_path)
{
  g_autofree gchar *path = phodav_path_canonicalize (_path);
  PhodavPathTrieNode *node = path_trie_lookup_node (self->paths, path, TRUE);

  g_assert (node != NULL);

  if (!node->path)
    {
      Path *p = g_new0 (Path, 1);

      p->path = g_steal_pointer (&path);
      p->refs = 1;
      p->store = self->paths;
      node->path = p;
    }

  return node->path;
}

/* drop the path from the trie when it holds no lock anymore;
 * called when a LOCK request is rolled back, so that a failed request
 * does not leave an empty entry behind */
static void
lock_store_remove_path_if_empty_locked (PhodavServer *self, const gchar *_path)
{
  g_autofree gchar *path = phodav_path_canonicalize (_path);
  PhodavPathTrieNode *node = path_trie_lookup_node (self->paths, path, FALSE);

  if (!node || !node->path)
    return;

  if (node->path->locks)
    return;

  path_unref (node->path);
  node->path = NULL;
  path_trie_prune_node (node);
}

gint64
phodav_lock_store_next_expiry (PhodavServer *self)
{
  gint64 next;

  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);
  if (self->paths->expiry->len)
    next = g_array_index (self->paths->expiry, DAVLock *, 0)->timeout;
  else
    next = 0;
  g_mutex_unlock (&self->lock_store_mutex);

  return next;
}

/* pop the elapsed locks from the expiry heap, earliest first. The trie
 * is only touched to drop a path once its last lock is gone, so expiry
 * costs O(expired) rather than O(all resources). */
static void
lock_store_expire_heap_locked (PhodavServer *self)
{
  PhodavPathTrie *trie = self->paths;
  gint64 now = g_get_monotonic_time () / G_USEC_PER_SEC;

  while (trie->expiry->len > 0)
    {
      DAVLock *lock = g_array_index (trie->expiry, DAVLock *, 0);
      Path *path = lock->path;

      if (lock->timeout == 0 || lock->timeout > now)
        break;

      lock_store_free_lock_locked (trie, lock);
      lock_store_remove_path_if_empty_locked (self, path->path);
    }
}

/* free the locks whose timeout has elapsed, and drop the empty paths
 * so the trie does not grow without bound with the locks taken
 * over the lifetime of the server */
void
phodav_lock_store_expire_locks (PhodavServer *self)
{
  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);
  g_mutex_unlock (&self->lock_store_mutex);

  phodav_server_schedule_lock_expiry (self);
}

/* destroy the locks rooted on a deleted resource and its members, and
 * drop the paths which are no longer needed */
void
phodav_lock_store_remove_locks (PhodavServer *self, const gchar *_path)
{
  g_autofree gchar *root = phodav_path_canonicalize (_path);

  g_mutex_lock (&self->lock_store_mutex);
  path_trie_remove_subtree (self->paths, root);
  g_mutex_unlock (&self->lock_store_mutex);

  phodav_server_schedule_lock_expiry (self);
}

typedef gboolean (*PathCb) (Path *path, gpointer data);

/* visit the root, then each path component down to @path */
static gboolean
path_trie_foreach_prefix (PhodavPathTrie *trie, const gchar *path, PathCb cb, gpointer data)
{
  PhodavPathTrieNode *node = trie->root;
  const gchar *p = path;

  if (!path || *path != '/')
    return TRUE;

  if (node->path && !cb (node->path, data))
    return FALSE;

  while (*p)
    {
      const gchar *start;
      const gchar *slash;
      g_autofree gchar *segment = NULL;
      PhodavPathTrieNode *child;

      while (*p == '/')
        p++;
      if (!*p)
        break;

      start = p;
      slash = strchr (start, '/');
      if (slash)
        {
          segment = g_strndup (start, slash - start);
          p = slash;
        }
      else
        {
          segment = g_strdup (start);
          p += strlen (p);
        }

      child = g_hash_table_lookup (node->children, segment);
      if (!child)
        break;

      node = child;
      if (node->path && !cb (node->path, data))
        return FALSE;
    }

  return TRUE;
}

static gboolean
path_trie_node_foreach (PhodavPathTrieNode *node, PathCb cb, gpointer data)
{
  GHashTableIter iter;
  PhodavPathTrieNode *child;

  if (node->path && !cb (node->path, data))
    return FALSE;

  g_hash_table_iter_init (&iter, node->children);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &child))
    if (!path_trie_node_foreach (child, cb, data))
      return FALSE;

  return TRUE;
}

/* visit @path and every path below it */
static gboolean
path_trie_foreach_subtree (PhodavPathTrie *trie, const gchar *path, PathCb cb, gpointer data)
{
  PhodavPathTrieNode *node = path_trie_lookup_node (trie, path, FALSE);

  if (!node)
    return TRUE;

  return path_trie_node_foreach (node, cb, data);
}

static gboolean
lock_store_foreach_parent_path_locked (PhodavServer *self, const gchar *path, PathCb cb, gpointer data)
{
  g_autofree gchar *canonical = phodav_path_canonicalize (path);

  return path_trie_foreach_prefix (self->paths, canonical, cb, data);
}

typedef struct _PathGetLock
{
  const gchar *token;
  DAVLock *lock;
} PathGetLock;

static gboolean
_path_get_lock (Path *path, gpointer data)
{
  PathGetLock *d = data;
  GList *l;

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
static DAVLock *
lock_store_lookup_lock_locked (PhodavServer *self, const gchar *path, const gchar *token)
{
  PathGetLock p = { .token = token };

  lock_store_foreach_parent_path_locked (self, path, _path_get_lock, &p);

  return p.lock;
}

static DAVLock *
lock_store_get_lock_locked (PhodavServer *self, const gchar *path, const gchar *token)
{
  DAVLock *lock = lock_store_lookup_lock_locked (self, path, token);

  if (!lock)
    g_debug ("Invalid lock token %s for %s", token, path);

  return lock;
}

static gboolean
other_lock_exists (Path *path, gpointer data)
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

static gboolean
lock_store_path_has_other_locks_locked (PhodavServer *self, const gchar *path, GList *locks)
{
  return !lock_store_foreach_parent_path_locked (self, path, other_lock_exists, locks);
}

/* does the tree rooted at @_path contain a lock which is not among
 * the submitted @locks? Operations which modify or replace a whole
 * collection (DELETE, MOVE, or a COPY/MOVE overwriting a collection)
 * must submit the tokens of the locks held on its members too
 * (RFC 4918 sections 7.1 and 9.6.1). */
static gboolean
tree_other_lock_exists (Path *path, gpointer data)
{
  GList *locks = data;
  GList *l;

  for (l = path->locks; l != NULL; l = l->next)
    if (!locks_submitted_has (locks, l->data))
      return FALSE;

  return TRUE;
}

static gboolean
lock_store_tree_has_other_locks_locked (PhodavServer *self, const gchar *_path, GList *locks)
{
  g_autofree gchar *root = phodav_path_canonicalize (_path);

  return !path_trie_foreach_subtree (self->paths, root,
                                     tree_other_lock_exists, locks);
}

typedef struct _TreeLockCheck
{
  const gchar *path; /* canonical path of the new lock root */
  DAVLock *lock;     /* the lock about to be taken */
  gboolean conflict;
} TreeLockCheck;

static gboolean
tree_check_lock_cb (Path *path, gpointer user_data)
{
  TreeLockCheck *d = user_data;
  GList *l;

  /* the lock root itself and its ancestors are checked by
   * lock_store_foreach_parent_path_locked() in add_lock */
  if (g_strcmp0 (path->path, d->path) == 0)
    return TRUE;

  for (l = path->locks; l != NULL; l = l->next)
    {
      DAVLock *other = l->data;

      /* RFC 4918 section 6.1: an exclusive lock conflicts with any
       * other kind of lock, whether direct or indirect */
      if (other->scope == DAV_LOCK_SCOPE_EXCLUSIVE ||
          d->lock->scope == DAV_LOCK_SCOPE_EXCLUSIVE)
        {
          d->conflict = TRUE;
          return FALSE;
        }
    }

  return TRUE;
}

/* does a depth-infinity lock on @path conflict with a lock on one of
 * its members? (RFC 4918 section 7.4: such a LOCK request must fail
 * with 423 when the members are locked in a conflicting manner) */
static gboolean
lock_store_tree_has_lock_conflict_locked (PhodavServer *self, const gchar *_path, DAVLock *lock)
{
  g_autofree gchar *path = phodav_path_canonicalize (_path);
  TreeLockCheck d = { .path = path, .lock = lock, .conflict = FALSE };

  path_trie_foreach_subtree (self->paths, path, tree_check_lock_cb, &d);

  return d.conflict;
}

/* -------------------------------------------------------------------------
 * The value-returning lock service
 *
 * The queries below return decisions or owned XML, never borrowed
 * Path/DAVLock pointers. The trie is protected by @lock_store_mutex,
 * so a read-only PROPFIND job may walk it beside a mutation.
 */

gboolean
phodav_lock_store_has_token (PhodavServer *self, const gchar *path, const gchar *token)
{
  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);
  gboolean found = lock_store_get_lock_locked (self, path, token) != NULL;
  g_mutex_unlock (&self->lock_store_mutex);

  return found;
}

gboolean
phodav_lock_store_path_has_other_locks (PhodavServer *self, const gchar *path, GList *locks)
{
  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);
  gboolean found = lock_store_path_has_other_locks_locked (self, path, locks);
  g_mutex_unlock (&self->lock_store_mutex);

  return found;
}

gboolean
phodav_lock_store_tree_has_other_locks (PhodavServer *self, const gchar *path, GList *locks)
{
  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);
  gboolean found = lock_store_tree_has_other_locks_locked (self, path, locks);
  g_mutex_unlock (&self->lock_store_mutex);

  return found;
}

static gboolean
check_exclusive_lock (Path *path, gpointer data)
{
  DAVLock *lock = data;
  DAVLock *other = NULL;
  GList *l;

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

  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);

  if (self->paths->lock_count >= PHODAV_LOCK_COUNT_MAX)
    {
      g_debug ("lock store full: %u locks", self->paths->lock_count);
      status = SOUP_STATUS_INSUFFICIENT_STORAGE;
      goto out;
    }

  p = lock_store_get_path_locked (self, path);
  lock = dav_lock_new (p, token, scope, type, depth, owner, timeout);
  if (!lock_store_foreach_parent_path_locked (self, path, check_exclusive_lock, lock) ||
      (depth == DEPTH_INFINITY &&
       lock_store_tree_has_lock_conflict_locked (self, path, lock)))
    {
      dav_lock_free (lock);
      lock_store_remove_path_if_empty_locked (self, path);
      goto out;
    }

  path_add_lock (p, lock);
  lock_store_heap_insert (self->paths, lock);
  self->paths->lock_count++;
  status = SOUP_STATUS_OK;

out:
  g_mutex_unlock (&self->lock_store_mutex);

  phodav_server_schedule_lock_expiry (self);
  return status;
}

gint
phodav_lock_store_refresh_lock (PhodavServer *self, const gchar *path, GList *submitted, guint timeout, gchar **refreshed_token)
{
  GList *l;
  gint status = SOUP_STATUS_BAD_REQUEST;

  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);

  for (l = submitted; l; l = l->next)
    {
      LockSubmitted *sub = l->data;
      DAVLock *lock = lock_store_lookup_lock_locked (self, path, sub->token);

      if (!lock)
        continue;

      lock_store_heap_remove (self->paths, lock);
      dav_lock_refresh_timeout (lock, timeout);
      lock_store_heap_insert (self->paths, lock);
      *refreshed_token = g_strdup (lock->token);
      status = SOUP_STATUS_OK;
      break;
    }

  g_mutex_unlock (&self->lock_store_mutex);

  phodav_server_schedule_lock_expiry (self);
  return status;
}

xmlNodePtr
phodav_lock_store_lock_node (PhodavServer *self, const gchar *path, const gchar *token, xmlNsPtr ns)
{
  DAVLock *lock;
  xmlNodePtr node = NULL;

  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);
  lock = lock_store_lookup_lock_locked (self, path, token);
  if (lock)
    node = dav_lock_get_activelock_node (lock, ns);
  g_mutex_unlock (&self->lock_store_mutex);

  return node;
}

gint
phodav_lock_store_remove_lock (PhodavServer *self, const gchar *path, const gchar *token)
{
  DAVLock *lock;
  gint status = SOUP_STATUS_CONFLICT;

  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);
  lock = lock_store_get_lock_locked (self, path, token);
  if (lock)
    {
      lock_store_free_lock_locked (self->paths, lock);
      lock_store_remove_path_if_empty_locked (self, path);
      status = SOUP_STATUS_NO_CONTENT;
    }
  g_mutex_unlock (&self->lock_store_mutex);

  phodav_server_schedule_lock_expiry (self);
  return status;
}

static gboolean
lockdiscovery_cb (Path *path, gpointer data)
{
  xmlNodePtr node = data;
  GList *l;

  for (l = path->locks; l != NULL; l = l->next)
    xmlAddChild (node, dav_lock_get_activelock_node (l->data, NULL));

  return TRUE;
}

void
phodav_lock_store_lockdiscovery (PhodavServer *self, const gchar *path, xmlNodePtr node)
{
  g_mutex_lock (&self->lock_store_mutex);
  lock_store_expire_heap_locked (self);
  lock_store_foreach_parent_path_locked (self, path, lockdiscovery_cb, node);
  g_mutex_unlock (&self->lock_store_mutex);
}
