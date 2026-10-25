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

static void
mkcol_run (gpointer data)
{
  MkcolJob *job = data;
  g_autoptr(PhodavPath) handle = NULL;

  handle = phodav_path_resolve (job->handler, job->path,
                                PHODAV_PATH_CREATE, &job->status);
  if (!handle)
    return;

  if (!phodav_path_mkdir (handle, job->cancellable, &job->error))
    job->status = phodav_status_from_error (job->error);
  else
    job->status = SOUP_STATUS_CREATED;
}

static void
mkcol_done (gpointer data)
{
  MkcolJob *job = data;

  phodav_server_namespace_release (handler_get_server (job->handler));
  phodav_request_complete (job->req, job->status,
                           g_steal_pointer (&job->error));
  mkcol_job_free (job);
}

static void
mkcol_start (gpointer data)
{
  MkcolJob *job = data;
  PhodavServer *server = handler_get_server (job->handler);
  SoupMessageBody *request_body =
    phodav_request_get_request_body (job->req);

  if (phodav_request_is_finished (job->req))
    {
      phodav_server_namespace_release (server);
      phodav_request_complete (job->req, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
      mkcol_job_free (job);
      return;
    }

  if (request_body && request_body->length)
    {
      phodav_server_namespace_release (server);
      phodav_request_complete (job->req, SOUP_STATUS_UNSUPPORTED_MEDIA_TYPE,
                               NULL);
      mkcol_job_free (job);
      return;
    }

  job->status = phodav_check_if (job->req, &job->submitted);
  if (job->status != SOUP_STATUS_OK)
    {
      phodav_server_namespace_release (server);
      phodav_request_complete (job->req, job->status, NULL);
      mkcol_job_free (job);
      return;
    }

  if (phodav_lock_store_path_has_other_locks (server, job->path,
                                              job->submitted))
    {
      phodav_server_namespace_release (server);
      phodav_request_complete (job->req, SOUP_STATUS_LOCKED, NULL);
      mkcol_job_free (job);
      return;
    }

  phodav_server_dispatch (server, mkcol_run, mkcol_done, job);
}

gint
phodav_method_mkcol (PhodavRequest *req, GError **err)
{
  MkcolJob *job = g_new0 (MkcolJob, 1);

  (void) err;

  job->req = phodav_request_ref (req);
  job->handler = path_handler_ref (phodav_request_get_handler (req));
  job->path = g_strdup (phodav_request_get_path (req));
  job->cancellable = g_object_ref (phodav_request_get_cancellable (req));

  phodav_server_namespace_acquire (handler_get_server (job->handler),
                                   mkcol_start, job);

  return 0;
}
