/*
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
/* keep the feature-test macro in sync with the meson check for the
 * descriptor-relative operations */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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

#ifdef HAVE_DIRFD_OPERATIONS
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

#ifdef HAVE_DIRFD_OPERATIONS
  if (parent_fd >= 0 && name && proc_fd_available ())
    path->named_file = proc_fd_file (parent_fd, name);

  if (fd >= 0 && proc_fd_available ())
    path->read_file = proc_fd_file (fd, NULL);
#endif

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

#ifdef HAVE_DIRFD_OPERATIONS
static PhodavPath *
resolve_native (PathHandler *handler, const gchar *relative, PhodavPathMode mode, gint *status)
{
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
          gint errsv = errno;
          struct stat st;

          /* O_NOFOLLOW with O_DIRECTORY may report a symlink as
           * ENOTDIR instead of ELOOP: keep the same policy as for a
           * final symlink component */
          if ((errsv == ENOTDIR || errsv == ELOOP) &&
              fstatat (dirfd, parts[i], &st, AT_SYMLINK_NOFOLLOW) == 0 &&
              S_ISLNK (st.st_mode))
            *status = SOUP_STATUS_FORBIDDEN;
          else
            *status = status_from_errno (errsv, mode, FALSE);
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
}
#endif

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

/* turn an already validated GFile into a handle. A native object keeps
 * descriptors of itself and of its parent, so later operations do not
 * resolve its path again. */
static PhodavPath *
path_handle_from_gfile (GFile *file, PhodavPathMode mode, gint *status)
{
  g_autoptr(GFile) parent = NULL;
  int fd = -1;
  int parent_fd = -1;
  gchar *name = NULL;

#ifdef HAVE_DIRFD_OPERATIONS
  /* the generic backend may still resolve to a native object (a real
   * child of a virtual directory): keep its descriptor too */
  if (proc_fd_available () &&
      g_file_is_native (file) && !PHODAV_IS_VIRTUAL_DIR (file))
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

static PhodavPath *
resolve_gfile_handle (PathHandler *handler, const gchar *path, PhodavPathMode mode, gint *status)
{
  g_autoptr(GFile) file = resolve_gfile (handler, path, mode, status);

  if (!file)
    return NULL;

  return path_handle_from_gfile (file, mode, status);
}

PhodavPath *
phodav_path_resolve (PathHandler *handler, const gchar *path, PhodavPathMode mode, gint *status)
{
  const gchar *relative = handler_get_relative_path (handler, path);

  if (!relative)
    {
      *status = SOUP_STATUS_FORBIDDEN;
      return NULL;
    }

#ifdef HAVE_DIRFD_OPERATIONS
  /* a native root stays on the descriptor-based resolution: falling
   * back to a path-based lookup after it failed would bypass the
   * symlink and existence policy it enforces */
  if (proc_fd_available () &&
      g_file_is_native (handler_get_root_file (handler)))
    return resolve_native (handler, relative, mode, status);
#endif

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

static gboolean
path_name_is_valid (const gchar *name)
{
  return name && *name &&
         g_strcmp0 (name, ".") != 0 &&
         g_strcmp0 (name, "..") != 0 &&
         strchr (name, '/') == NULL;
}

#ifdef HAVE_DIRFD_OPERATIONS
static void
set_error_from_errno (GError **error, gint errsv, const gchar *what)
{
  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
               "%s: %s", what, g_strerror (errsv));
}

/* the descriptor of a directory handle, opening it by name below its
 * held parent when the handle was created for a new entry */
static int
path_dir_fd (PhodavPath *path)
{
  int fd;

  if (path->fd >= 0)
    return path->fd;

  if (path->parent_fd < 0 || !path->name)
    return -1;

  fd = openat (path->parent_fd, path->name,
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd >= 0)
    path->fd = fd;

  return fd;
}

static PhodavPath *
resolve_native_child (PhodavPath *parent, const gchar *name,
                      PhodavPathMode mode, gint *status)
{
  int dirfd = path_dir_fd (parent);
  int fd;
  int parent_fd;
  int errsv;

  if (dirfd < 0)
    {
      *status = status_from_errno (errno, mode, FALSE);
      return NULL;
    }

  fd = openat (dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd >= 0)
    {
      parent_fd = fcntl (dirfd, F_DUPFD_CLOEXEC, 0);
      if (parent_fd < 0)
        {
          errsv = errno;
          close (fd);
          *status = status_from_errno (errsv, mode, TRUE);
          return NULL;
        }

      return path_handle_new (NULL, fd, parent_fd, name);
    }

  errsv = errno;
  if (errsv == ENOENT && mode == PHODAV_PATH_CREATE)
    {
      parent_fd = fcntl (dirfd, F_DUPFD_CLOEXEC, 0);
      if (parent_fd < 0)
        {
          *status = status_from_errno (errno, mode, TRUE);
          return NULL;
        }

      return path_handle_new (NULL, -1, parent_fd, name);
    }

  *status = status_from_errno (errsv, mode, TRUE);
  return NULL;
}

#endif

static PhodavPath *
resolve_gfile_child (PhodavPath *parent, const gchar *name,
                     PhodavPathMode mode, gint *status)
{
  GFile *base = parent->read_file ?: parent->named_file ?: parent->file;
  g_autoptr(GFile) child = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GError) error = NULL;

  if (!base)
    {
      *status = SOUP_STATUS_NOT_FOUND;
      return NULL;
    }

  child = g_file_get_child (base, name);
  info = g_file_query_info (child, "standard::type",
                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                            NULL, &error);
  if (!info)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
          mode == PHODAV_PATH_CREATE)
        return path_handle_from_gfile (child, mode, status);

      *status = phodav_status_from_error (error);
      return NULL;
    }

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      *status = SOUP_STATUS_FORBIDDEN;
      return NULL;
    }

  return path_handle_from_gfile (child, mode, status);
}

