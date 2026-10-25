/*
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "config.h"

#include "phodav-path.h"

#include "phodav-lock.h"
#include "phodav-virtual-dir.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>

#ifdef G_OS_WIN32
#include <io.h>
#define phodav_fsync _commit
#else
#include <sys/stat.h>
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

struct _PhodavPath
{
  gint ref_count;
  GFile *file;       /* the resource as it was named */
  GFile *read_file;  /* the held object, or @file without a descriptor */
  GFile *named_file; /* the entry below the held parent, or @read_file */
  gchar *name;       /* the final component, NULL for the root */
  int fd;            /* descriptor of the resolved object, or -1 */
  int parent_fd;     /* descriptor of its parent directory, or -1 */
};

static gboolean
proc_fd_available (void)
{
#if defined(__linux__)
  static gsize available = 0;

  if (g_once_init_enter (&available))
    g_once_init_leave (&available,
                       g_file_test ("/proc/self/fd", G_FILE_TEST_IS_DIR) ? 2 : 1);

  return available == 2;
#else
  return FALSE;
#endif
}

static gint
status_from_errno (gint errsv, PhodavPathMode mode, gboolean last)
{
  if (errsv == ENOENT)
    {
      if (mode == PHODAV_PATH_CREATE)
        return last ? SOUP_STATUS_NOT_FOUND : SOUP_STATUS_CONFLICT;
      return SOUP_STATUS_NOT_FOUND;
    }

  if (errsv == EACCES || errsv == EPERM || errsv == ELOOP)
    return SOUP_STATUS_FORBIDDEN;
  if (errsv == ENOTDIR)
    return SOUP_STATUS_CONFLICT;

  return SOUP_STATUS_INTERNAL_SERVER_ERROR;
}

#ifdef G_OS_UNIX
static void
close_fd_data (gpointer data)
{
  int fd = GPOINTER_TO_INT (data) - 1;

  if (fd >= 0)
    close (fd);
}

/* the /proc path is only meaningful while its descriptor is open: let
 * the GFile own a descriptor, so it stays valid after the handle is gone */
static GFile *
proc_fd_file (int fd, const gchar *name)
{
  int copy = fcntl (fd, F_DUPFD_CLOEXEC, 0);
  g_autofree gchar *proc = NULL;
  g_autoptr(GFile) parent = NULL;
  GFile *file;
  struct stat st;

  if (copy < 0)
    return NULL;

  if (!name && fstat (copy, &st) == 0 && S_ISDIR (st.st_mode))
    {
      /* end the path on a real directory component, so that a
       * NOFOLLOW query or enumeration sees the held directory rather
       * than the /proc link to it */
      proc = g_strdup_printf ("/proc/self/fd/%d/.", copy);
      file = g_file_new_for_path (proc);
    }
  else
    {
      proc = g_strdup_printf ("/proc/self/fd/%d", copy);
      parent = g_file_new_for_path (proc);
      file = name ? g_file_get_child (parent, name) : g_object_ref (parent);
    }

  g_object_set_data_full (G_OBJECT (file), "phodav-fd",
                          GINT_TO_POINTER (copy + 1), close_fd_data);

  return file;
}
#endif

static PhodavPath *
path_handle_new (GFile *file, int fd, int parent_fd, const gchar *name)
{
  PhodavPath *path = g_new0 (PhodavPath, 1);

  path->ref_count = 1;
  path->file = file ? g_object_ref (file) : NULL;
  path->fd = fd;
  path->parent_fd = parent_fd;
  path->name = g_strdup (name);

  if (parent_fd >= 0 && name && proc_fd_available ())
    {
#ifdef G_OS_UNIX
      path->named_file = proc_fd_file (parent_fd, name);
#else
      path->named_file = NULL;
#endif
    }

  if (fd >= 0 && proc_fd_available ())
    {
#ifdef G_OS_UNIX
      path->read_file = proc_fd_file (fd, NULL);
#else
      path->read_file = NULL;
#endif
    }

  if (!path->read_file)
    {
      if (fd < 0 && path->parent_fd >= 0 && path->name)
        path->read_file = g_object_ref (path->named_file);
      else if (path->file)
        path->read_file = g_object_ref (path->file);
    }

  if (!path->named_file)
    {
      if (path->read_file)
        path->named_file = g_object_ref (path->read_file);
      else if (path->file)
        path->named_file = g_object_ref (path->file);
    }

  return path;
}

PhodavPath *
phodav_path_ref (PhodavPath *path)
{
  g_atomic_int_inc (&path->ref_count);

  return path;
}

