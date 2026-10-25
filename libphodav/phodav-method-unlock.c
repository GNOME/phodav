/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-priv.h"

typedef struct _Unlock
{
  PhodavRequest *req;
  PhodavServer *server;
  gchar *path;
  gchar *token;
} Unlock;

static void
unlock_free (Unlock *unlock)
{
  g_clear_pointer (&unlock->req, phodav_request_unref);
  g_clear_object (&unlock->server);
  g_free (unlock->path);
  g_free (unlock->token);
  g_free (unlock);
}

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

static void
unlock_start (gpointer data)
{
  Unlock *unlock = data;
  gint status;

  if (phodav_request_is_finished (unlock->req))
    {
      phodav_server_namespace_release (unlock->server);
      phodav_request_complete (unlock->req, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
      unlock_free (unlock);
      return;
    }

  status = phodav_lock_store_remove_lock (unlock->server, unlock->path,
                                          unlock->token);

  phodav_server_namespace_release (unlock->server);
  phodav_request_complete (unlock->req, status, NULL);
  unlock_free (unlock);
}

gint
phodav_method_unlock (PhodavRequest *req, GError **err)
{
  Unlock *unlock = g_new0 (Unlock, 1);

  (void) err;

  unlock->token = remove_brackets (
    soup_message_headers_get_one (phodav_request_get_request_headers (req), "Lock-Token"));
  if (!unlock->token)
    {
      g_free (unlock);
      return SOUP_STATUS_BAD_REQUEST;
    }

  unlock->req = phodav_request_ref (req);
  unlock->server = g_object_ref (handler_get_server (phodav_request_get_handler (req)));
  unlock->path = g_strdup (phodav_request_get_path (req));

  phodav_server_namespace_acquire (unlock->server, unlock_start, unlock);

  return 0;
}
