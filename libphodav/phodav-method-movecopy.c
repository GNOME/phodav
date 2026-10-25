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
do_copy_r (PathHandler *handler, const gchar *src_path, const gchar *dest_path, GFile *src, GFile *dest, GFileCopyFlags flags, GCancellable *cancellable, GError **err)
{
  g_autoptr(GFileEnumerator) e = NULL;

  if (!g_file_make_directory_with_parents (dest, cancellable, err))
    return FALSE;

  e = g_file_enumerate_children (src, "standard::*",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, err);
  if (!e)
    return FALSE;

  while (1)
    {
      g_autoptr(GFileInfo) info = g_file_enumerator_next_file (e, cancellable, err);
      g_autoptr(GFile) src_child = NULL;
      g_autoptr(GFile) dest_child = NULL;
      g_autofree gchar *src_child_path = NULL;
      g_autofree gchar *dest_child_path = NULL;
      g_autofree gchar *name = NULL;
      gint status;
      gboolean isdir;

      if (!info)
        break;

      if ((g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK))
        {
          g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                               "COPY does not follow symbolic links");
          return FALSE;
        }

      name = g_strdup (g_file_info_get_name (info));
      src_child_path = g_build_path ("/", src_path, name, NULL);
      dest_child_path = g_build_path ("/", dest_path, name, NULL);

      src_child = phodav_path_resolve (handler, src_child_path,
                                       PHODAV_PATH_EXISTING, &status);
      if (!src_child)
        {
          g_set_error_literal (err, G_IO_ERROR,
                               status == SOUP_STATUS_FORBIDDEN
                                 ? G_IO_ERROR_PERMISSION_DENIED
                                 : G_IO_ERROR_FAILED,
                               "Cannot resolve the COPY source");
          return FALSE;
        }

      dest_child = phodav_path_resolve (handler, dest_child_path,
                                        PHODAV_PATH_CREATE, &status);
      if (!dest_child)
        {
          g_set_error_literal (err, G_IO_ERROR,
                               status == SOUP_STATUS_FORBIDDEN
                                 ? G_IO_ERROR_PERMISSION_DENIED
                                 : G_IO_ERROR_FAILED,
                               "Cannot resolve the COPY destination");
          return FALSE;
        }

      isdir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

      if (isdir)
        {
          if (!do_copy_r (handler, src_child_path, dest_child_path,
                          src_child, dest_child, flags, cancellable, err))
            return FALSE;
        }
      else if (!g_file_copy (src_child, dest_child, flags, cancellable, NULL, NULL, err))
        return FALSE;
    }

  return TRUE;
}

static gboolean
movecopy_once (gboolean copy, GFile *file, GFile *dest, GFileCopyFlags flags, GCancellable *cancellable, GError **err)
{
  return copy
           ? g_file_copy (file, dest, flags, cancellable, NULL, NULL, err)
           : g_file_move (file, dest, flags, cancellable, NULL, NULL, err);
}