/* resolve one direct child by name, below the held object rather than
 * by rebuilding its path. Native handles use openat(), so a renamed or
 * replaced ancestor cannot redirect the lookup. */
PhodavPath *
phodav_path_resolve_child (PhodavPath *parent, const gchar *name,
                           PhodavPathMode mode, gint *status)
{
  g_return_val_if_fail (parent != NULL, NULL);

  if (!path_name_is_valid (name))
    {
      *status = SOUP_STATUS_FORBIDDEN;
      return NULL;
    }

#ifdef HAVE_DIRFD_OPERATIONS
  if (proc_fd_available () &&
      (parent->fd >= 0 || (parent->parent_fd >= 0 && parent->name)))
    return resolve_native_child (parent, name, mode, status);
#endif

  return resolve_gfile_child (parent, name, mode, status);
}

static GFile *
path_query_file (PhodavPath *path)
{
  /* the named entry is the object the request selected; for the root
   * there is no parent entry, use the held object itself */
  if (path->name && path->named_file)
    return path->named_file;

  return path->read_file ?: path->named_file ?: path->file;
}

GFileInfo *
phodav_path_query_info (PhodavPath *path, const gchar *attributes,
                        GFileQueryInfoFlags flags,
                        GCancellable *cancellable, GError **error)
{
  GFile *file = path_query_file (path);

  if (!file)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "The path has no filesystem object");
      return NULL;
    }

  /* the root handle has no named entry: its GFile is the /proc link
   * to the held descriptor, which must be followed to reach the
   * object itself */
  if (!path->name && path->fd >= 0)
    flags &= ~G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS;

  return g_file_query_info (file, attributes, flags, cancellable, error);
}

GFileInputStream *
phodav_path_open (PhodavPath *path, GCancellable *cancellable, GError **error)
{
  GFile *file = path->read_file ?: path->named_file ?: path->file;

  if (!file)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "The path has no filesystem object");
      return NULL;
    }

  return g_file_read (file, cancellable, error);
}

GFileEnumerator *
phodav_path_enumerate (PhodavPath *path, const gchar *attributes,
                       GFileQueryInfoFlags flags,
                       GCancellable *cancellable, GError **error)
{
  GFile *file = path->read_file ?: path->named_file ?: path->file;

  if (!file)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "The path has no filesystem object");
      return NULL;
    }

  return g_file_enumerate_children (file, attributes, flags, cancellable, error);
}

gboolean
phodav_path_mkdir (PhodavPath *path, GCancellable *cancellable, GError **error)
{
#ifdef HAVE_DIRFD_OPERATIONS
  if (path->parent_fd >= 0 && path->name)
    {
      if (mkdirat (path->parent_fd, path->name, 0777) < 0)
        {
          set_error_from_errno (error, errno, "Cannot create the directory");
          return FALSE;
        }

      return TRUE;
    }
#endif

  return g_file_make_directory (path->named_file ?: path->file,
                                cancellable, error);
}

#ifdef HAVE_DIRFD_OPERATIONS
static gboolean
path_unlink_at (int parent_fd, const gchar *name,
                GCancellable *cancellable, GError **error)
{
  struct stat st;
  int flags = 0;

  if (fstatat (parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISDIR (st.st_mode))
    flags = AT_REMOVEDIR;

  if (unlinkat (parent_fd, name, flags) < 0)
    {
      set_error_from_errno (error, errno, "Cannot remove the resource");
      return FALSE;
    }

  return TRUE;
}
#endif

gboolean
phodav_path_unlink_child (PhodavPath *parent, const gchar *name,
                          GCancellable *cancellable, GError **error)
{
#ifdef HAVE_DIRFD_OPERATIONS
  int dirfd = path_dir_fd (parent);

  if (dirfd >= 0)
    return path_unlink_at (dirfd, name, cancellable, error);
#endif

  {
    GFile *base = parent->read_file ?: parent->named_file ?: parent->file;
    g_autoptr(GFile) child = NULL;

    if (!base)
      {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "The path has no filesystem object");
        return FALSE;
      }

    child = g_file_get_child (base, name);
    return g_file_delete (child, cancellable, error);
  }
}

