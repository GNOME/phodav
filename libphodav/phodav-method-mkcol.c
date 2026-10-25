/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-priv.h"

static gint
do_mkcol_file (GFile *file, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  gint status = SOUP_STATUS_CREATED;

  if (!g_file_make_directory (file, cancellable, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        status = SOUP_STATUS_CONFLICT;
      else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        status = SOUP_STATUS_METHOD_NOT_ALLOWED;
      else
        {
          status = SOUP_STATUS_FORBIDDEN;
          g_propagate_error (err, g_steal_pointer (&error));
        }
    }

  return status;
}

gint
phodav_method_mkcol (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  const gchar *path = phodav_request_get_path (req);
  GFile *file = NULL;
  GCancellable *cancellable = phodav_request_get_cancellable (req);
  gint status;
  g_autolist(LockSubmitted) submitted = NULL;
  PhodavLockGuard *guard = NULL;
  SoupMessageBody *request_body = phodav_request_get_request_body (req);

  if (request_body && request_body->length)
    {
      status = SOUP_STATUS_UNSUPPORTED_MEDIA_TYPE;
      goto end;
    }

  status = phodav_check_if (req, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  {
    GList *paths = g_list_prepend (NULL, (gpointer) path);

    guard = phodav_lock_store_guard_acquire (handler_get_server (handler), paths);
    g_list_free (paths);
  }

  if (phodav_lock_store_path_has_other_locks (handler_get_server (handler),
                                              path, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  file = handler_get_file_for_path (handler, path);
  status = do_mkcol_file (file, cancellable, err);

end:
  phodav_lock_store_guard_release (guard);
  g_clear_object (&file);
  return status;
}
