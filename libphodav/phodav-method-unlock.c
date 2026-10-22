/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-priv.h"

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
phodav_method_unlock (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  DAVLock *lock;
  gint status = SOUP_STATUS_BAD_REQUEST;

  gchar *token = remove_brackets (
    soup_message_headers_get_one (soup_server_message_get_request_headers (msg), "Lock-Token"));

  if (!token)
    goto end;

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