gboolean
phodav_path_unlink (PhodavPath *path, GCancellable *cancellable, GError **error)
{
#ifdef HAVE_DIRFD_OPERATIONS
  if (path->parent_fd >= 0 && path->name)
    return path_unlink_at (path->parent_fd, path->name, cancellable, error);
#endif

  return g_file_delete (path->read_file ?: path->named_file ?: path->file,
                        cancellable, error);
}

typedef struct _DeleteTree
{
  PhodavServer *server;
  PhodavPathDeleteNotify notify;
  gpointer data;
  GCancellable *cancellable;
} DeleteTree;

/* the entry of @path was removed: drop the descriptor and the read
 * GFile of the old object, so the handle can be used again to create
 * the same name below its parent */
static void
path_invalidate (PhodavPath *path)
{
#ifdef HAVE_DIRFD_OPERATIONS
  if (path->fd >= 0)
    {
      close (path->fd);
      path->fd = -1;
    }
#endif
  g_clear_object (&path->read_file);
  if (path->named_file)
    path->read_file = g_object_ref (path->named_file);
  else if (path->file)
    path->read_file = g_object_ref (path->file);
}

static gint
delete_tree (DeleteTree *ctx, PhodavPath *path, const gchar *rel_path)
{
  g_autoptr(GFileEnumerator) e = NULL;
  g_autoptr(GError) error = NULL;
  gint status = SOUP_STATUS_NO_CONTENT;

  e = phodav_path_enumerate (path, "standard::*",
                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                             ctx->cancellable, NULL);
  if (e)
    {
      while (TRUE)
        {
          g_autoptr(GFileInfo) info = NULL;
          const gchar *name;
          g_autofree gchar *child_path = NULL;

          info = g_file_enumerator_next_file (e, ctx->cancellable, &error);
          if (!info)
            break;

          name = g_file_info_get_name (info);
          child_path = g_build_path ("/", rel_path, name, NULL);

          if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
            {
              g_autoptr(PhodavPath) child = NULL;
              gint child_status;

              child = phodav_path_resolve_child (path, name,
                                                 PHODAV_PATH_EXISTING,
                                                 &child_status);
              if (!child)
                {
                  if (ctx->notify)
                    ctx->notify (child_path, child_status, ctx->data);
                  continue;
                }

              delete_tree (ctx, child, child_path);
            }
          else
            {
              if (!phodav_path_unlink_child (path, name,
                                             ctx->cancellable, &error))
                {
                  if (ctx->notify)
                    ctx->notify (child_path,
                                 phodav_status_from_error (error),
                                 ctx->data);
                  g_clear_error (&error);
                }
            }
        }

      if (error)
        {
          g_debug ("DELETE: enumeration error: %s", error->message);
          g_clear_error (&error);
        }

      g_file_enumerator_close (e, ctx->cancellable, NULL);
    }

  if (!phodav_path_unlink (path, ctx->cancellable, &error))
    {
      status = phodav_status_from_error (error);
      if (ctx->notify)
        ctx->notify (rel_path, status, ctx->data);
      g_clear_error (&error);
      return status;
    }

  path_invalidate (path);
  phodav_lock_store_remove_locks (ctx->server, rel_path);
  return status;
}

gint
phodav_path_delete_tree (PhodavServer *server, PhodavPath *path,
                         const gchar *rel_path,
                         PhodavPathDeleteNotify notify, gpointer data,
                         GCancellable *cancellable)
{
  DeleteTree ctx = {
    .server = server,
    .notify = notify,
    .data = data,
    .cancellable = cancellable,
  };

  return delete_tree (&ctx, path, rel_path);
}

static gboolean
path_copy_tree (PhodavPath *from, PhodavPath *to, GFileCopyFlags flags,
                gboolean recursive, GCancellable *cancellable, GError **error)
{
  g_autoptr(GFileInfo) info = NULL;
  GFileType type;

  info = phodav_path_query_info (from, "standard::type",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!info)
    return FALSE;

  type = g_file_info_get_file_type (info);

  if (type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "COPY does not follow symbolic links");
      return FALSE;
    }

  if (type != G_FILE_TYPE_DIRECTORY)
    {
      GFile *source = from->read_file ?: from->named_file ?: from->file;
      GFile *dest = to->named_file ?: to->read_file ?: to->file;

      if (!source || !dest)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "The path has no filesystem object");
          return FALSE;
        }

      return g_file_copy (source, dest, flags, cancellable, NULL, NULL, error);
    }

  if (!phodav_path_mkdir (to, cancellable, error))
    return FALSE;

  if (!recursive)
    return TRUE;