void
phodav_path_unref (PhodavPath *path)
{
  if (!path || !g_atomic_int_dec_and_test (&path->ref_count))
    return;

  g_clear_object (&path->file);
  g_clear_object (&path->read_file);
  g_clear_object (&path->named_file);
  g_free (path->name);
#ifdef G_OS_UNIX
  if (path->fd >= 0)
    close (path->fd);
  if (path->parent_fd >= 0)
    close (path->parent_fd);
#endif
  g_free (path);
}

GFile *
phodav_path_get_file (PhodavPath *path)
{
  return path->read_file;
}

GFile *
phodav_path_get_named_file (PhodavPath *path)
{
  return path->named_file;
}

const gchar *
phodav_path_get_name (PhodavPath *path)
{
  return path->name;
}

int
phodav_path_get_fd (PhodavPath *path)
{
  return path->fd;
}

static GFile *resolve_gfile (PathHandler *handler, const gchar *path, PhodavPathMode mode, gint *status);

static PhodavPath *
resolve_native (PathHandler *handler, const gchar *relative, PhodavPathMode mode, gint *status)
{
#ifdef G_OS_UNIX
  g_autofree gchar *root = g_file_get_path (handler_get_root_file (handler));
  g_auto(GStrv) parts = g_strsplit (relative, "/", -1);
  gboolean creating = FALSE;
  gchar *name = NULL;
  int dirfd = -1;
  int fd = -1;
  gint last = -1;
  gint i;

  if (!root)
    return NULL;

  dirfd = open (root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirfd < 0)
    {
      *status = status_from_errno (errno, mode, FALSE);
      return NULL;
    }

  for (i = 0; parts[i]; i++)
    if (*parts[i])
      last = i;

  for (i = 0; parts[i]; i++)
    {
      gint next;

      if (!*parts[i])
        continue;

      if (i == last)
        {
          fd = openat (dirfd, parts[i],
                       O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
          if (fd >= 0)
            {
              name = g_strdup (parts[i]);
              break;
            }

          if (errno == ENOENT && mode == PHODAV_PATH_CREATE)
            {
              creating = TRUE;
              name = g_strdup (parts[i]);
              break;
            }

          *status = status_from_errno (errno, mode, TRUE);
          close (dirfd);
          return NULL;
        }

      next = openat (dirfd, parts[i],
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0)
        {
          *status = status_from_errno (errno, mode, FALSE);
          close (dirfd);
          return NULL;
        }

      close (dirfd);
      dirfd = next;
    }

  if (!name)
    {
      /* the root itself: keep the directory descriptor, there is no
       * parent entry to name it by */
      fd = dirfd;
      dirfd = -1;
    }

  if (creating)
    {
      PhodavPath *result = path_handle_new (NULL, -1, dirfd, name);

      g_free (name);
      return result;
    }

  {
    PhodavPath *result = path_handle_new (NULL, fd, dirfd, name);

    g_free (name);
    return result;
  }
#else
  return NULL;
#endif
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

static PhodavPath *
resolve_gfile_handle (PathHandler *handler, const gchar *path, PhodavPathMode mode, gint *status)
{
  g_autoptr(GFile) file = resolve_gfile (handler, path, mode, status);
  g_autoptr(GFile) parent = NULL;
  int fd = -1;
  int parent_fd = -1;
  gchar *name = NULL;

  if (!file)
    return NULL;

#ifdef G_OS_UNIX
  /* the generic backend may still resolve to a native object (a real
   * child of a virtual directory): keep its descriptor too */
  if (g_file_is_native (file) && !PHODAV_IS_VIRTUAL_DIR (file))
    {
      g_autofree gchar *full = g_file_get_path (file);

      if (full)
        {
          if (mode == PHODAV_PATH_EXISTING)
            fd = open (full, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);

          parent = g_file_get_parent (file);
          if (parent)
            {
              g_autofree gchar *parent_path = g_file_get_path (parent);

              if (parent_path)
                parent_fd = open (parent_path,
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            }

          name = g_file_get_basename (file);
        }
    }
#endif

  {
    PhodavPath *result = path_handle_new (file, fd, parent_fd, name);

    g_free (name);
    return result;
  }
}

PhodavPath *
phodav_path_resolve (PathHandler *handler, const gchar *path, PhodavPathMode mode, gint *status)
{
  const gchar *relative = handler_get_relative_path (handler, path);
  PhodavPath *result;

  if (!relative)
    {
      *status = SOUP_STATUS_FORBIDDEN;
      return NULL;
    }

  if (proc_fd_available () &&
      g_file_is_native (handler_get_root_file (handler)))
    {
      result = resolve_native (handler, relative, mode, status);
      if (result)
        return result;
    }

  return resolve_gfile_handle (handler, path, mode, status);
}

static GFile *
resolve_gfile (PathHandler *handler, const gchar *path, PhodavPathMode mode, gint *status)
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
