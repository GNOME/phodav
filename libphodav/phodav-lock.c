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

#include "phodav-lock.h"
#include "phodav-utils.h"
#include "phodav-path.h"

void
dav_lock_refresh_timeout (DAVLock *lock, guint timeout)
{
  if (timeout)
    lock->timeout = g_get_monotonic_time () / G_USEC_PER_SEC + timeout;
  else
    lock->timeout = 0;
}

DAVLock *
dav_lock_new (Path *path, const gchar *token,
              DAVLockScopeType scope, DAVLockType type,
              DepthType depth, const xmlNodePtr owner,
              guint timeout)
{
  DAVLock *lock;

  g_return_val_if_fail (token, NULL);
  g_return_val_if_fail (strlen (token) == sizeof (lock->token), NULL);

  lock = g_slice_new0 (DAVLock);
  lock->path = path_ref (path);
  memcpy (lock->token, token, sizeof (lock->token));
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

  g_slice_free (DAVLock, lock);
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
                              xmlNsPtr       ns)
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

  node = xmlNewChild (active, ns, BAD_CAST "lockroot", NULL);
  node = xmlNewChild (node, ns, BAD_CAST "href", NULL);
  xmlAddChild (node, xmlNewText (BAD_CAST lock->path->path));
  if (lock->timeout)
    {
      gchar *tmp = g_strdup_printf ("Second-%" G_GINT64_FORMAT, lock->timeout -
                                    g_get_monotonic_time () / G_USEC_PER_SEC);
      node = xmlNewChild (active, ns, BAD_CAST "timeout", NULL);
      xmlAddChild (node, xmlNewText (BAD_CAST tmp));
      g_free (tmp);
    }

  return active;
}

LockSubmitted *
lock_submitted_new (const gchar *path, const gchar *token)
{
  LockSubmitted *l;

  g_return_val_if_fail (path, NULL);
  g_return_val_if_fail (token, NULL);

  l = g_slice_new (LockSubmitted);

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
  g_slice_free (LockSubmitted, l);
}

gboolean
locks_submitted_has (GList *locks, DAVLock *lock)
{
  GList *l;

  for (l = locks; l != NULL; l = l->next)
  {
    LockSubmitted *sub = l->data;
    if (!g_strcmp0 (sub->path, lock->path->path) &&
        !g_strcmp0 (sub->token, lock->token))
      return TRUE;
  }

  g_message ("missing lock: %s %s", lock->path->path, lock->token);

  return FALSE;
}
