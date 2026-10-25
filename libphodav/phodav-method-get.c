/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-priv.h"
#include "phodav-utils.h"

/* the directory listings are generated on the fly in memory, from an
 * enumeration of the whole directory: cap them, so that a huge
 * directory cannot exhaust the memory of the server. The limit is
 * well above what a person browses in a share. */
#define MAX_DIRECTORY_ENTRIES 4096
#define MAX_DIRECTORY_LISTING_SIZE (8 * 1024 * 1024)

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
  g_autoptr(GFileEnumerator) e = NULL;
  g_autoptr(GPtrArray) entries = NULL;
  GString *listing;
  gchar *escaped;
  gchar *name;
  gint i;

  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE,
                                 cancellable, err);
  if (e == NULL)
    return NULL;

  entries = g_ptr_array_new_with_free_func (g_free);

  while (TRUE)
    {
      GFileInfo *info;
      gboolean dir;

      /* the info is borrowed from the enumerator, and only valid
       * until the next iteration */
      if (!g_file_enumerator_iterate (e, &info, NULL, cancellable, err))
        return NULL; /* the error is set: serve no partial listing */

      if (info == NULL)
        break; /* the end of the directory */

      if (entries->len >= MAX_DIRECTORY_ENTRIES)
        {
          g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                               "The directory has too many entries to list");
          return NULL;
        }

      dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

      /* remember the directory entries by their trailing slash */
      g_ptr_array_add (entries, g_strdup_printf ("%s%s",
                                                 g_file_info_get_name (info), dir ? "/" : ""));
    }

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

      if (listing->len + strlen (line) > MAX_DIRECTORY_LISTING_SIZE)
        {
          g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                               "The directory listing is too large");
          g_string_free (listing, TRUE);
          return NULL;
        }

      g_string_append (listing, line);
    }
  g_string_append (listing, "</p></body>\r\n</html>\r\n");

  return listing;
}

/* the headers which are common to the GET and HEAD responses of a
 * directory */
static void
method_get_directory_set_headers (PhodavRequest *req)
{
  SoupMessageHeaders *response_headers = phodav_request_get_response_headers (req);

  soup_message_headers_set_content_type (response_headers,
                                         "text/html; charset=utf-8", NULL);
  /* the listing is generated on the fly, it must not be cached */
  soup_message_headers_append (response_headers, "Cache-Control", "no-cache");
  /* the page lists names taken from the shared folder, which
   * must not be mistaken for another content type, and nothing
   * is ever loaded from it */
  soup_message_headers_append (response_headers, "X-Content-Type-Options", "nosniff");
  soup_message_headers_append (response_headers, "Content-Security-Policy", "default-src 'none'");
}

static gint
method_get_directory (PhodavRequest *req, GFile *file, GCancellable *cancellable, GError **error)
{
  GString *listing;
  gsize len;

  method_get_directory_set_headers (req);

  /* a HEAD request carries no representation, and the listing is
   * generated on the fly: do not enumerate the whole directory just
   * to throw its content away */
  if (g_strcmp0 (phodav_request_get_method (req), "HEAD") == 0)
    return SOUP_STATUS_OK;

  listing = get_directory_listing (file, cancellable, error);
  if (!listing)
    {
      if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE))
        g_debug ("GET: refusing the oversized directory listing of %s", g_file_peek_path (file));

      /* the directory disappeared, cannot be read, or is too large
       * to be listed: answer with the mapped status */
      return g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
               ? SOUP_STATUS_NOT_FOUND
               : SOUP_STATUS_FORBIDDEN;
    }

  len = listing->len;
  phodav_request_set_response (req, "text/html; charset=utf-8",
                               SOUP_MEMORY_TAKE,
                               g_string_free_and_steal (listing), len);

  return SOUP_STATUS_OK;
}

static void
method_get_add_validators (SoupMessageHeaders *response_headers, const gchar *etag, guint64 mtime)
{
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
}

