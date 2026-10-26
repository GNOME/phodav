/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include "phodav-backend.h"

#include <errno.h>
#include <fcntl.h>

#include <glib/gstdio.h>

#include "phodav-virtual-dir.h"

#ifdef G_OS_WIN32
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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
#endif

PhodavBackend *
phodav_backend_new (GFile *root)
{
#ifdef HAVE_DIRFD_OPERATIONS
  /* the descriptor backend is the only one which can enforce the
   * no-symlink policy atomically, use it whenever the root is native */
  if (proc_fd_available () && root &&
      g_file_is_native (root) && !PHODAV_IS_VIRTUAL_DIR (root))
    return phodav_backend_native_new (root);
#endif

  if (PHODAV_IS_VIRTUAL_DIR (root))
    return phodav_backend_virtual_new (root);

  return phodav_backend_gfile_new (root);
}

PhodavBackend *
phodav_backend_ref (PhodavBackend *backend)
{
  /* a backend is shared by every path resolved from a handler, and
   * those paths are created and released on the worker pools */
  g_atomic_int_inc (&backend->ref_count);

  return backend;
}

void
phodav_backend_unref (PhodavBackend *backend)
{
  if (!backend || !g_atomic_int_dec_and_test (&backend->ref_count))
    return;

  if (backend->klass->destroy)
    backend->klass->destroy (backend);

  g_clear_object (&backend->root);
  g_free (backend);
}

gint
phodav_path_status_from_errno (gint errsv, PhodavPathMode mode, gboolean last)
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

void
phodav_path_set_error_from_errno (GError **error, gint errsv, const gchar *what)
{
  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
               "%s: %s", what, g_strerror (errsv));
}

#ifdef HAVE_DIRFD_OPERATIONS
static void
close_fd_data (gpointer data)
{
  gint fd = GPOINTER_TO_INT (data) - 1;

  if (fd >= 0)
    close (fd);
}

/* the /proc path is only meaningful while its descriptor is open: let
 * the GFile own a descriptor, so it stays valid after the handle is gone */
static GFile *
proc_fd_file (gint fd, const gchar *name)
{
  gint copy = fcntl (fd, F_DUPFD_CLOEXEC, 0);
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

PhodavPath *
phodav_path_new (PhodavBackend *backend,
                 GFile *file,
                 gint fd,
                 gint parent_fd,
                 const gchar *name)
{
  PhodavPath *path = g_new0 (PhodavPath, 1);

  path->ref_count = 1;
  path->backend = phodav_backend_ref (backend);
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

/* Turn an already validated GFile into a handle. A native object keeps
 * descriptors of itself and of its parent, so later operations do not
 * resolve its path again. */
PhodavPath *
phodav_path_new_from_gfile (PhodavBackend *backend,
                            GFile *file,
                            PhodavPathMode mode,
                            gint *status)
{
  g_autoptr(GFile) parent = NULL;
  gint fd = -1;
  gint parent_fd = -1;
  gchar *name = NULL;

  (void) status;
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
    PhodavPath *path = phodav_path_new (backend, file, fd, parent_fd, name);

    g_free (name);
    return path;
  }
}

GFile *
phodav_path_query_file (PhodavPath *path)
{
  /* the named entry is the object the request selected; for the root
   * there is no parent entry, use the held object itself */
  if (path->name && path->named_file)
    return path->named_file;

  return path->read_file ?: path->named_file ?
                                             : path->file;
}

#ifdef HAVE_DIRFD_OPERATIONS
static gboolean
unlink_entry (gint parent_fd, const gchar *name, GError **error)
{
  struct stat st;
  gint flags = 0;

  if (fstatat (parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISDIR (st.st_mode))
    flags = AT_REMOVEDIR;

  if (unlinkat (parent_fd, name, flags) < 0)
    {
      phodav_path_set_error_from_errno (error, errno,
                                        "Cannot remove the resource");
      return FALSE;
    }

  return TRUE;
}
#endif

gboolean
phodav_backend_mkdir_at (PhodavPath *path,
                         GCancellable *cancellable,
                         GError **error)
{
#ifdef HAVE_DIRFD_OPERATIONS
  if (path->parent_fd >= 0 && path->name)
    {
      if (mkdirat (path->parent_fd, path->name, 0777) < 0)
        {
          phodav_path_set_error_from_errno (error, errno,
                                            "Cannot create the directory");
          return FALSE;
        }

      return TRUE;
    }
#endif

  return phodav_backend_gfile_mkdir (path, cancellable, error);
}

gboolean
phodav_backend_unlink_at (PhodavPath *path,
                          GCancellable *cancellable,
                          GError **error)
{
#ifdef HAVE_DIRFD_OPERATIONS
  if (path->parent_fd >= 0 && path->name)
    return unlink_entry (path->parent_fd, path->name, error);
#endif

  return phodav_backend_gfile_unlink (path, cancellable, error);
}

gboolean
phodav_backend_unlink_child_at (PhodavPath *parent,
                                const gchar *name,
                                GCancellable *cancellable,
                                GError **error)
{
#ifdef HAVE_DIRFD_OPERATIONS
  gint dirfd = phodav_path_dir_fd (parent);

  if (dirfd >= 0)
    return unlink_entry (dirfd, name, error);
#endif

  return phodav_backend_gfile_unlink_child (parent, name,
                                            cancellable, error);
}

gboolean
phodav_backend_move_at (PhodavPath *from,
                        PhodavPath *to,
                        GFileCopyFlags flags,
                        GCancellable *cancellable,
                        GError **error)
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
          phodav_path_set_error_from_errno (error, errno,
                                            "Cannot rename the resource");
          return FALSE;
        }
    }
#endif

  return phodav_backend_gfile_move (from, to, flags, cancellable, error);
}

gint
phodav_path_dir_fd (PhodavPath *path)
{
#ifdef HAVE_DIRFD_OPERATIONS
  gint fd;

  if (path->fd >= 0)
    return path->fd;

  if (path->parent_fd < 0 || !path->name)
    return -1;

  fd = openat (path->parent_fd, path->name,
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd >= 0)
    path->fd = fd;

  return fd;
#else
  (void) path;
  return -1;
#endif
}
