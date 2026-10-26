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

static gint
delete_work (PhodavRequest *req, GError **err)
{
  DeleteJob *job = phodav_request_get_method_data (req);
  g_autoptr(PhodavPath) handle = NULL;

  (void) err;

  handle = phodav_path_resolve (job->handler, job->path,
                                PHODAV_PATH_EXISTING, &job->status);
  if (!handle)
    return job->status;

  job->status = phodav_path_delete_tree (handle, job->path,
                                         delete_notify, job->mstatus,
                                         job->removed, job->cancellable);
  return SOUP_STATUS_OK;
}

static gint
delete_commit (PhodavRequest *req, GError **err)
{
  DeleteJob *job = phodav_request_get_method_data (req);
  PhodavServer *server = handler_get_server (job->handler);
  gint status = job->status;

  (void) err;

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

  return status;
}

static gint
delete_validate (PhodavRequest *req, GError **err)
{
  DeleteJob *job;
  PhodavServer *server = handler_get_server (phodav_request_get_handler (req));
  DepthType depth;
  gint status;

  (void) err;

  /* RFC 4918 section 9.6.1: DELETE acts with depth infinity, and a
   * client must not submit any other depth */
  if (!depth_from_string (soup_message_headers_get_one (phodav_request_get_request_headers (req), "Depth"), &depth) ||
      depth != DEPTH_INFINITY)
    return SOUP_STATUS_BAD_REQUEST;

  job = g_new0 (DeleteJob, 1);
  job->req = phodav_request_ref (req);
  job->handler = path_handler_ref (phodav_request_get_handler (req));
  job->path = g_strdup (phodav_request_get_path (req));
  job->cancellable = g_object_ref (phodav_request_get_cancellable (req));
  phodav_request_set_method_data (req, job, (GDestroyNotify) delete_job_free);

  status = phodav_check_if (req, &job->submitted);
  if (status != SOUP_STATUS_OK)
    return status;

  /* deleting a collection also deletes its members: the tokens of
   * the locks held on them must be submitted too */
  if (phodav_lock_store_tree_has_other_locks (server, job->path,
                                              job->submitted))
    return SOUP_STATUS_LOCKED;

  job->mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify) response_free);
  job->removed = g_ptr_array_new_with_free_func (g_free);
  job->status = SOUP_STATUS_NO_CONTENT;

  return SOUP_STATUS_OK;
}

const PhodavMethodOps phodav_method_delete_ops = {
  .validate = delete_validate,
  .work = delete_work,
  .commit = delete_commit,
};
