/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the raw POSIX extended attribute operations, shared by the PROPFIND
 * and PROPPATCH implementations. The platform differences (Apple
 * argument lists, the errno of a missing attribute) live here only. */

/* the errno of a missing extended attribute */
gint phodav_xattr_no_attr_errno (void);

/* getxattr(), with the Apple and non-Apple signatures hidden */
gssize phodav_xattr_get (const gchar *path, const gchar *name, gpointer value, gsize size);

/* removexattr(), mapped to a SOUP_STATUS_* value. Removing an
 * attribute which is not set is not an error: PROPPATCH removals are
 * idempotent. */
gint phodav_xattr_remove (const gchar *path, const gchar *name);

G_END_DECLS
