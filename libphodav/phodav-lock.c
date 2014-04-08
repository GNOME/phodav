/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2; -*- */
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
              LockScopeType scope, LockType type,
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
