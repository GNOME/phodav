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
  gint status;
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

static gint
unlock_commit (PhodavRequest *req, GError **err)
{
  Unlock *unlock = phodav_request_get_method_data (req);

  (void) err;

  return unlock->status;
}

static gint
unlock_validate (PhodavRequest *req, GError **err)
{
  g_autofree gchar *token = remove_brackets (
    soup_message_headers_get_one (phodav_request_get_request_headers (req), "Lock-Token"));
  Unlock *unlock;

  (void) err;

  if (!token)
    return SOUP_STATUS_BAD_REQUEST;

  unlock = g_new0 (Unlock, 1);
  unlock->token = g_steal_pointer (&token);
  unlock->req = phodav_request_ref (req);
  unlock->server = g_object_ref (handler_get_server (phodav_request_get_handler (req)));
  unlock->path = g_strdup (phodav_request_get_path (req));
  unlock->status = phodav_lock_store_remove_lock (unlock->server, unlock->path,
                                                  unlock->token);
  phodav_request_set_method_data (req, unlock, (GDestroyNotify) unlock_free);

  return SOUP_STATUS_OK;
}

const PhodavMethodOps phodav_method_unlock_ops = {
  .validate = unlock_validate,
  .commit = unlock_commit,
};
