/*
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "config.h"

#include "phodav-path.h"

#include "phodav-lock.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>

#ifdef G_OS_WIN32
#include <io.h>
#define phodav_fsync _commit
#else
#include <unistd.h>
#define phodav_fsync fsync
#endif

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

gint
phodav_status_from_error (const GError *error)
{
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
      g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    return SOUP_STATUS_NOT_FOUND;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_READ_ONLY) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    return SOUP_STATUS_FORBIDDEN;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    return SOUP_STATUS_METHOD_NOT_ALLOWED;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE))
    return SOUP_STATUS_CONFLICT;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE))
    return SOUP_STATUS_INSUFFICIENT_STORAGE;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return SOUP_STATUS_SERVICE_UNAVAILABLE;

  return SOUP_STATUS_INTERNAL_SERVER_ERROR;
}

GFile *
phodav_path_resolve (PathHandler *handler, const gchar *path, PhodavPathMode mode, gint *status)
{
  const gchar *relative = handler_get_relative_path (handler, path);
  g_auto(GStrv) parts = NULL;
  g_autoptr(GFile) current = NULL;
  gint last = -1;
  gint i;

  if (!relative)
    {
      *status = SOUP_STATUS_FORBIDDEN;
      return NULL;
    }

  current = g_object_ref (handler_get_root_file (handler));
  parts = g_strsplit (relative, "/", -1);

  for (i = 0; parts[i]; i++)
    if (*parts[i])
      last = i;

  for (i = 0; parts[i]; i++)
    {
      g_autoptr(GFile) next = NULL;
      g_autoptr(GFileInfo) info = NULL;
      g_autoptr(GError) error = NULL;

      if (!*parts[i])
        continue;

      next = g_file_get_child (current, parts[i]);
      info = g_file_query_info (next, "standard::type",
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                NULL, &error);

      if (!info)
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              if (mode == PHODAV_PATH_CREATE && i == last)
                {
                  g_set_object (&current, next);
                  break;
                }

              *status = mode == PHODAV_PATH_CREATE
                          ? SOUP_STATUS_CONFLICT
                          : SOUP_STATUS_NOT_FOUND;
              return NULL;
            }

          *status = phodav_status_from_error (error);
          return NULL;
        }

      if (g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK)
        {
          *status = SOUP_STATUS_FORBIDDEN;
          return NULL;
        }

      if (i != last &&
          g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
        {
          *status = SOUP_STATUS_CONFLICT;
          return NULL;
        }

      g_set_object (&current, next);
    }

  return g_steal_pointer (&current);
}

gboolean
phodav_path_sync (GFile *file, GCancellable *cancellable, GError **error)
{
  g_autofree gchar *path = g_file_get_path (file);
  int fd;

  /* a generic backend has no descriptor to sync: closing its output
   * stream already flushed the upload to the backend */
  if (!path || !g_file_is_native (file))
    return TRUE;

  fd = g_open (path, O_RDONLY, 0);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot open %s for syncing: %s", path, g_strerror (errno));
      return FALSE;
    }

  if (phodav_fsync (fd) < 0)
    {
      gint saved_errno = errno;

      close (fd);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Cannot sync %s: %s", path, g_strerror (saved_errno));
      return FALSE;
    }

  close (fd);
  return TRUE;
}

gboolean
phodav_path_rename (GFile *from, GFile *to, GCancellable *cancellable, GError **error)
{
  if (!phodav_path_sync (from, cancellable, error))
    return FALSE;

  return g_file_move (from, to, G_FILE_COPY_OVERWRITE,
                      cancellable, NULL, NULL, error);
}
