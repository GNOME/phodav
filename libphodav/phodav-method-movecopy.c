/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-priv.h"
#include "phodav-utils.h"
#include "phodav-virtual-dir.h"

typedef struct _MoveCopyJob
{
  PhodavRequest *req;
  PathHandler *handler;
  gchar *path;
  gchar *dest;
  DepthType depth;
  gboolean copy;
  GCancellable *cancellable;
  GList *submitted;
  GPtrArray *removed;
  gint status;
  GError *error;
} MoveCopyJob;

static gboolean
dest_exists (PhodavPath *dest, GCancellable *cancellable)
{
  g_autoptr(GFileInfo) info = NULL;

  info = phodav_path_query_info (dest, "standard::type",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);

  return info != NULL;
}

static gboolean
path_is_directory (PhodavPath *path, GCancellable *cancellable)
{
  g_autoptr(GFileInfo) info = NULL;

  info = phodav_path_query_info (path, "standard::type",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);

  return info &&
         g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
}

static gint
delete_destination (PhodavPath *dest, const gchar *dest_path, GPtrArray *removed, GCancellable *cancellable)
{
  return phodav_path_delete_tree (dest, dest_path, NULL, NULL,
                                  removed, cancellable);
}

static gint
do_movecopy_file (MoveCopyJob *job)
{
  g_autoptr(PhodavPath) src = NULL;
  g_autoptr(PhodavPath) dest = NULL;
  g_autoptr(GError) error = NULL;
  SoupMessageHeaders *request_headers =
    phodav_request_get_request_headers (job->req);
  GFileCopyFlags flags = G_FILE_COPY_ALL_METADATA;
  gboolean overwrite;
  gboolean exists;
  gboolean ok;

  src = phodav_path_resolve (job->handler, job->path,
                             PHODAV_PATH_EXISTING, &job->status);
  if (!src)
    {
      /* a missing MOVE/COPY source is reported as a conflict */
      if (job->status == SOUP_STATUS_NOT_FOUND)
        job->status = SOUP_STATUS_CONFLICT;
      return job->status;
    }

  dest = phodav_path_resolve (job->handler, job->dest,
                              PHODAV_PATH_CREATE, &job->status);
  if (!dest)
    return job->status;

  /* take the short path as g_file_copy seems to be rather complicated */
  if (PHODAV_IS_VIRTUAL_DIR (phodav_path_get_file (src)) ||
      PHODAV_IS_VIRTUAL_DIR (phodav_path_get_named_file (dest)))
    return SOUP_STATUS_FORBIDDEN;

  overwrite = g_strcmp0 (
                soup_message_headers_get_one (request_headers, "Overwrite"), "F") != 0;
  if (overwrite)
    flags |= G_FILE_COPY_OVERWRITE;
  exists = dest_exists (dest, job->cancellable);

  /* replacing a collection removes it together with its members: delete
   * it first (RFC 4918 section 9.6.1). This also avoids relying on the
   * copy error to detect a directory destination: the win32 backend
   * reports G_IO_ERROR_PERMISSION_DENIED where the POSIX one reports
   * G_IO_ERROR_IS_DIRECTORY or G_IO_ERROR_WOULD_MERGE. */
  if (overwrite && exists &&
      (path_is_directory (src, job->cancellable) ||
       path_is_directory (dest, job->cancellable)))
    delete_destination (dest, job->dest, job->removed, job->cancellable);

  /* a COPY is recursive when its depth is infinity; a MOVE commits
   * with a rename, which the path layer protects. */
  if (job->copy)
    ok = phodav_path_copy (src, dest, flags, job->depth == DEPTH_INFINITY,
                           job->cancellable, &error);
  else
    ok = phodav_path_move (src, dest, flags, job->cancellable, &error);

  /* an existing destination of another kind cannot be replaced in
   * place: remove it, then try again with the destination gone */
  if (!ok && overwrite &&
      (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY) ||
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE) ||
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY)) &&
      delete_destination (dest, job->dest, job->removed,
                          job->cancellable) == SOUP_STATUS_NO_CONTENT)
    {
      g_clear_error (&error);
      if (job->copy)
        ok = phodav_path_copy (src, dest, flags,
                               job->depth == DEPTH_INFINITY,
                               job->cancellable, &error);
      else
        ok = phodav_path_move (src, dest, flags, job->cancellable, &error);
    }

  if (ok)
    return exists ? SOUP_STATUS_NO_CONTENT : SOUP_STATUS_CREATED;

  if (!overwrite && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    return SOUP_STATUS_PRECONDITION_FAILED;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_debug ("MOVE/COPY source or destination is missing: %s",
               error->message);
      return SOUP_STATUS_CONFLICT;
    }

  g_debug ("MOVE/COPY failed: %s", error->message);
  job->error = g_steal_pointer (&error);
  return SOUP_STATUS_INTERNAL_SERVER_ERROR;
}