#ifdef HAVE_DIRFD_OPERATIONS
  if (path_dir_fd (from) < 0 || path_dir_fd (to) < 0)
    {
      set_error_from_errno (error, errno, "Cannot open the copied directory");
      return FALSE;
    }
#endif

  {
    g_autoptr(GFileEnumerator) e = NULL;
    g_autoptr(GError) enumerate_error = NULL;

    e = phodav_path_enumerate (from, "standard::*",
                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                               cancellable, error);
    if (!e)
      return FALSE;

    while (TRUE)
      {
        g_autoptr(GFileInfo) child_info = NULL;
        g_autoptr(PhodavPath) src_child = NULL;
        g_autoptr(PhodavPath) dest_child = NULL;
        const gchar *name;
        gint status;

        child_info = g_file_enumerator_next_file (e, cancellable, &enumerate_error);
        if (!child_info)
          break;

        name = g_file_info_get_name (child_info);
        src_child = phodav_path_resolve_child (from, name,
                                               PHODAV_PATH_EXISTING,
                                               &status);
        if (!src_child)
          {
            g_set_error (error, G_IO_ERROR,
                         status == SOUP_STATUS_FORBIDDEN
                           ? G_IO_ERROR_PERMISSION_DENIED
                           : G_IO_ERROR_FAILED,
                         "Cannot resolve the COPY source");
            return FALSE;
          }

        dest_child = phodav_path_resolve_child (to, name,
                                                PHODAV_PATH_CREATE,
                                                &status);
        if (!dest_child)
          {
            g_set_error (error, G_IO_ERROR,
                         status == SOUP_STATUS_FORBIDDEN
                           ? G_IO_ERROR_PERMISSION_DENIED
                           : G_IO_ERROR_FAILED,
                         "Cannot resolve the COPY destination");
            return FALSE;
          }

        if (!path_copy_tree (src_child, dest_child, flags,
                             recursive, cancellable, error))
          return FALSE;
      }

    if (enumerate_error)
      {
        g_propagate_error (error, g_steal_pointer (&enumerate_error));
        return FALSE;
      }

    g_file_enumerator_close (e, cancellable, NULL);
  }

  return TRUE;
}

gboolean
phodav_path_copy (PhodavPath *from, PhodavPath *to, GFileCopyFlags flags,
                  gboolean recursive, GCancellable *cancellable, GError **error)
{
  return path_copy_tree (from, to, flags, recursive, cancellable, error);
}

static gboolean
path_rename_handles (PhodavPath *from, PhodavPath *to, GFileCopyFlags flags,
                     GCancellable *cancellable, GError **error)
{
#ifdef HAVE_DIRFD_OPERATIONS
  if (!(flags & G_FILE_COPY_OVERWRITE))
    {
      struct stat st;

      if (to->parent_fd >= 0 && to->name &&
          fstatat (to->parent_fd, to->name, &st, AT_SYMLINK_NOFOLLOW) == 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                               "The destination already exists");
          return FALSE;
        }
    }

  if (from->parent_fd >= 0 && from->name &&
      to->parent_fd >= 0 && to->name)
    {
      if (renameat (from->parent_fd, from->name,
                    to->parent_fd, to->name) == 0)
        return TRUE;

      if (errno != EXDEV)
        {
          set_error_from_errno (error, errno, "Cannot rename the resource");
          return FALSE;
        }
    }
#endif

  return g_file_move (from->named_file ?: from->read_file,
                      to->named_file ?: to->read_file,
                      flags, cancellable, NULL, NULL, error);
}

gboolean
phodav_path_rename (PhodavPath *from, PhodavPath *to,
                    GCancellable *cancellable, GError **error)
{
  return path_rename_handles (from, to, G_FILE_COPY_OVERWRITE,
                              cancellable, error);
}

gboolean
phodav_path_move (PhodavPath *from, PhodavPath *to, GFileCopyFlags flags,
                  GCancellable *cancellable, GError **error)
{
  return path_rename_handles (from, to, flags, cancellable, error);
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

#ifdef G_OS_WIN32
  /* _commit() needs a writable handle on Windows */
  fd = g_open (path, O_RDWR, 0);
#else
  fd = g_open (path, O_RDONLY, 0);
#endif
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
phodav_path_rename_files (GFile *from, GFile *to,
                          GCancellable *cancellable, GError **error)
{
  /* sync the staged file before taking the filesystem write lock, see
   * phodav_path_sync() */
  return g_file_move (from, to, G_FILE_COPY_OVERWRITE,
                      cancellable, NULL, NULL, error);
}
