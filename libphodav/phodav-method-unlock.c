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
#include "phodav-lock.h"

static gchar *
remove_brackets (const gchar *str)
{
  if (!str)
    return NULL;

  gint len = strlen (str);

  if (str[0] != '<' || str[len - 1] != '>')
    return NULL;

  return g_strndup (str + 1, len - 2);
}

gint
phodav_method_unlock (PathHandler *handler, SoupServerMessage *msg,
                      const char *path, GError **err)
{
  DAVLock *lock;
  gint status = SOUP_STATUS_BAD_REQUEST;

  gchar *token = remove_brackets (
                                  soup_message_headers_get_one (soup_server_message_get_request_headers (msg), "Lock-Token"));

  g_return_val_if_fail (token != NULL, SOUP_STATUS_BAD_REQUEST);

  lock = server_path_get_lock (handler_get_server (handler), path, token);
  if (!lock)
    {
      status = SOUP_STATUS_CONFLICT;
      goto end;
    }

  dav_lock_free (lock);
  status = SOUP_STATUS_NO_CONTENT;

end:
  g_free (token);
  return status;
}
