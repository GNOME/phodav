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
#ifndef __PHODAV_PATH_H__
#define __PHODAV_PATH_H__

#include "phodav-priv.h"

G_BEGIN_DECLS

struct _Path
{
  gchar         *path;
  GList         *locks;
  guint32        refs;
};

Path *                  path_ref                    (Path *path);
void                    path_unref                  (Path *path);
void                    path_remove_lock            (Path *path, DAVLock *lock);
void                    path_add_lock               (Path *path, DAVLock *lock);

G_END_DECLS

#endif /* __PHODAV_PATH_H__ */
