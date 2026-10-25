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
phodav_method_unlock (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  PhodavServer *server = handler_get_server (handler);
  const gchar *path = phodav_request_get_path (req);
  gint status = SOUP_STATUS_BAD_REQUEST;
  GList *paths;
  PhodavLockGuard *guard;

  gchar *token = remove_brackets (
    soup_message_headers_get_one (phodav_request_get_request_headers (req), "Lock-Token"));

  if (!token)
    goto end;

  paths = g_list_prepend (NULL, (gpointer) path);
  guard = phodav_lock_store_guard_acquire (server, paths);
  g_list_free (paths);

  status = phodav_lock_store_remove_lock (server, path, token);
  phodav_lock_store_guard_release (guard);

end:
  g_free (token);
  return status;
}
