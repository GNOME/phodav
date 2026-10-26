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

#include <errno.h>
#include <fcntl.h>

#include <glib/gstdio.h>

#include "phodav-backend.h"
#include "phodav-lock.h"

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

PhodavPath *
phodav_path_ref (PhodavPath *path)
{
  path->ref_count++;

  return path;
}

void
phodav_path_unref (PhodavPath *path)
{
  if (!path || --path->ref_count > 0)
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
  g_clear_pointer (&path->backend, phodav_backend_unref);
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

gint
phodav_path_get_fd (PhodavPath *path)
{
  return path->fd;
}

/* The public resolution entry points below are the policy boundary:
 * the backend only reports whether the resource can be reached, while
 * the share-relative path validation, the requested existence mode and
 * the mapping of failures to HTTP status codes are shared here. */
PhodavPath *
phodav_path_resolve (PathHandler *handler, const gchar *path, PhodavPathMode mode, gint *status)
{
  const gchar *relative = handler_get_relative_path (handler, path);
  PhodavBackend *backend = handler_get_backend (handler);

  if (!relative)
    {
      *status = SOUP_STATUS_FORBIDDEN;
      return NULL;
    }

  return backend->klass->resolve (backend, handler, relative, mode, status);
}

static gboolean
path_name_is_valid (const gchar *name)
{
  return name && *name &&
         g_strcmp0 (name, ".") != 0 &&
         g_strcmp0 (name, "..") != 0 &&
         strchr (name, '/') == NULL;
}

/* resolve one direct child by name, below the held object rather than
 * by rebuilding its path. Native handles use openat(), so a renamed or
 * replaced ancestor cannot redirect the lookup. */
PhodavPath *
phodav_path_resolve_child (PhodavPath *parent, const gchar *name, PhodavPathMode mode, gint *status)
{
  g_return_val_if_fail (parent != NULL, NULL);

  if (!path_name_is_valid (name))
    {
      *status = SOUP_STATUS_FORBIDDEN;
      return NULL;
    }

  return parent->backend->klass->resolve_child (parent->backend, parent,
                                                name, mode, status);
}

GFileInfo *
phodav_path_query_info (PhodavPath *path, const gchar *attributes, GFileQueryInfoFlags flags, GCancellable *cancellable, GError **error)
{
  return path->backend->klass->query_info (path, attributes, flags,
                                           cancellable, error);
}

GFileInputStream *
phodav_path_open (PhodavPath *path, GCancellable *cancellable, GError **error)
{
  return path->backend->klass->open (path, cancellable, error);
}

GFileEnumerator *
phodav_path_enumerate (PhodavPath *path, const gchar *attributes, GFileQueryInfoFlags flags, GCancellable *cancellable, GError **error)
{
  return path->backend->klass->enumerate (path, attributes, flags,
                                          cancellable, error);
}

gboolean
phodav_path_mkdir (PhodavPath *path, GCancellable *cancellable, GError **error)
{
  return path->backend->klass->mkdir (path, cancellable, error);
}

gboolean
phodav_path_unlink_child (PhodavPath *parent, const gchar *name, GCancellable *cancellable, GError **error)
{
  return parent->backend->klass->unlink_child (parent, name,
                                               cancellable, error);
}

gboolean
phodav_path_unlink (PhodavPath *path, GCancellable *cancellable, GError **error)
{
  return path->backend->klass->unlink (path, cancellable, error);
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

typedef struct _DeleteTree
{
  PhodavPathDeleteNotify notify;
  gpointer data;
  GPtrArray *removed;
  GCancellable *cancellable;
} DeleteTree;

/* the entry of @path was removed: drop the descriptor and the read
 * GFile of the old object, so the handle can be used again to create
 * the same name below its parent */
static void
path_invalidate (PhodavPath *path)
{
#ifdef HAVE_DIRFD_OPERATIONS
  g_clear_fd (&path->fd, NULL);
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
  if (ctx->removed)
    g_ptr_array_add (ctx->removed, g_strdup (rel_path));
  return status;
}

gint
phodav_path_delete_tree (PhodavPath *path, const gchar *rel_path, PhodavPathDeleteNotify notify, gpointer data, GPtrArray *removed, GCancellable *cancellable)
{
  DeleteTree ctx = {
    .notify = notify,
    .data = data,
    .removed = removed,
    .cancellable = cancellable,
  };

  return delete_tree (&ctx, path, rel_path);
}

static gboolean
path_copy_tree (PhodavPath *from, PhodavPath *to, GFileCopyFlags flags, gboolean recursive, GCancellable *cancellable, GError **error)
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
    return from->backend->klass->copy_file (from, to, flags,
                                            cancellable, error);

  if (!phodav_path_mkdir (to, cancellable, error))
    return FALSE;

  if (!recursive)
    return TRUE;

#ifdef HAVE_DIRFD_OPERATIONS
  if (phodav_path_dir_fd (from) < 0 || phodav_path_dir_fd (to) < 0)
    {
      phodav_path_set_error_from_errno (error, errno,
                                        "Cannot open the copied directory");
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
phodav_path_copy (PhodavPath *from, PhodavPath *to, GFileCopyFlags flags, gboolean recursive, GCancellable *cancellable, GError **error)
{
  return path_copy_tree (from, to, flags, recursive, cancellable, error);
}

gboolean
phodav_path_rename (PhodavPath *from, PhodavPath *to, GCancellable *cancellable, GError **error)
{
  return from->backend->klass->move (from, to, G_FILE_COPY_OVERWRITE,
                                     cancellable, error);
}

gboolean
phodav_path_move (PhodavPath *from, PhodavPath *to, GFileCopyFlags flags, GCancellable *cancellable, GError **error)
{
  return from->backend->klass->move (from, to, flags,
                                     cancellable, error);
}

gboolean
phodav_path_sync (GFile *file, GCancellable *cancellable, GError **error)
{
  g_autofree gchar *path = g_file_get_path (file);
  gint fd;

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
phodav_path_rename_files (GFile *from, GFile *to, GCancellable *cancellable, GError **error)
{
  /* the staged file was already synced by phodav_path_sync(), see the
   * PUT commit job */
  return g_file_move (from, to, G_FILE_COPY_OVERWRITE,
                      cancellable, NULL, NULL, error);
}
