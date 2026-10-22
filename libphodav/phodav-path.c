/*
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "config.h"

#include "phodav-path.h"

#include "phodav-lock.h"

Path *
path_ref (Path *path)
{
  path->refs++;

  return path;
}

void
path_unref (Path *path)
{
  path->refs--;

  if (path->refs == 0)
    {
      /* the locks remove themselves from the list when freed */
      while (path->locks)
        dav_lock_free (path->locks->data);

      g_free (path->path);
      g_free (path);
    }
}

void
path_remove_lock (Path *path, DAVLock *lock)
{
  g_return_if_fail (path);
  g_return_if_fail (lock);

  path->locks = g_list_remove (path->locks, lock);
}

void
path_add_lock (Path *path, DAVLock *lock)
{
  g_return_if_fail (path);
  g_return_if_fail (lock);

  path->locks = g_list_append (path->locks, lock);
}

void
path_expire_locks (Path *path)
{
  GList *l = path->locks;

  while (l)
    {
      GList *next = l->next;
      DAVLock *lock = l->data;

      if (dav_lock_is_expired (lock))
        dav_lock_free (lock);

      l = next;
    }
}
