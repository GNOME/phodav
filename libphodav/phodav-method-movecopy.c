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
do_copy_r (GFile *src, GFile *dest, GFileCopyFlags flags, GCancellable *cancellable, GError **err)
{
  g_autoptr(GFileEnumerator) e = NULL;

  if (!g_file_make_directory_with_parents (dest, cancellable, err))
    return FALSE;

  e = g_file_enumerate_children (src, "standard::*", G_FILE_QUERY_INFO_NONE,
                                 cancellable, err);
  if (!e)
    return FALSE;

  while (1)
    {
      g_autoptr(GFileInfo) info = g_file_enumerator_next_file (e, cancellable, err);
      if (!info)
        break;

      g_autoptr(GFile) src_child = g_file_get_child (src, g_file_info_get_name (info));
      g_autoptr(GFile) dest_child = g_file_get_child (dest, g_file_info_get_name (info));
      gboolean isdir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

      if (isdir)
        {
          if (!do_copy_r (src_child, dest_child, flags, cancellable, err))
            return FALSE;
        }
      else if (!g_file_copy (src_child, dest_child, flags, cancellable, NULL, NULL, err))
        return FALSE;
    }

  return TRUE;
}

static gint
do_movecopy_file (SoupServerMessage *msg, PhodavServer *server, GFile *file, GFile *dest, const gchar *dest_path, DepthType depth, GCancellable *cancellable, GError **err)
{
  GError *error = NULL;
  gboolean overwrite;
  gint status = SOUP_STATUS_PRECONDITION_FAILED;
  gboolean copy = soup_server_message_get_method (msg) == SOUP_METHOD_COPY;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  GFileCopyFlags flags = G_FILE_COPY_ALL_METADATA;
  gboolean retry = FALSE;
  gboolean exists;

  overwrite = !!g_strcmp0 (
    soup_message_headers_get_one (request_headers, "Overwrite"), "F");
  if (overwrite)
    flags |= G_FILE_COPY_OVERWRITE;
  exists = g_file_query_exists (dest, cancellable);

  /* the request was validated to be depth 0 or infinity */
again:
  copy
    ? g_file_copy (file, dest, flags, cancellable, NULL, NULL, &error)
    : g_file_move (file, dest, flags, cancellable, NULL, NULL, &error);

  if (overwrite && !retry &&
      (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY) ||
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE)) &&
      phodav_delete_file (server, dest_path, dest, NULL, cancellable) == SOUP_STATUS_NO_CONTENT)
    {
      g_clear_error (&error);
      retry = TRUE;
      goto again;
    }
  else if (!overwrite &&
           g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      g_clear_error (&error);
      goto end;
    }
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE))
    {
      g_clear_error (&error);
      if (copy)
        {
          if (depth == DEPTH_INFINITY)
            do_copy_r (file, dest, flags, cancellable, &error);
          else
            g_file_make_directory_with_parents (dest, cancellable, &error);
        }
    }
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      status = SOUP_STATUS_CONFLICT;
      g_clear_error (&error);
      goto end;
    }

  if (error)
    g_propagate_error (err, error);
  else
    status = exists ? SOUP_STATUS_NO_CONTENT : SOUP_STATUS_CREATED;

end:
  return status;
}

gint
phodav_method_movecopy (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  GFile *file = NULL, *dest_file = NULL;
  GCancellable *cancellable = handler_get_cancellable (handler);
  GUri *dest_uri = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;
  const gchar *dest;
  DepthType depth = DEPTH_INFINITY;
  g_autofree gchar *udest = NULL;
  g_autolist(LockSubmitted) submitted = NULL;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);

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
  if (!udest || *udest != '/' || g_path_is_absolute (udest + 1) ||
      path_has_dot_dot_segment (udest))
    {
      g_debug ("invalid destination path: %s", dest);
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }

  status = phodav_check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  if (server_path_has_other_locks (handler_get_server (handler), udest, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  dest_file = g_file_get_child (handler_get_file (handler), udest + 1);

  file = g_file_get_child (handler_get_file (handler), path + 1);

  /* take the short path as g_file_copy seems to be rather complicated */
  if (PHODAV_IS_VIRTUAL_DIR (file) || PHODAV_IS_VIRTUAL_DIR (dest_file))
    {
      status = SOUP_STATUS_FORBIDDEN;
      goto end;
    }
  status = do_movecopy_file (msg, handler_get_server (handler), file, dest_file, udest, depth,
                             cancellable, err);

end:
  if (dest_uri)
    g_uri_unref (dest_uri);
  g_clear_object (&file);
  g_clear_object (&dest_file);
  return status;
}
