/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <errno.h>

#include <glib/gstdio.h>

#include "phodav-backend.h"

#ifdef HAVE_DIRFD_OPERATIONS
#include <fcntl.h>
#include <unistd.h>
#endif

PhodavPath *
phodav_backend_gfile_resolve (PhodavBackend *backend,
                              PathHandler *handler,
                              const gchar *relative,
                              PhodavPathMode mode,
                              gint *status)
{
  g_auto(GStrv) parts = NULL;
  g_autoptr(GFile) current = NULL;
  gint last = -1;
  gint i;

  (void) handler;

  current = g_object_ref (backend->root);
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

  return phodav_path_new_from_gfile (backend, current, mode, status);
}

PhodavPath *
phodav_backend_gfile_resolve_child (PhodavBackend *backend,
                                    PhodavPath *parent,
                                    const gchar *name,
                                    PhodavPathMode mode,
                                    gint *status)
{
  GFile *base = parent->read_file ?: parent->named_file ?
                                                        : parent->file;
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
        return phodav_path_new_from_gfile (backend, child, mode, status);

      *status = phodav_status_from_error (error);
      return NULL;
    }

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      *status = SOUP_STATUS_FORBIDDEN;
      return NULL;
    }

  return phodav_path_new_from_gfile (backend, child, mode, status);
}

GFileInfo *
phodav_backend_gfile_query_info (PhodavPath *path,
                                 const gchar *attributes,
                                 GFileQueryInfoFlags flags,
                                 GCancellable *cancellable,
                                 GError **error)
{
  GFile *file = phodav_path_query_file (path);

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
phodav_backend_gfile_open (PhodavPath *path,
                           GCancellable *cancellable,
                           GError **error)
{
  GFile *file = path->read_file ?: path->named_file ?
                                                    : path->file;

  if (!file)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "The path has no filesystem object");
      return NULL;
    }

  return g_file_read (file, cancellable, error);
}

GFileEnumerator *
phodav_backend_gfile_enumerate (PhodavPath *path,
                                const gchar *attributes,
                                GFileQueryInfoFlags flags,
                                GCancellable *cancellable,
                                GError **error)
{
  GFile *file = path->read_file ?: path->named_file ?
                                                    : path->file;

  if (!file)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "The path has no filesystem object");
      return NULL;
    }

  return g_file_enumerate_children (file, attributes, flags, cancellable, error);
}

gboolean
phodav_backend_gfile_mkdir (PhodavPath *path,
                            GCancellable *cancellable,
                            GError **error)
{
  return g_file_make_directory (path->named_file ?: path->file,
                                cancellable, error);
}

gboolean
phodav_backend_gfile_unlink (PhodavPath *path,
                             GCancellable *cancellable,
                             GError **error)
{
  return g_file_delete (path->read_file ?: path->named_file ?
                                                            : path->file,
                        cancellable, error);
}

gboolean
phodav_backend_gfile_unlink_child (PhodavPath *parent,
                                   const gchar *name,
                                   GCancellable *cancellable,
                                   GError **error)
{
  GFile *base = parent->read_file ?: parent->named_file ?
                                                        : parent->file;
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

gboolean
phodav_backend_gfile_copy_file (PhodavPath *from,
                                PhodavPath *to,
                                GFileCopyFlags flags,
                                GCancellable *cancellable,
                                GError **error)
{
  GFile *source = from->read_file ?: from->named_file ?
                                                      : from->file;
  GFile *dest = to->named_file ?: to->read_file ?
                                                : to->file;

  if (!source || !dest)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "The path has no filesystem object");
      return FALSE;
    }

  return g_file_copy (source, dest, flags, cancellable, NULL, NULL, error);
}

gboolean
phodav_backend_gfile_move (PhodavPath *from,
                           PhodavPath *to,
                           GFileCopyFlags flags,
                           GCancellable *cancellable,
                           GError **error)
{
  GFile *source = from->named_file ?: from->read_file;
  GFile *dest = to->named_file ?: to->read_file;

  if (!source || !dest)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "The path has no filesystem object");
      return FALSE;
    }

  return g_file_move (source, dest, flags, cancellable, NULL, NULL, error);
}

static const PhodavBackendClass gfile_backend_class = {
  .name = "gfile",
  .resolve = phodav_backend_gfile_resolve,
  .resolve_child = phodav_backend_gfile_resolve_child,
  .query_info = phodav_backend_gfile_query_info,
  .open = phodav_backend_gfile_open,
  .enumerate = phodav_backend_gfile_enumerate,
  .mkdir = phodav_backend_mkdir_at,
  .unlink = phodav_backend_unlink_at,
  .unlink_child = phodav_backend_unlink_child_at,
  .copy_file = phodav_backend_gfile_copy_file,
  .move = phodav_backend_move_at,
};

PhodavBackend *
phodav_backend_gfile_new (GFile *root)
{
  PhodavBackend *backend = g_new0 (PhodavBackend, 1);

  backend->klass = &gfile_backend_class;
  backend->ref_count = 1;
  backend->root = g_object_ref (root);

  return backend;
}
