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
#ifndef __PHODAV_PRIV_H__
#define __PHODAV_PRIV_H__

#include "config.h"

#include <glib/gi18n.h>
#include <glib-object.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

G_BEGIN_DECLS

typedef struct _DAVLock DAVLock;
typedef struct _Path    Path;

typedef enum _LockScopeType {
  LOCK_SCOPE_NONE,
  LOCK_SCOPE_EXCLUSIVE,
  LOCK_SCOPE_SHARED,
} LockScopeType;

typedef enum _LockType {
  LOCK_NONE,
  LOCK_WRITE,
} LockType;

typedef enum _DepthType {
  DEPTH_ZERO,
  DEPTH_ONE,
  DEPTH_INFINITY
} DepthType;

struct _DAVLock
{
  Path         *path;
  gchar         token[45];
  LockScopeType scope;
  LockType      type;
  DepthType     depth;
  xmlNodePtr    owner;
  guint64       timeout;
};

struct _Path
{
  gchar  *path;
  GList  *locks;
  guint32 refs;
};

G_END_DECLS

#endif /* __PHODAV_PRIV_H__ */