static void
movecopy_job_free (MoveCopyJob *job)
{
  g_clear_pointer (&job->req, phodav_request_unref);
  g_clear_pointer (&job->handler, path_handler_unref);
  g_free (job->path);
  g_free (job->dest);
  g_clear_object (&job->cancellable);
  g_list_free_full (job->submitted, (GDestroyNotify) lock_submitted_free);
  g_clear_pointer (&job->removed, g_ptr_array_unref);
  g_clear_error (&job->error);
  g_free (job);
}

static gint
movecopy_work (PhodavRequest *req, GError **err)
{
  MoveCopyJob *job = phodav_request_get_method_data (req);

  (void) err;

  job->status = do_movecopy_file (job);
  return SOUP_STATUS_OK;
}

static gint
movecopy_commit (PhodavRequest *req, GError **err)
{
  MoveCopyJob *job = phodav_request_get_method_data (req);
  PhodavServer *server = handler_get_server (job->handler);

  if (job->removed)
    {
      guint i;

      for (i = 0; i < job->removed->len; i++)
        phodav_lock_store_remove_locks (server,
                                        g_ptr_array_index (job->removed, i));
    }

  /* the source is gone: do not leave its locks behind on a path which
   * no longer names a resource */
  if (!job->copy &&
      (job->status == SOUP_STATUS_CREATED ||
       job->status == SOUP_STATUS_NO_CONTENT))
    phodav_lock_store_remove_locks (server, job->path);

  if (job->error && err)
    *err = g_steal_pointer (&job->error);

  return job->status;
}

static gint
movecopy_validate (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  const gchar *path = phodav_request_get_path (req);
  GUri *dest_uri = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;
  const gchar *dest;
  gboolean copy = g_strcmp0 (phodav_request_get_method (req), "COPY") == 0;
  DepthType depth = DEPTH_INFINITY;
  g_autofree gchar *udest = NULL;
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  MoveCopyJob *job;
  PhodavServer *server = handler_get_server (handler);

  (void) err;

  /* RFC 4918 section 9.8.3 and 10.2: only the depths 0 and infinity
   * are valid for COPY and MOVE, an invalid depth must be rejected
   * with 400 */
  if (!depth_from_string (soup_message_headers_get_one (request_headers, "Depth"), &depth) ||
      depth == DEPTH_ONE)
    {
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }

  dest = soup_message_headers_get_one (request_headers, "Destination");
  if (!dest)
    goto end;
  dest_uri = g_uri_parse (dest, SOUP_HTTP_URI_FLAGS, NULL);
  if (!dest_uri)
    {
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }
  dest = g_uri_get_path (dest_uri);
  if (!dest || !*dest)
    goto end;

  /* like the request path, reject encoded slashes: they would survive
   * dot-segment removal and allow escaping the root once decoded;
   * and an empty first segment ("//"), which g_file_get_child()
   * would resolve against the filesystem root */
  udest = g_uri_unescape_string (dest, "/");
  if (!phodav_path_is_safe (udest) || !handler_path_is_covered (handler, udest))
    {
      g_debug ("invalid destination path: %s", dest);
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }

  {
    g_autofree gchar *canonical = phodav_path_canonicalize (path);
    g_autofree gchar *dest_canonical = phodav_path_canonicalize (udest);

    /* a COPY or MOVE into itself would recurse endlessly, and
     * replacing one of the source ancestors would delete the source
     * before it is copied: the destination and the source must not
     * cover each other (RFC 4918 sections 9.8.5 and 9.9.4) */
    if (path_is_covered (dest_canonical, canonical) ||
        path_is_covered (canonical, dest_canonical))
      {
        g_debug ("the destination %s overlaps the source %s", udest, path);
        status = SOUP_STATUS_CONFLICT;
        goto end;
      }
  }

  job = g_new0 (MoveCopyJob, 1);
  job->req = phodav_request_ref (req);
  job->handler = path_handler_ref (handler);
  job->path = g_strdup (path);
  job->dest = g_strdup (udest);
  job->depth = depth;
  job->copy = copy;
  job->cancellable = g_object_ref (phodav_request_get_cancellable (req));
  job->status = SOUP_STATUS_OK;
  phodav_request_set_method_data (req, job, (GDestroyNotify) movecopy_job_free);

  status = phodav_check_if (req, &job->submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  /* moving a collection removes its members with it: the tokens of
   * the locks held on them must be submitted too. A COPY leaves the
   * source untouched, its locks do not apply. */
  if (!job->copy &&
      phodav_lock_store_tree_has_other_locks (server, job->path,
                                              job->submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  /* overwriting a collection removes its members: their lock tokens
   * must be submitted too, not only the ones of the destination and
   * its ancestors */
  if (phodav_lock_store_path_has_other_locks (server, job->dest,
                                              job->submitted) ||
      phodav_lock_store_tree_has_other_locks (server, job->dest,
                                              job->submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  job->removed = g_ptr_array_new_with_free_func (g_free);
  status = SOUP_STATUS_OK;

end:
  if (dest_uri)
    g_uri_unref (dest_uri);
  return status;
}

const PhodavMethodOps phodav_method_movecopy_ops = {
  .validate = movecopy_validate,
  .work = movecopy_work,
  .commit = movecopy_commit,
};