static gint
do_movecopy_file (PhodavRequest *req, PhodavServer *server, GFile *file, GFile *dest, const gchar *src_path, const gchar *dest_path, DepthType depth, GCancellable *cancellable, GError **err)
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
  exists = g_file_query_exists (dest, cancellable);

  /* replacing a collection removes it together with its members: delete
   * it first (RFC 4918 section 9.6.1). This also avoids relying on the
   * copy error to detect a directory destination: the win32 backend
   * reports G_IO_ERROR_PERMISSION_DENIED where the POSIX one reports
   * G_IO_ERROR_IS_DIRECTORY or G_IO_ERROR_WOULD_MERGE. */
  if (overwrite && exists)
    {
      g_autoptr(GFileInfo) src_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                         cancellable, NULL);
      g_autoptr(GFileInfo) dest_info = g_file_query_info (dest, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                          cancellable, NULL);

      if (src_info && dest_info &&
          (g_file_info_get_file_type (src_info) == G_FILE_TYPE_DIRECTORY ||
           g_file_info_get_file_type (dest_info) == G_FILE_TYPE_DIRECTORY))
        phodav_delete_file (server, dest_path, dest, NULL, cancellable);
    }

  /* the request was validated to be depth 0 or infinity */
  ok = movecopy_once (copy, file, dest, flags, cancellable, &error);

  /* an existing destination of another kind cannot be replaced in
   * place: remove it, then try again with the destination gone */
  if (!ok && overwrite &&
      (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY) ||
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE)) &&
      phodav_delete_file (server, dest_path, dest, NULL, cancellable) == SOUP_STATUS_NO_CONTENT)
    {
      g_clear_error (&error);
      ok = movecopy_once (copy, file, dest, flags, cancellable, &error);
    }

  /* a directory cannot be copied in one go; the depth of the request
   * decides whether its members are copied too */
  if (!ok && copy &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE))
    {
      g_clear_error (&error);
      ok = depth == DEPTH_INFINITY
             ? do_copy_r (phodav_request_get_handler (req), src_path, dest_path,
                          file, dest, flags, cancellable, &error)
             : g_file_make_directory_with_parents (dest, cancellable, &error);
    }

  if (ok)
    return exists ? SOUP_STATUS_NO_CONTENT : SOUP_STATUS_CREATED;

  if (!overwrite && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    return SOUP_STATUS_PRECONDITION_FAILED;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    return SOUP_STATUS_CONFLICT;

  g_propagate_error (err, g_steal_pointer (&error));
  return SOUP_STATUS_INTERNAL_SERVER_ERROR;
}

gint
phodav_method_movecopy (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  const gchar *path = phodav_request_get_path (req);
  GFile *file = NULL, *dest_file = NULL;
  GCancellable *cancellable = phodav_request_get_cancellable (req);
  PhodavServer *server = handler_get_server (handler);
  GUri *dest_uri = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;
  const gchar *dest;
  gboolean copy = g_strcmp0 (phodav_request_get_method (req), "COPY") == 0;
  DepthType depth = DEPTH_INFINITY;
  g_autofree gchar *udest = NULL;
  g_autolist(LockSubmitted) submitted = NULL;
  PhodavLockGuard *guard = NULL;
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

  status = phodav_check_if (req, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  {
    GList *paths = NULL;

    paths = g_list_prepend (paths, (gpointer) path);
    paths = g_list_prepend (paths, (gpointer) udest);
    guard = phodav_lock_store_guard_acquire (server, paths);
    g_list_free (paths);
  }

  /* moving a collection removes its members with it: the tokens of
   * the locks held on them must be submitted too. A COPY leaves the
   * source untouched, its locks do not apply. */
  if (!copy && phodav_lock_store_tree_has_other_locks (server, path, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  /* overwriting a collection removes its members: their lock tokens
   * must be submitted too, not only the ones of the destination and
   * its ancestors */
  if (phodav_lock_store_path_has_other_locks (server, udest, submitted) ||
      phodav_lock_store_tree_has_other_locks (server, udest, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  file = phodav_path_resolve (handler, path, PHODAV_PATH_EXISTING, &status);
  if (!file)
    {
      /* a missing MOVE/COPY source is a conflict, like the backend
       * error the copy operation used to report */
      if (status == SOUP_STATUS_NOT_FOUND)
        status = SOUP_STATUS_CONFLICT;
      goto end;
    }

  dest_file = phodav_path_resolve (handler, udest, PHODAV_PATH_CREATE, &status);
  if (!dest_file)
    goto end;

  /* take the short path as g_file_copy seems to be rather complicated */
  if (PHODAV_IS_VIRTUAL_DIR (file) || PHODAV_IS_VIRTUAL_DIR (dest_file))
    {
      status = SOUP_STATUS_FORBIDDEN;
      goto end;
    }
  status = do_movecopy_file (req, server, file, dest_file, path, udest, depth,
                             cancellable, err);

end:
  phodav_lock_store_guard_release (guard);
  if (dest_uri)
    g_uri_unref (dest_uri);
  g_clear_object (&file);
  g_clear_object (&dest_file);
  return status;
}
