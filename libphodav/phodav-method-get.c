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

#include "phodav-priv.h"
#include "phodav-utils.h"

static gint
compare_strings (gconstpointer a, gconstpointer b)
{
  const gchar **sa = (const gchar **) a;
  const gchar **sb = (const gchar **) b;

  return g_strcmp0 (*sa, *sb);
}

static GString *
get_directory_listing (GFile *file, GCancellable *cancellable, GError **err)
{
  GString *listing;
  g_autoptr(GPtrArray) entries = NULL;
  GFileEnumerator *e;
  gchar *escaped;
  gchar *name;
  gint i;

  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE,
                                 cancellable, err);
  if (e == NULL)
    return NULL;

  entries = g_ptr_array_new_with_free_func (g_free);
  while (1)
    {
      GFileInfo *info = g_file_enumerator_next_file (e, cancellable, err);
      gboolean dir;

      if (!info)
        break;

      dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

      /* remember the directory entries by their trailing slash */
      g_ptr_array_add (entries, g_strdup_printf ("%s%s",
                                                 g_file_info_get_name (info), dir ? "/" : ""));
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
      const gchar *entry = g_ptr_array_index (entries, i);
      gsize len = strlen (entry);
      gboolean dir = len > 0 && entry[len - 1] == '/';
      g_autofree gchar *base = NULL;
      g_autofree gchar *href = NULL;
      g_autofree gchar *line = NULL;

      /* the href must be URI escaped, the label XML escaped, or names
       * with "?", "&", spaces... would produce broken links */
      base = g_strdup (entry);
      if (dir)
        base[strlen (base) - 1] = '\0';
      href = g_uri_escape_string (base, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
      line = g_markup_printf_escaped ("<a href=\"%s%s\">%s</a><br/>\r\n",
                                      href, dir ? "/" : "", base);
      g_string_append (listing, line);
    }
  g_string_append (listing, "</p></body>\r\n</html>\r\n");

  return listing;
}

static gint
method_get (SoupServerMessage *msg, GFile *file, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;
  g_autoptr(GFileInfo) info = NULL;
  const gchar *etag;
  guint64 mtime;
  const gchar *content_type;
  SoupMessageHeaders *response_headers;
  const gchar *method;

  info = g_file_query_info (file, "standard::*,etag::*,time::modified",
                            G_FILE_QUERY_INFO_NONE, cancellable, &error);
  if (!info)
    goto end;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      GString *listing;
      gsize len;

      listing = get_directory_listing (file, cancellable, &error);
      if (!listing)
        {
          /* the directory disappeared or cannot be read since the
           * query above, answer with the mapped status */
          status = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
                     ? SOUP_STATUS_NOT_FOUND
                     : SOUP_STATUS_FORBIDDEN;
          goto end;
        }

      len = listing->len;
      /* the listing is generated on the fly, it must not be cached */
      soup_message_headers_append (soup_server_message_get_response_headers (msg),
                                   "Cache-Control", "no-cache");
      soup_server_message_set_response (msg, "text/html; charset=utf-8",
                                        SOUP_MEMORY_TAKE,
                                        g_string_free_and_steal (listing), len);
      status = SOUP_STATUS_OK;
      goto end;
    }

  etag = g_file_info_get_etag (info);
  mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  g_warn_if_fail (etag != NULL);

  response_headers = soup_server_message_get_response_headers (msg);

  /* the validators allow a client to revalidate its cached
   * representation with a cheap conditional request, answered
   * with Not Modified */
  if (etag)
    {
      g_autofree gchar *quoted = g_strdup_printf ("\"%s\"", etag);
      soup_message_headers_append (response_headers, "ETag", quoted);
    }

  if (mtime)
    {
      g_autofree gchar *modified = http_time_to_string (mtime);

      if (modified)
        soup_message_headers_append (response_headers, "Last-Modified", modified);
    }

  /* the content may only be reused after revalidation */
  soup_message_headers_append (response_headers, "Cache-Control", "no-cache");

  if (http_request_not_modified (soup_server_message_get_request_headers (msg), etag, mtime))
    {
      /* RFC 9110 section 15.4.5: no body, and no representation
       * metadata beyond the validators */
      soup_message_headers_remove (response_headers, "Content-Type");
      status = SOUP_STATUS_NOT_MODIFIED;
      goto end;
    }

  /* special files may have no content type, do not leave the
   * Content-Type header unset */
  content_type = g_file_info_get_content_type (info);
  soup_message_headers_set_content_type (response_headers,
                                         content_type ? content_type : "application/octet-stream",
                                         NULL);

  method = soup_server_message_get_method (msg);
  if (method == SOUP_METHOD_GET)
    {
      GMappedFile *mapping;
      g_autoptr(GBytes) buffer = NULL;
      g_autofree gchar *path = g_file_get_path (file);

      mapping = g_mapped_file_new (path, FALSE, NULL);
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
      status = SOUP_STATUS_OK;
    }
  else if (method == SOUP_METHOD_HEAD)
    {
      g_autofree gchar *length = NULL;

      /* We could just use the same code for both GET and
       * HEAD (soup-message-server-io.c will fix things up).
       * But we'll optimize and avoid the extra I/O.
       */
      length = g_strdup_printf ("%" G_GUINT64_FORMAT, g_file_info_get_size (info));
      soup_message_headers_append (response_headers, "Content-Length", length);

      status = SOUP_STATUS_OK;
    }
  else
    g_warn_if_reached ();

end:
  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_debug ("getfile: %s", error->message);
      else
        g_propagate_error (err, g_steal_pointer (&error));
    }

  return status;
}

gint
phodav_method_get (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  GFile *file;
  GCancellable *cancellable = handler_get_cancellable (handler);
  gint status;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  status = method_get (msg, file, cancellable, err);
  g_object_unref (file);

  return status;
}
