/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-priv.h"

static gint
do_mkcol_file (SoupServerMessage *msg, GFile *file, GCancellable *cancellable, GError **err)
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
phodav_method_mkcol (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  GFile *file = NULL;
  GCancellable *cancellable = handler_get_cancellable (handler);
  gint status;
  g_autolist(LockSubmitted) submitted = NULL;
  SoupMessageBody *request_body = soup_server_message_get_request_body (msg);

  if (request_body && request_body->length)
    {
      status = SOUP_STATUS_UNSUPPORTED_MEDIA_TYPE;
      goto end;
    }

  status = phodav_check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  status = do_mkcol_file (msg, file, cancellable, err);

end:
  g_clear_object (&file);
  return status;
}
