/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-xattr.h"

#include <errno.h>

#include <libsoup/soup.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

gint
phodav_xattr_no_attr_errno (void)
{
#if defined(ENOATTR)
  return ENOATTR;
#elif defined(ENODATA)
  return ENODATA;
#else
  return ENOENT;
#endif
}

gssize
phodav_xattr_get (const gchar *path, const gchar *name, gpointer value, gsize size)
{
#ifdef HAVE_SYS_XATTR_H
#ifdef __APPLE__
  return getxattr (path, name, value, size, 0, 0);
#else
  return getxattr (path, name, value, size);
#endif
#else
  errno = ENOTSUP;
  return -1;
#endif
}

gint
phodav_xattr_remove (const gchar *path, const gchar *name)
{
#ifdef HAVE_SYS_XATTR_H
  if (!path)
    return SOUP_STATUS_FORBIDDEN;

#ifdef __APPLE__
  if (removexattr (path, name, 0) == 0)
#else
  if (removexattr (path, name) == 0)
#endif
    return SOUP_STATUS_OK;

  /* removing a property which is not set is not an error: PROPPATCH
   * removals are idempotent, and litmus deletes the same property
   * twice */
  if (errno == phodav_xattr_no_attr_errno ())
    return SOUP_STATUS_OK;

  if (errno == EACCES || errno == EPERM)
    return SOUP_STATUS_FORBIDDEN;

  if (errno == ENOENT)
    return SOUP_STATUS_NOT_FOUND;

  return SOUP_STATUS_INTERNAL_SERVER_ERROR;
#else
  g_debug ("cannot remove xattr from %s, not supported", path); /* FIXME? */
  return SOUP_STATUS_OK;
#endif
}
