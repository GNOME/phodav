/*
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include "phodav-priv.h"

G_BEGIN_DECLS

struct _Path
{
  gchar *path;
  GList *locks;
  guint32 refs;
  /* the lock store which owns this path, or NULL for a standalone one */
  struct _PhodavPathTrie *store;
};

Path *path_ref (Path *path);
void path_unref (Path *path);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Path, path_unref)
void path_remove_lock (Path *path, DAVLock *lock);
void path_add_lock (Path *path, DAVLock *lock);

/* free the locks whose timeout has elapsed; like dav_lock_free(), they
 * remove themselves from the path lock list */
void path_expire_locks (Path *path);

G_END_DECLS
