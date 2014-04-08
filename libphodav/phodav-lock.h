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
#ifndef __PHODAV_LOCK_H__
#define __PHODAV_LOCK_H__

#include "phodav-priv.h"

G_BEGIN_DECLS

DAVLock *        dav_lock_new             (Path *path, const gchar *token,
                                           LockScopeType scope, LockType type,
                                           DepthType depth, const xmlNodePtr owner,
                                           guint timeout);

void             dav_lock_free            (DAVLock *lock);

void             dav_lock_refresh_timeout (DAVLock *lock, guint timeout);

G_END_DECLS

#endif /* __PHODAV_LOCK_H__ */
