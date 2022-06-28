/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "phodav-priv.h"

static gint
do_mkcol_file (SoupServerMessage *msg, GFile *file,
               GCancellable *cancellable, GError **err)
{
  GError *error = NULL;
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
          g_propagate_error (err, error);
          error = NULL;
        }

      g_clear_error (&error);
    }

  return status;
}

gint
phodav_method_mkcol (PathHandler *handler, SoupServerMessage *msg,
                     const char *path, GError **err)
{
  GFile *file = NULL;
  GCancellable *cancellable = handler_get_cancellable (handler);
  gint status;
  GList *submitted = NULL;
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
