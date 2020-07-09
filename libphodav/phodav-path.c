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
        g_list_free_full (path->locks, (GDestroyNotify) dav_lock_free);
        g_free (path->path);
        g_slice_free (Path, path);
    }
}

void
path_remove_lock (Path *path, DAVLock *lock)
{
    g_return_if_fail (path);
    g_return_if_fail (lock);

    path->locks =  g_list_remove (path->locks, lock);
}

void
path_add_lock (Path *path, DAVLock *lock)
{
    g_return_if_fail (path);
    g_return_if_fail (lock);

    path->locks = g_list_append (path->locks, lock);
}
