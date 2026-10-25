/*
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"

#include "phodav-path.h"
#include "phodav-utils.h"

void
dav_lock_refresh_timeout (DAVLock *lock, guint timeout)
{
  /* an unbounded or missing timeout is capped too, so that a client
   * cannot keep a lock forever or ask for an absurd deadline */
  if (timeout == 0 || timeout > PHODAV_LOCK_TIMEOUT_MAX)
    timeout = PHODAV_LOCK_TIMEOUT_MAX;

  lock->timeout = g_get_monotonic_time () / G_USEC_PER_SEC + timeout;
}

gboolean
dav_lock_is_expired (const DAVLock *lock)
{
  return lock->timeout != 0 &&
         (guint64) (g_get_monotonic_time () / G_USEC_PER_SEC) >= lock->timeout;
}

DAVLock *
dav_lock_new (Path *path, const gchar *token, DAVLockScopeType scope, DAVLockType type, DepthType depth, const xmlNodePtr owner, guint timeout)
{
  DAVLock *lock;

  g_return_val_if_fail (token, NULL);
  g_return_val_if_fail (strlen (token) == DAV_LOCK_TOKEN_LENGTH, NULL);

  lock = g_new0 (DAVLock, 1);
  lock->path = path_ref (path);
  lock->expiry_index = -1;
  /* copy the NUL terminator too, do not rely on the struct padding */
  memcpy (lock->token, token, DAV_LOCK_TOKEN_LENGTH + 1);
  lock->scope = scope;
  lock->type = type;
  lock->depth = depth;
  if (owner)
    lock->owner = xmlCopyNode (owner, 1);

  dav_lock_refresh_timeout (lock, timeout);

  return lock;
}

void
dav_lock_free (DAVLock *lock)
{
  g_return_if_fail (lock);

  path_remove_lock (lock->path, lock);
  path_unref (lock->path);

  if (lock->owner)
    xmlFreeNode (lock->owner);

  g_free (lock);
}

static const gchar *
locktype_to_string (DAVLockType type)
{
  if (type == DAV_LOCK_WRITE)
    return "write";

  g_return_val_if_reached (NULL);
}

static const gchar *
lockscope_to_string (DAVLockScopeType type)
{
  if (type == DAV_LOCK_SCOPE_EXCLUSIVE)
    return "exclusive";
  else if (type == DAV_LOCK_SCOPE_SHARED)
    return "shared";

  g_return_val_if_reached (NULL);
}

xmlNodePtr
dav_lock_get_activelock_node (const DAVLock *lock,
                              xmlNsPtr ns)
{
  xmlNodePtr node, active;

  active = xmlNewNode (ns, BAD_CAST "activelock");

  node = xmlNewChild (active, ns, BAD_CAST "locktype", NULL);
  xmlNewChild (node, ns, BAD_CAST locktype_to_string (lock->type), NULL);
  node = xmlNewChild (active, ns, BAD_CAST "lockscope", NULL);
  xmlNewChild (node, ns, BAD_CAST lockscope_to_string (lock->scope), NULL);
  node = xmlNewChild (active, ns, BAD_CAST "depth", NULL);
  xmlAddChild (node, xmlNewText (BAD_CAST depth_to_string (lock->depth)));

  if (lock->owner)
    xmlAddChild (active, xmlCopyNode (lock->owner, 1));

  node = xmlNewChild (active, ns, BAD_CAST "locktoken", NULL);
  node = xmlNewChild (node, ns, BAD_CAST "href", NULL);
  xmlAddChild (node, xmlNewText (BAD_CAST lock->token));

  {
    g_autofree gchar *href = g_uri_escape_string (lock->path->path,
                                                  G_URI_RESERVED_CHARS_ALLOWED_IN_PATH,
                                                  FALSE);

    node = xmlNewChild (active, ns, BAD_CAST "lockroot", NULL);
    node = xmlNewChild (node, ns, BAD_CAST "href", NULL);
    xmlAddChild (node, xmlNewText (BAD_CAST href));
  }
  if (lock->timeout)
    {
      g_autofree gchar *tmp = g_strdup_printf ("Second-%" G_GINT64_FORMAT, lock->timeout -
                                                                             g_get_monotonic_time () / G_USEC_PER_SEC);
      node = xmlNewChild (active, ns, BAD_CAST "timeout", NULL);
      xmlAddChild (node, xmlNewText (BAD_CAST tmp));
    }

  return active;
}

LockSubmitted *
lock_submitted_new (const gchar *path, const gchar *token)
{
  LockSubmitted *l;

  g_return_val_if_fail (path, NULL);
  g_return_val_if_fail (token, NULL);

  l = g_new (LockSubmitted, 1);

  l->path = g_strdup (path);
  l->token = g_strdup (token);

  remove_trailing (l->path, '/');

  return l;
}

void
lock_submitted_free (LockSubmitted *l)
{
  g_free (l->path);
  g_free (l->token);
  g_free (l);
}

gboolean
locks_submitted_has (GList *locks, DAVLock *lock)
{
  GList *l;

  for (l = locks; l != NULL; l = l->next)
    {
      LockSubmitted *sub = l->data;

      /* the token may be submitted for the locked resource itself or
       * for any resource covered by the lock */
      if (g_strcmp0 (sub->token, lock->token) == 0 &&
          path_is_covered (sub->path, lock->path->path))
        return TRUE;
    }

  g_debug ("missing lock: %s %s", lock->path->path, lock->token);

  return FALSE;
}
