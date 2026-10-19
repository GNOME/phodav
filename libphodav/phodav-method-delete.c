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

#include "config.h"

#include "phodav-multistatus.h"
#include "phodav-priv.h"

static gint
error_to_status (GError *err)
{
  if (g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    return SOUP_STATUS_NOT_FOUND;
  if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    return SOUP_STATUS_NOT_FOUND;

  return SOUP_STATUS_FORBIDDEN;
}

gint
phodav_delete_file (const gchar *path, GFile *file, GHashTable *mstatus, GCancellable *cancellable)
{
  g_autoptr(GFileEnumerator) e = NULL;
  g_autoptr(GError) error = NULL;
  gint status = SOUP_STATUS_NO_CONTENT;

  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE,
                                 cancellable, NULL);
  if (e)
    {
      while (1)
        {
          g_autoptr(GFileInfo) info = g_file_enumerator_next_file (e, cancellable, &error);
          if (!info)
            break;
          g_autoptr(GFile) del = g_file_get_child (file, g_file_info_get_name (info));
          g_autofree gchar *escape = g_markup_escape_text (g_file_info_get_name (info), -1);
          g_autofree gchar *del_path = g_build_path ("/", path, escape, NULL);
          phodav_delete_file (del_path, del, mstatus, cancellable);
        }

      g_file_enumerator_close (e, cancellable, NULL);
    }

  if (error)
    g_warning ("DELETE: enumeration error: %s", error->message);

  g_autoptr(GError) del_error = NULL;
  if (!g_file_delete (file, cancellable, &del_error) && mstatus)
    {
      status = error_to_status (del_error);

      g_hash_table_insert (mstatus, g_strdup (path),
                           response_new (NULL, status));
    }

  if (del_error)
    g_debug ("ignored del error: %s", del_error->message);

  return status;
}

gint
phodav_method_delete (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  GFile *file = NULL;
  g_autoptr(GHashTable) mstatus = NULL;
  gint status;
  GList *submitted = NULL;

  /* depth = depth_from_string(soup_message_headers_get_one (msg->request_headers, "Depth")); */
  /* must be == infinity with collection */

  status = phodav_check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) response_free);

  status = phodav_delete_file (path, file, mstatus, cancellable);
  if (status == SOUP_STATUS_NO_CONTENT)
    if (g_hash_table_size (mstatus) > 0)
      status = set_response_multistatus (msg, mstatus);

end:
  g_clear_object (&file);

  return status;
}
