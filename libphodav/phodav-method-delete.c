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

typedef struct _DeleteJob
{
  PhodavRequest *req;
  PathHandler *handler;
  gchar *path;
  GCancellable *cancellable;
  GList *submitted;
  GHashTable *mstatus;
  GPtrArray *removed;
  gint status;
} DeleteJob;

static void
delete_notify (const gchar *path, gint status, gpointer data)
{
  GHashTable *mstatus = data;

  if (mstatus)
    g_hash_table_insert (mstatus, g_strdup (path),
                         response_new (NULL, status));
}

static void
delete_job_free (DeleteJob *job)
{
  g_clear_pointer (&job->req, phodav_request_unref);
  g_clear_pointer (&job->handler, path_handler_unref);
  g_free (job->path);
  g_clear_object (&job->cancellable);
  g_list_free_full (job->submitted, (GDestroyNotify) lock_submitted_free);
  g_clear_pointer (&job->mstatus, g_hash_table_unref);
  g_clear_pointer (&job->removed, g_ptr_array_unref);
  g_free (job);
}

static void
delete_run (gpointer data)
{
  DeleteJob *job = data;
  g_autoptr(PhodavPath) handle = NULL;

  handle = phodav_path_resolve (job->handler, job->path,
                                PHODAV_PATH_EXISTING, &job->status);
  if (!handle)
    return;

  job->status = phodav_path_delete_tree (handle, job->path,
                                         delete_notify, job->mstatus,
                                         job->removed, job->cancellable);
}

static void
delete_done (gpointer data)
{
  DeleteJob *job = data;
  PhodavServer *server = handler_get_server (job->handler);
  gint status = job->status;

  if (job->removed)
    {
      guint i;

      for (i = 0; i < job->removed->len; i++)
        phodav_lock_store_remove_locks (server,
                                        g_ptr_array_index (job->removed, i));
    }

  if (status == SOUP_STATUS_NO_CONTENT && job->mstatus &&
      g_hash_table_size (job->mstatus) > 0)
    status = set_response_multistatus (job->req, job->mstatus, NULL);

  phodav_server_namespace_release (server);
  phodav_request_complete (job->req, status, NULL);
  delete_job_free (job);
}

static void
delete_start (gpointer data)
{
  DeleteJob *job = data;
  PhodavServer *server = handler_get_server (job->handler);

  if (phodav_request_is_finished (job->req))
    {
      phodav_server_namespace_release (server);
      phodav_request_complete (job->req, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
      delete_job_free (job);
      return;
    }

  job->status = phodav_check_if (job->req, &job->submitted);
  if (job->status != SOUP_STATUS_OK)
    goto done;

  /* deleting a collection also deletes its members: the tokens of
   * the locks held on them must be submitted too */
  if (phodav_lock_store_tree_has_other_locks (server, job->path,
                                              job->submitted))
    {
      job->status = SOUP_STATUS_LOCKED;
      goto done;
    }

  job->mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify) response_free);
  job->removed = g_ptr_array_new_with_free_func (g_free);

  phodav_server_dispatch (server, delete_run, delete_done, job);
  return;

done:
  phodav_server_namespace_release (server);
  phodav_request_complete (job->req, job->status, NULL);
  delete_job_free (job);
}

gint
phodav_method_delete (PhodavRequest *req, GError **err)
{
  DeleteJob *job;
  DepthType depth;

  /* RFC 4918 section 9.6.1: DELETE acts with depth infinity, and a
   * client must not submit any other depth */
  if (!depth_from_string (soup_message_headers_get_one (phodav_request_get_request_headers (req), "Depth"), &depth) ||
      depth != DEPTH_INFINITY)
    return SOUP_STATUS_BAD_REQUEST;

  (void) err;

  job = g_new0 (DeleteJob, 1);
  job->req = phodav_request_ref (req);
  job->handler = path_handler_ref (phodav_request_get_handler (req));
  job->path = g_strdup (phodav_request_get_path (req));
  job->cancellable = g_object_ref (phodav_request_get_cancellable (req));

  phodav_server_namespace_acquire (handler_get_server (job->handler),
                                   delete_start, job);

  return 0;
}
