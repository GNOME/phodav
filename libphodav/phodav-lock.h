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

#include "phodav-priv.h"

G_BEGIN_DECLS

typedef struct _LockSubmitted
{
  gchar *path;
  gchar *token;
} LockSubmitted;

DAVLock *dav_lock_new (Path *path, const gchar *token, DAVLockScopeType scope, DAVLockType type, DepthType depth, const xmlNodePtr owner, guint timeout);

void dav_lock_free (DAVLock *lock);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (DAVLock, dav_lock_free)

void dav_lock_refresh_timeout (DAVLock *lock, guint timeout);

/* a lock with a timeout which has elapsed; infinite locks never
 * expire and must be released with UNLOCK */
gboolean dav_lock_is_expired (const DAVLock *lock);

xmlNodePtr dav_lock_get_activelock_node (const DAVLock *lock,
                                         xmlNsPtr ns);

LockSubmitted *lock_submitted_new (const gchar *path, const gchar *token);
void lock_submitted_free (LockSubmitted *l);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (LockSubmitted, lock_submitted_free)
gboolean locks_submitted_has (GList *locks, DAVLock *lock);

G_END_DECLS
