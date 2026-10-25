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
delete_destination (PhodavServer *server, PhodavPath *dest,
                    const gchar *dest_path, GCancellable *cancellable)
{
  return phodav_path_delete_tree (server, dest, dest_path,
                                  NULL, NULL, cancellable);
}

static gint
do_movecopy_file (PhodavRequest *req, PhodavServer *server,
                  PhodavPath *src, PhodavPath *dest,
                  const gchar *src_path, const gchar *dest_path,
                  DepthType depth, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  gboolean copy = g_strcmp0 (phodav_request_get_method (req), "COPY") == 0;
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  GFileCopyFlags flags = G_FILE_COPY_ALL_METADATA;
  gboolean overwrite;
  gboolean exists;
  gboolean ok;

  overwrite = g_strcmp0 (
                soup_message_headers_get_one (request_headers, "Overwrite"), "F") != 0;
  if (overwrite)
    flags |= G_FILE_COPY_OVERWRITE;
  exists = dest_exists (dest, cancellable);

  /* replacing a collection removes it together with its members: delete
   * it first (RFC 4918 section 9.6.1). This also avoids relying on the
   * copy error to detect a directory destination: the win32 backend
   * reports G_IO_ERROR_PERMISSION_DENIED where the POSIX one reports
   * G_IO_ERROR_IS_DIRECTORY or G_IO_ERROR_WOULD_MERGE. */
  if (overwrite && exists &&
      (path_is_directory (src, cancellable) ||
       path_is_directory (dest, cancellable)))
    delete_destination (server, dest, dest_path, cancellable);

  /* a COPY is recursive when its depth is infinity; a MOVE commits
   * with a rename, which the path layer protects. */
  if (copy)
    ok = phodav_path_copy (src, dest, flags, depth == DEPTH_INFINITY,
                           cancellable, &error);
  else
    ok = phodav_path_move (src, dest, flags, cancellable, &error);

  /* an existing destination of another kind cannot be replaced in
   * place: remove it, then try again with the destination gone */
  if (!ok && overwrite &&
      (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY) ||
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE) ||
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY)) &&
      delete_destination (server, dest, dest_path, cancellable) == SOUP_STATUS_NO_CONTENT)
    {
      g_clear_error (&error);
      if (copy)
        ok = phodav_path_copy (src, dest, flags, depth == DEPTH_INFINITY,
                               cancellable, &error);
      else
        ok = phodav_path_move (src, dest, flags, cancellable, &error);
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
  g_propagate_error (err, g_steal_pointer (&error));
  return SOUP_STATUS_INTERNAL_SERVER_ERROR;
}

gint
phodav_method_movecopy (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  const gchar *path = phodav_request_get_path (req);
  g_autoptr(PhodavPath) handle = NULL;
  g_autoptr(PhodavPath) dest_handle = NULL;
  GCancellable *cancellable = phodav_request_get_cancellable (req);
  PhodavServer *server = handler_get_server (handler);
  GUri *dest_uri = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;
  const gchar *dest;
  gboolean copy = g_strcmp0 (phodav_request_get_method (req), "COPY") == 0;
  DepthType depth = DEPTH_INFINITY;
  g_autofree gchar *udest = NULL;
  g_autolist(LockSubmitted) submitted = NULL;
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);

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

  /* the lock check and the resolution of both resources are the
   * decision point: take the write lock only for them, then let the
   * long copy or the rename run without holding it. */
  phodav_server_write_lock (server);

  status = phodav_check_if (req, &submitted);
  if (status != SOUP_STATUS_OK)
    goto unlock;

  /* moving a collection removes its members with it: the tokens of
   * the locks held on them must be submitted too. A COPY leaves the
   * source untouched, its locks do not apply. */
  if (!copy && phodav_lock_store_tree_has_other_locks (server, path, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto unlock;
    }

  /* overwriting a collection removes its members: their lock tokens
   * must be submitted too, not only the ones of the destination and
   * its ancestors */
  if (phodav_lock_store_path_has_other_locks (server, udest, submitted) ||
      phodav_lock_store_tree_has_other_locks (server, udest, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto unlock;
    }

  handle = phodav_path_resolve (handler, path, PHODAV_PATH_EXISTING, &status);
  if (!handle)
    {
      /* a missing MOVE/COPY source is reported as a conflict */
      if (status == SOUP_STATUS_NOT_FOUND)
        status = SOUP_STATUS_CONFLICT;
      goto unlock;
    }

  dest_handle = phodav_path_resolve (handler, udest, PHODAV_PATH_CREATE, &status);
  if (!dest_handle)
    goto unlock;

  /* take the short path as g_file_copy seems to be rather complicated */
  if (PHODAV_IS_VIRTUAL_DIR (phodav_path_get_file (handle)) ||
      PHODAV_IS_VIRTUAL_DIR (phodav_path_get_named_file (dest_handle)))
    {
      status = SOUP_STATUS_FORBIDDEN;
      goto unlock;
    }

unlock:
  phodav_server_write_unlock (server);

  if (status == SOUP_STATUS_OK && handle && dest_handle)
    status = do_movecopy_file (req, server, handle, dest_handle, path,
                               udest, depth, cancellable, err);

end:
  if (dest_uri)
    g_uri_unref (dest_uri);
  return status;
}
