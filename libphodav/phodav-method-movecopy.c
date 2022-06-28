/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "phodav-priv.h"
#include "phodav-utils.h"
#include "phodav-lock.h"
#include "phodav-virtual-dir.h"

static gboolean
do_copy_r (GFile *src, GFile *dest, GFileCopyFlags flags,
           GCancellable *cancellable, GError **err)
{
  GFileEnumerator *e = NULL;
  gboolean success = FALSE;
  GFile *src_child = NULL;
  GFile *dest_child = NULL;

  if (!g_file_make_directory_with_parents (dest, cancellable, err))
    goto end;

  e = g_file_enumerate_children (src, "standard::*", G_FILE_QUERY_INFO_NONE,
                                 cancellable, err);
  if (!e)
    goto end;

  while (1)
    {
      GFileInfo *info = g_file_enumerator_next_file (e, cancellable, err);
      if (!info)
        break;

      src_child = g_file_get_child (src, g_file_info_get_name (info));
      dest_child = g_file_get_child (dest, g_file_info_get_name (info));
      gboolean isdir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
      g_object_unref (info);

      if (isdir)
        {
          if (!do_copy_r (src_child, dest_child, flags, cancellable, err))
            goto end;
        }
      else if (!g_file_copy (src_child, dest_child, flags, cancellable, NULL, NULL, err))
        goto end;

      g_clear_object (&src_child);
      g_clear_object (&dest_child);
    }

  success = TRUE;

end:
  g_clear_object (&e);
  g_clear_object (&src_child);
  g_clear_object (&dest_child);

  return success;
}

static gint
do_movecopy_file (SoupServerMessage *msg, GFile *file,
                  GFile *dest, const gchar *dest_path,
                  GCancellable *cancellable, GError **err)
{
  GError *error = NULL;
  gboolean overwrite;
  DepthType depth;
  gint status = SOUP_STATUS_PRECONDITION_FAILED;
  gboolean copy = soup_server_message_get_method (msg) == SOUP_METHOD_COPY;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  GFileCopyFlags flags = G_FILE_COPY_ALL_METADATA;
  gboolean retry = FALSE;
  gboolean exists;

  depth = depth_from_string (soup_message_headers_get_one (request_headers, "Depth"));
  overwrite = !!g_strcmp0 (
    soup_message_headers_get_one (request_headers, "Overwrite"), "F");
  if (overwrite)
    flags |= G_FILE_COPY_OVERWRITE;
  exists = g_file_query_exists (dest, cancellable);

again:
  switch (depth)
    {
    case DEPTH_INFINITY:
    case DEPTH_ZERO: {
        copy
        ? g_file_copy (file, dest, flags, cancellable, NULL, NULL, &error)
        : g_file_move (file, dest, flags, cancellable, NULL, NULL, &error);

        if (overwrite && !retry &&
            (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY) ||
             g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE)) &&
            phodav_delete_file (dest_path, dest, NULL, cancellable) == SOUP_STATUS_NO_CONTENT)
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

        break;
      }

    default:
      g_warn_if_reached ();
    }

  if (error)
    g_propagate_error (err, error);
  else
    status = exists ? SOUP_STATUS_NO_CONTENT : SOUP_STATUS_CREATED;

end:
  return status;
}

gint
phodav_method_movecopy (PathHandler *handler, SoupServerMessage *msg,
                        const char *path, GError **err)
{
  GFile *file = NULL, *dest_file = NULL;
  GCancellable *cancellable = handler_get_cancellable (handler);
  GUri *dest_uri = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;
  const gchar *dest;
  gchar *udest;
  GList *submitted = NULL;

  dest = soup_message_headers_get_one (soup_server_message_get_request_headers (msg), "Destination");
  if (!dest)
    goto end;
  dest_uri = g_uri_parse (dest, SOUP_HTTP_URI_FLAGS, NULL);
  dest = g_uri_get_path (dest_uri);
  if (!dest || !*dest)
    goto end;

  status = phodav_check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  if (server_path_has_other_locks (handler_get_server (handler), dest, submitted))
    {
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  udest = g_uri_unescape_string (dest + 1, NULL);
  dest_file = g_file_get_child (handler_get_file (handler), udest);
  g_free (udest);

  file = g_file_get_child (handler_get_file (handler), path + 1);

  /* take the short path as g_file_copy seems to be rather complicated */
  if (PHODAV_IS_VIRTUAL_DIR (file) || PHODAV_IS_VIRTUAL_DIR (dest_file))
    {
      status = SOUP_STATUS_FORBIDDEN;
      goto end;
    }
  status = do_movecopy_file (msg, file, dest_file, dest,
                             cancellable, err);

end:
  if (dest_uri)
    g_uri_unref (dest_uri);
  g_clear_object (&file);
  g_clear_object (&dest_file);
  g_list_free_full (submitted, (GDestroyNotify) lock_submitted_free);
  return status;
}
