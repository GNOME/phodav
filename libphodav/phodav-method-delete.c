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

gint
phodav_delete_file (PhodavServer *server, const gchar *path, GFile *file, GHashTable *mstatus, GCancellable *cancellable)
{
  g_autoptr(GFileEnumerator) e = NULL;
  g_autoptr(GError) error = NULL;
  gint status = SOUP_STATUS_NO_CONTENT;

  e = g_file_enumerate_children (file, "standard::*",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (e)
    {
      while (1)
        {
          g_autoptr(GFileInfo) info = g_file_enumerator_next_file (e, cancellable, &error);
          if (!info)
            break;
          g_autoptr(GFile) del = g_file_get_child (file, g_file_info_get_name (info));
          /* the multistatus keys are the decoded resource paths, the
           * hrefs are escaped when the response is built */
          g_autofree gchar *del_path = g_build_path ("/", path, g_file_info_get_name (info), NULL);

          if ((g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK))
            {
              g_autoptr(GError) link_error = NULL;

              if (!g_file_delete (del, cancellable, &link_error) && mstatus)
                g_hash_table_insert (mstatus, g_strdup (del_path),
                                     response_new (NULL,
                                                   phodav_status_from_error (link_error)));
              continue;
            }

          phodav_delete_file (server, del_path, del, mstatus, cancellable);
        }

      g_file_enumerator_close (e, cancellable, NULL);
    }

  if (error)
    g_debug ("DELETE: enumeration error: %s", error->message);

  g_autoptr(GError) del_error = NULL;
  if (!g_file_delete (file, cancellable, &del_error))
    {
      if (mstatus)
        {
          status = phodav_status_from_error (del_error);

          g_hash_table_insert (mstatus, g_strdup (path),
                               response_new (NULL, status));
        }
    }
  else
    phodav_lock_store_remove_locks (server, path);

  if (del_error)
    g_debug ("ignored del error: %s", del_error->message);

  return status;
}

gint
phodav_method_delete (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  const gchar *path = phodav_request_get_path (req);
  GCancellable *cancellable = phodav_request_get_cancellable (req);
  GFile *file = NULL;
  g_autoptr(GHashTable) mstatus = NULL;
  gint status;
  g_autolist(LockSubmitted) submitted = NULL;
  DepthType depth;

  phodav_server_write_lock (handler_get_server (handler));

  /* RFC 4918 section 9.6.1: DELETE acts with depth infinity, and a
   * client must not submit any other depth */
  if (!depth_from_string (soup_message_headers_get_one (phodav_request_get_request_headers (req), "Depth"), &depth) ||
      depth != DEPTH_INFINITY)
    {
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }

  status = phodav_check_if (req, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  /* deleting a collection also deletes its members: the tokens of
   * the locks held on them must be submitted too */
  if (phodav_lock_store_tree_has_other_locks (handler_get_server (handler),
                                              path, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  file = phodav_path_resolve (handler, path, PHODAV_PATH_EXISTING, &status);
  if (!file)
    goto end;
  mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) response_free);

  status = phodav_delete_file (handler_get_server (handler), path, file, mstatus, cancellable);
  if (status == SOUP_STATUS_NO_CONTENT)
    if (g_hash_table_size (mstatus) > 0)
      status = set_response_multistatus (req, mstatus, NULL);

end:
  phodav_server_write_unlock (handler_get_server (handler));
  g_clear_object (&file);

  return status;
}
