/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-backend.h"

#include "phodav-virtual-dir.h"

static gboolean
is_virtual (PhodavPath *path)
{
  return PHODAV_IS_VIRTUAL_DIR (phodav_path_query_file (path));
}

static gboolean
virtual_mkdir (PhodavPath *path, GCancellable *cancellable, GError **error)
{
  if (is_virtual (path))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "A virtual directory cannot be created");
      return FALSE;
    }

  return phodav_backend_mkdir_at (path, cancellable, error);
}

static gboolean
virtual_unlink (PhodavPath *path, GCancellable *cancellable, GError **error)
{
  if (is_virtual (path))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "A virtual directory cannot be removed");
      return FALSE;
    }

  return phodav_backend_unlink_at (path, cancellable, error);
}

static gboolean
virtual_unlink_child (PhodavPath *parent, const gchar *name, GCancellable *cancellable, GError **error)
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
  if (PHODAV_IS_VIRTUAL_DIR (child))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "A virtual directory cannot be removed");
      return FALSE;
    }

  return phodav_backend_unlink_child_at (parent, name, cancellable, error);
}

static gboolean
virtual_copy_file (PhodavPath *from,
                   PhodavPath *to,
                   GFileCopyFlags flags,
                   GCancellable *cancellable,
                   GError **error)
{
  if (is_virtual (from) || is_virtual (to))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "A virtual directory cannot be copied");
      return FALSE;
    }

  return phodav_backend_gfile_copy_file (from, to, flags,
                                         cancellable, error);
}

static gboolean
virtual_move (PhodavPath *from,
              PhodavPath *to,
              GFileCopyFlags flags,
              GCancellable *cancellable,
              GError **error)
{
  if (is_virtual (from) || is_virtual (to))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "A virtual directory cannot be moved");
      return FALSE;
    }

  return phodav_backend_move_at (from, to, flags, cancellable, error);
}

static const PhodavBackendClass virtual_backend_class = {
  .name = "virtual",
  .resolve = phodav_backend_gfile_resolve,
  .resolve_child = phodav_backend_gfile_resolve_child,
  .query_info = phodav_backend_gfile_query_info,
  .open = phodav_backend_gfile_open,
  .enumerate = phodav_backend_gfile_enumerate,
  .mkdir = virtual_mkdir,
  .unlink = virtual_unlink,
  .unlink_child = virtual_unlink_child,
  .copy_file = virtual_copy_file,
  .move = virtual_move,
};

PhodavBackend *
phodav_backend_virtual_new (GFile *root)
{
  PhodavBackend *backend = g_new0 (PhodavBackend, 1);

  backend->klass = &virtual_backend_class;
  backend->ref_count = 1;
  backend->root = g_object_ref (root);

  return backend;
}
