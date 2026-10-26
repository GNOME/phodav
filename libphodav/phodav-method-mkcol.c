/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-priv.h"

typedef struct _MkcolJob
{
  PhodavRequest *req;
  PathHandler *handler;
  gchar *path;
  GCancellable *cancellable;
  GList *submitted;
  gint status;
  GError *error;
} MkcolJob;

static void
mkcol_job_free (MkcolJob *job)
{
  g_clear_pointer (&job->req, phodav_request_unref);
  g_clear_pointer (&job->handler, path_handler_unref);
  g_free (job->path);
  g_clear_object (&job->cancellable);
  g_list_free_full (job->submitted, (GDestroyNotify) lock_submitted_free);
  g_clear_error (&job->error);
  g_free (job);
}

static gint
mkcol_work (PhodavRequest *req, GError **err)
{
  MkcolJob *job = phodav_request_get_method_data (req);
  g_autoptr(PhodavPath) handle = NULL;
  g_autoptr(GError) error = NULL;

  (void) err;

  job->status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
  handle = phodav_path_resolve (job->handler, job->path,
                                PHODAV_PATH_CREATE, &job->status);
  if (!handle)
    return SOUP_STATUS_OK;

  if (!phodav_path_mkdir (handle, job->cancellable, &error))
    {
      job->error = g_steal_pointer (&error);
      job->status = phodav_status_from_error (job->error);
    }
  else
    job->status = SOUP_STATUS_CREATED;

  return SOUP_STATUS_OK;
}

static gint
mkcol_commit (PhodavRequest *req, GError **err)
{
  MkcolJob *job = phodav_request_get_method_data (req);

  if (job->error && err)
    *err = g_steal_pointer (&job->error);

  return job->status;
}

static gint
mkcol_validate (PhodavRequest *req, GError **err)
{
  MkcolJob *job;
  PhodavServer *server = handler_get_server (phodav_request_get_handler (req));
  SoupMessageBody *request_body = phodav_request_get_request_body (req);
  gint status;

  (void) err;

  if (request_body && request_body->length)
    return SOUP_STATUS_UNSUPPORTED_MEDIA_TYPE;

  job = g_new0 (MkcolJob, 1);
  job->req = phodav_request_ref (req);
  job->handler = path_handler_ref (phodav_request_get_handler (req));
  job->path = g_strdup (phodav_request_get_path (req));
  job->cancellable = g_object_ref (phodav_request_get_cancellable (req));
  phodav_request_set_method_data (req, job, (GDestroyNotify) mkcol_job_free);

  status = phodav_check_if (req, &job->submitted);
  if (status != SOUP_STATUS_OK)
    return status;

  if (phodav_lock_store_path_has_other_locks (server, job->path,
                                              job->submitted))
    return SOUP_STATUS_LOCKED;

  job->status = SOUP_STATUS_CREATED;
  return SOUP_STATUS_OK;
}

const PhodavMethodOps phodav_method_mkcol_ops = {
  .validate = mkcol_validate,
  .work = mkcol_work,
  .commit = mkcol_commit,
};
