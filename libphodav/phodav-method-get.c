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

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  const char **sa = (const char * *) a;
  const char **sb = (const char * *) b;

  return g_strcmp0 (*sa, *sb);
}

static GString *
get_directory_listing (GFile *file, GCancellable *cancellable, GError **err)
{
  GString *listing;
  GPtrArray *entries;
  GFileEnumerator *e;
  gchar *escaped;
  gchar *name;
  gint i;

  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE,
                                 cancellable, err);
  g_return_val_if_fail (e != NULL, NULL);

  entries = g_ptr_array_new ();
  while (1)
    {
      GFileInfo *info = g_file_enumerator_next_file (e, cancellable, err);
      gboolean dir;

      if (!info)
        break;

      dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

      escaped = g_markup_printf_escaped ("%s%s",
                                         g_file_info_get_name (info), dir ? "/" : "");
      g_ptr_array_add (entries, escaped);
      g_object_unref (info);
    }
  g_file_enumerator_close (e, cancellable, NULL);
  g_clear_object (&e);

  g_ptr_array_sort (entries, compare_strings);

  listing = g_string_new ("<html>\r\n");
  name = g_file_get_basename (file);
  escaped = g_markup_escape_text (name, -1);
  g_free (name);
  g_string_append_printf (listing, "<head><title>Index of %s</title></head>\r\n", escaped);
  g_string_append_printf (listing, "<body><h1>Index of %s</h1>\r\n<p>\r\n", escaped);
  g_free (escaped);
  for (i = 0; i < entries->len; i++)
    {
      g_string_append_printf (listing, "<a href=\"%s\">%s</a><br/>\r\n",
                              (gchar *) entries->pdata[i],
                              (gchar *) entries->pdata[i]);
      g_free (entries->pdata[i]);
    }
  g_string_append (listing, "</p></body>\r\n</html>\r\n");

  g_ptr_array_free (entries, TRUE);
  return listing;
}

static gint
method_get (SoupServerMessage *msg, GFile *file,
            GCancellable *cancellable, GError **err)
{
  GError *error = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;
  GFileInfo *info;
  const gchar *etag;
  SoupMessageHeaders *response_headers;
  const char *method;

  info = g_file_query_info (file, "standard::*,etag::*",
                            G_FILE_QUERY_INFO_NONE, cancellable, &error);
  if (!info)
    goto end;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      GString *listing;
      gsize len;

      listing = get_directory_listing (file, cancellable, err);
      len = listing->len;
      soup_server_message_set_response (msg, "text/html; charset=utf-8",
                                        SOUP_MEMORY_TAKE,
                                        g_string_free_and_steal (listing), len);
      status = SOUP_STATUS_OK;
      goto end;
    }

  etag = g_file_info_get_etag (info);
  g_warn_if_fail (etag != NULL);

  response_headers = soup_server_message_get_response_headers (msg);

  if (etag)
    {
      gchar *tmp = g_strdup_printf ("\"%s\"", etag);
      soup_message_headers_append (response_headers, "ETag", tmp);
      g_free (tmp);
    }

  soup_message_headers_set_content_type (response_headers,
                                         g_file_info_get_content_type (info), NULL);

  method = soup_server_message_get_method (msg);
  if (method == SOUP_METHOD_GET)
    {
      GMappedFile *mapping;
      GBytes *buffer;
      gchar *path = g_file_get_path (file);

      mapping = g_mapped_file_new (path, FALSE, NULL);
      g_free (path);
      if (!mapping)
        {
          status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
          goto end;
        }

      buffer = g_bytes_new_with_free_func (g_mapped_file_get_contents (mapping),
                                           g_mapped_file_get_length (mapping),
                                           (GDestroyNotify) g_mapped_file_unref,
                                           mapping);
      soup_message_body_append_bytes (soup_server_message_get_response_body (msg), buffer);
      g_bytes_unref (buffer);
      status = SOUP_STATUS_OK;
    }
  else if (method == SOUP_METHOD_HEAD)
    {
      gchar *length;

      /* We could just use the same code for both GET and
       * HEAD (soup-message-server-io.c will fix things up).
       * But we'll optimize and avoid the extra I/O.
       */
      length = g_strdup_printf ("%" G_GUINT64_FORMAT, g_file_info_get_size (info));
      soup_message_headers_append (response_headers, "Content-Length", length);

      g_free (length);
      status = SOUP_STATUS_OK;
    }
  else
    g_warn_if_reached ();

end:
  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_debug ("getfile: %s", error->message);
          g_clear_error (&error);
        }
      else
        g_propagate_error (err, error);
    }

  g_clear_object (&info);
  return status;
}

gint
phodav_method_get (PathHandler *handler, SoupServerMessage *msg, const char *path, GError **err)
{
  GFile *file;
  GCancellable *cancellable = handler_get_cancellable (handler);
  gint status;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  status = method_get (msg, file, cancellable, err);
  g_object_unref (file);

  return status;
}
