/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-priv.h"
#include "phodav-utils.h"

#include <errno.h>
#include <glib/gstdio.h>

#ifdef G_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#define STREAM_CHUNK_SIZE (64 * 1024)

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

static gboolean
stream_write_string (PhodavStream *stream, const gchar *str)
{
  return phodav_stream_write (stream, (const guint8 *) str, strlen (str));
}

/* A failure which follows the client going away needs no answer: the
 * message is already finished, and cutting the connection would only
 * risk discarding a connection the client may still reuse. Every other
 * failure (a read error, a short read, a server-side cancellation) must
 * abort the response, so that a truncated body is not mistaken for the
 * complete representation. */
static void
stream_finish (PhodavRequest *req, PhodavStream *stream, gboolean ok)
{
  GCancellable *cancellable = phodav_request_get_cancellable (req);

  if (ok || (g_cancellable_is_cancelled (cancellable) &&
             phodav_request_is_finished (req)))
    phodav_stream_end (stream);
  else
    phodav_stream_abort (stream);
}

static gint
method_get_directory (PhodavRequest *req, GFile *file, GCancellable *cancellable, GError **error)
{
  g_autoptr(GFileEnumerator) e = NULL;
  g_autoptr(GString) header = NULL;
  PhodavStream *stream;
  g_autofree gchar *name = NULL;
  g_autofree gchar *escaped = NULL;
  gboolean ok = TRUE;
  gint status = SOUP_STATUS_OK;

  method_get_directory_set_headers (req);

  /* a HEAD request carries no representation, and the listing is
   * generated on the fly: do not enumerate the whole directory just
   * to throw its content away */
  if (g_strcmp0 (phodav_request_get_method (req), "HEAD") == 0)
    return SOUP_STATUS_OK;

  e = g_file_enumerate_children (file, "standard::*",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!e)
    return g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
             ? SOUP_STATUS_NOT_FOUND
             : SOUP_STATUS_FORBIDDEN;

  stream = phodav_request_stream_start (req, SOUP_STATUS_OK,
                                        "text/html; charset=utf-8");
  if (!stream)
    return SOUP_STATUS_INTERNAL_SERVER_ERROR;

  name = g_file_get_basename (file);
  escaped = g_markup_escape_text (name, -1);
  header = g_string_new ("<html>\r\n");
  g_string_append_printf (header, "<head><title>Index of %s</title></head>\r\n",
                          escaped);
  g_string_append_printf (header, "<body><h1>Index of %s</h1>\r\n<p>\r\n",
                          escaped);
  if (!stream_write_string (stream, header->str))
    {
      ok = FALSE;
      goto out;
    }

  while (TRUE)
    {
      GFileInfo *info;
      gboolean dir;
      g_autofree gchar *base = NULL;
      g_autofree gchar *href = NULL;
      g_autofree gchar *line = NULL;

      if (!g_file_enumerator_iterate (e, &info, NULL, cancellable, error))
        {
          ok = FALSE;
          break;
        }

      if (info == NULL)
        break;

      if ((g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK))
        continue;

      dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
      base = g_strdup (g_file_info_get_name (info));
      href = g_uri_escape_string (base, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
      line = g_markup_printf_escaped ("<a href=\"%s%s\">%s</a><br/>\r\n",
                                      href, dir ? "/" : "", base);

      if (!stream_write_string (stream, line))
        {
          ok = FALSE;
          break;
        }
    }

  if (ok && !stream_write_string (stream, "</p></body>\r\n</html>\r\n"))
    ok = FALSE;

out:
  g_file_enumerator_close (e, cancellable, NULL);
  stream_finish (req, stream, ok);

  return status;
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

static gboolean
stream_file_range (PhodavRequest *req, GFile *file, const gchar *path, guint64 first, guint64 count, PhodavStream *stream, GCancellable *cancellable)
{
  g_autofree guint8 *buffer = g_malloc (STREAM_CHUNK_SIZE);
  gboolean ok = TRUE;

#ifdef G_OS_UNIX
  int fd;
  guint64 offset = first;
  guint64 remaining = count;

  fd = g_open (path, O_RDONLY, 0);
  if (fd < 0)
    return FALSE;

  while (remaining > 0 && !g_cancellable_is_cancelled (cancellable))
    {
      gsize want = MIN ((guint64) STREAM_CHUNK_SIZE, remaining);
      ssize_t n = pread (fd, buffer, want, offset);

      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          ok = FALSE;
          break;
        }
      if (n == 0)
        {
          /* the file shrank while it was being sent: fewer bytes than
           * the announced representation must not look complete */
          ok = FALSE;
          break;
        }

      if (!phodav_stream_write (stream, buffer, n))
        {
          ok = FALSE;
          break;
        }

      offset += n;
      remaining -= n;
    }

  close (fd);
  ok = ok && remaining == 0;
#else
  g_autoptr(GFileInputStream) in = NULL;
  guint64 remaining = count;

  in = g_file_read (file, cancellable, NULL);
  if (!in)
    return FALSE;

  if (first > 0 && G_IS_SEEKABLE (in) &&
      !g_seekable_seek (G_SEEKABLE (in), first, G_SEEK_SET, cancellable, NULL))
    return FALSE;

  while (remaining > 0 && !g_cancellable_is_cancelled (cancellable))
    {
      gsize want = MIN ((guint64) STREAM_CHUNK_SIZE, remaining);
      gssize n = g_input_stream_read (G_INPUT_STREAM (in), buffer, want,
                                      cancellable, NULL);

      if (n <= 0)
        {
          /* an early EOF or a read error leaves the promised range
           * incomplete */
          ok = FALSE;
          break;
        }

      if (!phodav_stream_write (stream, buffer, n))
        {
          ok = FALSE;
          break;
        }

      remaining -= n;
    }

  ok = ok && remaining == 0;
#endif

  return ok;
}

static gint
method_get_file_body (PhodavRequest *req, GFile *file, GFileInfo *info, const gchar *etag, guint64 mtime)
{
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  SoupMessageHeaders *response_headers = phodav_request_get_response_headers (req);
  g_autofree gchar *path = g_file_get_path (file);
  guint64 size = (guint64) g_file_info_get_size (info);
  guint64 first = 0, last = 0;
  guint64 count;
  gint status;
  HttpRangeType range;
  PhodavStream *stream;
  gboolean ok;

  range = http_request_range (request_headers, etag, mtime, size, &first, &last);

  /* there is no satisfiable byte in the representation,
   * RFC 9110 section 14.2 */
  if (range == HTTP_RANGE_UNSATISFIABLE)
    {
      g_autofree gchar *content_range = g_strdup_printf ("bytes */%" G_GUINT64_FORMAT, size);

      soup_message_headers_append (response_headers, "Content-Range", content_range);
      return SOUP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE;
    }

  if (range == HTTP_RANGE_PARTIAL)
    {
      g_autofree gchar *content_range =
        g_strdup_printf ("bytes %" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT,
                         first, last, size);

      soup_message_headers_append (response_headers, "Content-Range", content_range);
      status = SOUP_STATUS_PARTIAL_CONTENT;
      count = last - first + 1;
    }
  else
    {
      status = SOUP_STATUS_OK;
      first = 0;
      count = size;
    }

  /* an empty file has no body to stream */
  if (count == 0)
    return status;

  stream = phodav_request_stream_start (req, status, NULL);
  if (!stream)
    return SOUP_STATUS_INTERNAL_SERVER_ERROR;

  ok = path && stream_file_range (req, file, path, first, count, stream,
                                  phodav_request_get_cancellable (req));
  if (!ok)
    g_debug ("GET: stopped streaming %s", path ?: "(non-native file)");

  stream_finish (req, stream, ok);

  return status;
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
  gint status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

  file = phodav_path_resolve (handler, phodav_request_get_path (req),
                              PHODAV_PATH_EXISTING, &status);
  if (!file)
    return status;

  status = method_get (req, file, cancellable, err);
  g_object_unref (file);

  return status;
}