static gint
method_get_file_body (PhodavRequest *req, GFile *file, GFileInfo *info, const gchar *etag, guint64 mtime)
{
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  SoupMessageHeaders *response_headers = phodav_request_get_response_headers (req);
  GMappedFile *mapping;
  g_autoptr(GBytes) buffer = NULL;
  g_autofree gchar *path = g_file_get_path (file);
  guint64 size = (guint64) g_file_info_get_size (info);
  guint64 first = 0, last = 0;
  HttpRangeType range;

  range = http_request_range (request_headers, etag, mtime, size, &first, &last);

  /* there is no satisfiable byte in the representation,
   * RFC 9110 section 14.2 */
  if (range == HTTP_RANGE_UNSATISFIABLE)
    {
      g_autofree gchar *content_range = g_strdup_printf ("bytes */%" G_GUINT64_FORMAT, size);

      soup_message_headers_append (response_headers, "Content-Range", content_range);
      return SOUP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE;
    }

  mapping = g_mapped_file_new (path, FALSE, NULL);
  if (!mapping)
    return SOUP_STATUS_INTERNAL_SERVER_ERROR;

  buffer = g_bytes_new_with_free_func (g_mapped_file_get_contents (mapping),
                                       g_mapped_file_get_length (mapping),
                                       (GDestroyNotify) g_mapped_file_unref,
                                       mapping);

  /* the file may have changed since it was queried, do not
   * serve bytes out of the content which is actually mapped */
  if (range == HTTP_RANGE_PARTIAL && last >= (guint64) g_bytes_get_size (buffer))
    range = HTTP_RANGE_NONE;

  if (range == HTTP_RANGE_PARTIAL)
    {
      g_autoptr(GBytes) slice = g_bytes_new_from_bytes (buffer, first, last - first + 1);
      g_autofree gchar *content_range =
        g_strdup_printf ("bytes %" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT,
                         first, last, size);

      phodav_request_set_body_bytes (req, g_bytes_ref (slice));
      soup_message_headers_append (response_headers, "Content-Range", content_range);
      return SOUP_STATUS_PARTIAL_CONTENT;
    }

  /* an empty file has no content to append, libsoup refuses
   * empty bodies */
  if (g_bytes_get_size (buffer) > 0)
    phodav_request_set_body_bytes (req, g_bytes_ref (buffer));

  return SOUP_STATUS_OK;
}

static gint
method_get_file (PhodavRequest *req, GFile *file, GFileInfo *info)
{
  g_autofree gchar *etag = phodav_etag_from_info (info);
  guint64 mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  const gchar *content_type;
  SoupMessageHeaders *response_headers = phodav_request_get_response_headers (req);
  const gchar *method;

  g_warn_if_fail (etag != NULL);

  method_get_add_validators (response_headers, etag, mtime);

  if (http_request_not_modified (phodav_request_get_request_headers (req), etag, mtime))
    {
      /* RFC 9110 section 15.4.5: no body, and no representation
       * metadata beyond the validators */
      soup_message_headers_remove (response_headers, "Content-Type");
      return SOUP_STATUS_NOT_MODIFIED;
    }

  /* special files may have no content type, do not leave the
   * Content-Type header unset */
  content_type = g_file_info_get_content_type (info);
  soup_message_headers_set_content_type (response_headers,
                                         content_type ? content_type : "application/octet-stream",
                                         NULL);

  /* byte ranges are supported on files, RFC 9110 section 14.4 */
  soup_message_headers_append (response_headers, "Accept-Ranges", "bytes");

  method = phodav_request_get_method (req);
  if (g_strcmp0 (method, "GET") == 0)
    return method_get_file_body (req, file, info, etag, mtime);

  if (g_strcmp0 (method, "HEAD") == 0)
    {
      g_autofree gchar *length = NULL;

      /* We could just use the same code for both GET and
       * HEAD (soup-message-server-io.c will fix things up).
       * But we'll optimize and avoid the extra I/O.
       */
      length = g_strdup_printf ("%" G_GUINT64_FORMAT, g_file_info_get_size (info));
      soup_message_headers_append (response_headers, "Content-Length", length);

      return SOUP_STATUS_OK;
    }

  g_warn_if_reached ();
  return SOUP_STATUS_NOT_FOUND;
}

static gint
method_get (PhodavRequest *req, GFile *file, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = NULL;
  gint status = SOUP_STATUS_NOT_FOUND;

  info = g_file_query_info (file, "standard::*,etag::*,time::modified,unix::inode",
                            G_FILE_QUERY_INFO_NONE, cancellable, &error);
  if (!info)
    goto end;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    status = method_get_directory (req, file, cancellable, &error);
  else
    status = method_get_file (req, file, info);

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
phodav_method_get (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  GFile *file;
  GCancellable *cancellable = phodav_request_get_cancellable (req);
  gint status;

  file = handler_get_file_for_path (handler, phodav_request_get_path (req));
  status = method_get (req, file, cancellable, err);
  g_object_unref (file);

  return status;
}
