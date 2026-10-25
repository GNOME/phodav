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

#ifdef HAVE_DIRFD_OPERATIONS

#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#ifdef SYS_openat2
#include <linux/openat2.h>
#if defined(RESOLVE_BENEATH) && defined(RESOLVE_NO_SYMLINKS)
#define PHODAV_HAVE_OPENAT2 1
#endif
#endif
#endif

typedef struct _PhodavBackendNative
{
  PhodavBackend parent;
  int root_fd;
} PhodavBackendNative;

#ifdef PHODAV_HAVE_OPENAT2
/* Resolve a path relative to @dirfd without ever following a symbolic
 * link or escaping the directory: the kernel applies the policy to the
 * whole walk atomically. */
static int
openat2_beneath (int dirfd, const gchar *path, int flags)
{
  struct open_how how = {
    .flags = flags,
    .mode = 0,
    .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS,
  };

  return syscall (SYS_openat2, dirfd, path, &how, sizeof (how));
}

static gboolean
openat2_unsupported (int errsv)
{
  return errsv == ENOSYS || errsv == EINVAL || errsv == E2BIG;
}
#endif

static int
open_dir_fallback (int root_fd, const gchar *relative)
{
  g_auto(GStrv) parts = g_strsplit (relative, "/", -1);
  int dirfd = fcntl (root_fd, F_DUPFD_CLOEXEC, 0);
  gint i;

  if (dirfd < 0)
    return -1;

  for (i = 0; parts[i]; i++)
    {
      gint next;

      if (!*parts[i])
        continue;

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
            errno = ELOOP;

          close (dirfd);
          return -1;
        }

      close (dirfd);
      dirfd = next;
    }

  return dirfd;
}

/* open the directory containing the final component of @relative */
static int
open_parent_dir (PhodavBackendNative *self, const gchar *relative)
{
  g_auto(GStrv) parts = g_strsplit (relative, "/", -1);
  g_autoptr(GString) parent = g_string_new (NULL);
  gint last = -1;
  gint i;
  int dirfd;

  for (i = 0; parts[i]; i++)
    if (*parts[i])
      last = i;

  if (last <= 0)
    return fcntl (self->root_fd, F_DUPFD_CLOEXEC, 0);

  for (i = 0; i < last; i++)
    {
      if (!*parts[i])
        continue;

      if (parent->len)
        g_string_append_c (parent, '/');
      g_string_append (parent, parts[i]);
    }

  if (parent->len == 0)
    return fcntl (self->root_fd, F_DUPFD_CLOEXEC, 0);

#ifdef PHODAV_HAVE_OPENAT2
  dirfd = openat2_beneath (self->root_fd, parent->str,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirfd >= 0)
    return dirfd;

  if (!openat2_unsupported (errno))
    return -1;
#endif

  return open_dir_fallback (self->root_fd, parent->str);
}

static PhodavPath *
resolve_native (PhodavBackend *backend,
                PathHandler *handler,
                const gchar *relative,
                PhodavPathMode mode,
                gint *status)
{
  PhodavBackendNative *self = (PhodavBackendNative *) backend;
  g_auto(GStrv) parts = g_strsplit (relative, "/", -1);
  gchar *name = NULL;
  int parent_fd = -1;
  int fd = -1;
  gint last = -1;
  gint i;

  (void) handler;

  if (self->root_fd < 0)
    {
      *status = SOUP_STATUS_NOT_FOUND;
      return NULL;
    }

  for (i = 0; parts[i]; i++)
    if (*parts[i])
      last = i;

  if (last < 0)
    {
      /* the root itself: keep a descriptor, there is no parent entry
       * to name it by */
      fd = fcntl (self->root_fd, F_DUPFD_CLOEXEC, 0);
      if (fd < 0)
        {
          *status = phodav_path_status_from_errno (errno, mode, FALSE);
          return NULL;
        }

      return phodav_path_new (backend, self->parent.root, fd, -1, NULL);
    }

  parent_fd = open_parent_dir (self, relative);
  if (parent_fd < 0)
    {
      gint errsv = errno;

      *status = phodav_path_status_from_errno (errsv, mode, FALSE);
      return NULL;
    }

  name = g_strdup (parts[last]);
  fd = openat (parent_fd, name,
               O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    {
      gint errsv = errno;

      if (errsv == ENOENT && mode == PHODAV_PATH_CREATE)
        return phodav_path_new (backend, NULL, -1, parent_fd, name);

      close (parent_fd);
      g_free (name);
      *status = phodav_path_status_from_errno (errsv, mode, TRUE);
      return NULL;
    }

  {
    PhodavPath *path = phodav_path_new (backend, NULL, fd, parent_fd, name);

    g_free (name);
    return path;
  }
}

static PhodavPath *
resolve_native_child (PhodavBackend *backend,
                      PhodavPath *parent,
                      const gchar *name,
                      PhodavPathMode mode,
                      gint *status)
{
  int dirfd = phodav_path_dir_fd (parent);
  int fd;
  int parent_fd;
  int errsv;

  if (dirfd < 0)
    {
      *status = phodav_path_status_from_errno (errno, mode, FALSE);
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
          *status = phodav_path_status_from_errno (errsv, mode, TRUE);
          return NULL;
        }

      return phodav_path_new (backend, NULL, fd, parent_fd, name);
    }

  errsv = errno;
  if (errsv == ENOENT && mode == PHODAV_PATH_CREATE)
    {
      parent_fd = fcntl (dirfd, F_DUPFD_CLOEXEC, 0);
      if (parent_fd < 0)
        {
          *status = phodav_path_status_from_errno (errno, mode, TRUE);
          return NULL;
        }

      return phodav_path_new (backend, NULL, -1, parent_fd, name);
    }

  *status = phodav_path_status_from_errno (errsv, mode, TRUE);
  return NULL;
}

static void
native_destroy (PhodavBackend *backend)
{
  PhodavBackendNative *self = (PhodavBackendNative *) backend;

  if (self->root_fd >= 0)
    close (self->root_fd);
  self->root_fd = -1;
}

static const PhodavBackendClass native_backend_class = {
  .name = "native-dirfd",
  .resolve = resolve_native,
  .resolve_child = resolve_native_child,
  .query_info = phodav_backend_gfile_query_info,
  .open = phodav_backend_gfile_open,
  .enumerate = phodav_backend_gfile_enumerate,
  .mkdir = phodav_backend_mkdir_at,
  .unlink = phodav_backend_unlink_at,
  .unlink_child = phodav_backend_unlink_child_at,
  .copy_file = phodav_backend_gfile_copy_file,
  .move = phodav_backend_move_at,
  .destroy = native_destroy,
};

PhodavBackend *
phodav_backend_native_new (GFile *root)
{
  PhodavBackendNative *self = g_new0 (PhodavBackendNative, 1);
  g_autofree gchar *path = g_file_get_path (root);

  self->parent.klass = &native_backend_class;
  self->parent.ref_count = 1;
  self->parent.root = g_object_ref (root);
  self->root_fd = -1;

  if (path)
    self->root_fd = open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

  return &self->parent;
}

#else

PhodavBackend *
phodav_backend_native_new (GFile *root)
{
  return phodav_backend_gfile_new (root);
}

#endif
