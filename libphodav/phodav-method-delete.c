/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-multistatus.h"
#include "phodav-priv.h"
#include "phodav-utils.h"

static void
delete_notify (const gchar *path, gint status, gpointer data)
{
  GHashTable *mstatus = data;

  if (mstatus)
    g_hash_table_insert (mstatus, g_strdup (path),
                         response_new (NULL, status));
}

gint
phodav_method_delete (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  PhodavServer *server = handler_get_server (handler);
  const gchar *path = phodav_request_get_path (req);
  GCancellable *cancellable = phodav_request_get_cancellable (req);
  g_autoptr(PhodavPath) handle = NULL;
  g_autoptr(GHashTable) mstatus = NULL;
  gint status;
  g_autolist(LockSubmitted) submitted = NULL;
  DepthType depth;

  /* RFC 4918 section 9.6.1: DELETE acts with depth infinity, and a
   * client must not submit any other depth */
  if (!depth_from_string (soup_message_headers_get_one (phodav_request_get_request_headers (req), "Depth"), &depth) ||
      depth != DEPTH_INFINITY)
    return SOUP_STATUS_BAD_REQUEST;

  /* decide under the write lock whether the request may proceed: the
   * lock check and the resolution of the resource must not race with
   * a mutation. The recursive deletion itself runs outside the lock,
   * so it does not stall the other requests. */
  phodav_server_write_lock (server);

  status = phodav_check_if (req, &submitted);
  if (status != SOUP_STATUS_OK)
    goto unlock;

  /* deleting a collection also deletes its members: the tokens of
   * the locks held on them must be submitted too */
  if (phodav_lock_store_tree_has_other_locks (server, path, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto unlock;
    }

  handle = phodav_path_resolve (handler, path, PHODAV_PATH_EXISTING, &status);

unlock:
  phodav_server_write_unlock (server);

  if (status != SOUP_STATUS_OK || !handle)
    return status;

  mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) response_free);

  status = phodav_path_delete_tree (server, handle, path,
                                    delete_notify, mstatus, cancellable);
  if (status == SOUP_STATUS_NO_CONTENT)
    if (g_hash_table_size (mstatus) > 0)
      status = set_response_multistatus (req, mstatus, NULL);

  return status;
}
