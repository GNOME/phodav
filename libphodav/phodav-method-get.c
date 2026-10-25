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

typedef struct _GetDirectoryPull
{
  GFileEnumerator *e;
  GString *buffer;
  gboolean header_done;
  gboolean eof;
} GetDirectoryPull;

static void
get_directory_pull_free (gpointer data)
{
  GetDirectoryPull *pull = data;

  if (pull->e)
    g_file_enumerator_close (pull->e, NULL, NULL);
  g_clear_object (&pull->e);
  if (pull->buffer)
    {
      g_string_free (pull->buffer, TRUE);
      pull->buffer = NULL;
    }
  g_free (pull);
}

static GBytes *
get_directory_pull (gpointer data, GCancellable *cancellable, GError **error)
{
  GetDirectoryPull *pull = data;

  if (pull->eof)
    return NULL;

  while (!pull->eof && pull->buffer->len < STREAM_CHUNK_SIZE)
    {
      GFileInfo *info;
      gboolean dir;
      g_autofree gchar *base = NULL;
      g_autofree gchar *href = NULL;
      g_autofree gchar *line = NULL;

      if (!g_file_enumerator_iterate (pull->e, &info, NULL, cancellable, error))
        return NULL;

      if (info == NULL)
        {
          g_string_append (pull->buffer, "</p></body>\r\n</html>\r\n");
          pull->eof = TRUE;
          break;
        }

      if (g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK)
        continue;

      dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
      base = g_strdup (g_file_info_get_name (info));
      href = g_uri_escape_string (base, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
      line = g_markup_printf_escaped ("<a href=\"%s%s\">%s</a><br/>\r\n",
                                      href, dir ? "/" : "", base);
      g_string_append (pull->buffer, line);
    }

  if (pull->buffer->len == 0)
    return NULL;

  {
    gsize len = pull->buffer->len;
    gchar *data = g_string_free (g_steal_pointer (&pull->buffer), FALSE);

    pull->buffer = g_string_sized_new (STREAM_CHUNK_SIZE);
    return g_bytes_new_take (data, len);
  }
}

static GString *
get_directory_header (const gchar *path)
{
  g_autofree gchar *name = g_path_get_basename (path);
  g_autofree gchar *escaped = g_markup_escape_text (name, -1);
  GString *header = g_string_new ("<html>\r\n");

  g_string_append_printf (header, "<head><title>Index of %s</title></head>\r\n",
                          escaped);
  g_string_append_printf (header, "<body><h1>Index of %s</h1>\r\n<p>\r\n",
                          escaped);

  return header;
}

static gint
method_get_directory (PhodavRequest *req, GFile *file, GCancellable *cancellable, GError **error)
{
  g_autoptr(GFileEnumerator) e = NULL;
  GetDirectoryPull *pull;

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

  pull = g_new0 (GetDirectoryPull, 1);
  pull->e = g_steal_pointer (&e);
  pull->buffer = get_directory_header (phodav_request_get_path (req));

  phodav_request_stream_start (req, SOUP_STATUS_OK,
                               "text/html; charset=utf-8",
                               get_directory_pull, pull,
                               get_directory_pull_free);

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

typedef struct _GetFilePull
{
  GFileInputStream *stream;
  gchar *path;
  int fd;
  guint64 offset;
  guint64 remaining;
} GetFilePull;

static void
get_file_pull_free (gpointer data)
{
  GetFilePull *pull = data;

  if (pull->stream)
    g_input_stream_close (G_INPUT_STREAM (pull->stream), NULL, NULL);
  g_clear_object (&pull->stream);
#ifdef G_OS_UNIX
  if (pull->fd >= 0)
    close (pull->fd);
#endif
  g_free (pull->path);
  g_free (pull);
}

static GBytes *
get_file_pull (gpointer data, GCancellable *cancellable, GError **error)
{
  GetFilePull *pull = data;
  g_autofree guint8 *buffer = g_malloc (STREAM_CHUNK_SIZE);
  gsize want = MIN ((guint64) STREAM_CHUNK_SIZE, pull->remaining);
  gssize n;

  if (pull->remaining == 0)
    return NULL;

#ifdef G_OS_UNIX
  if (pull->fd >= 0)
    {
      do
        n = pread (pull->fd, buffer, want, pull->offset);
      while (n < 0 && errno == EINTR && !g_cancellable_is_cancelled (cancellable));

      if (n < 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "Cannot read %s: %s", pull->path, g_strerror (errno));
          return NULL;
        }
    }
  else
#endif
    {
      n = g_input_stream_read (G_INPUT_STREAM (pull->stream), buffer, want,
                               cancellable, error);
      if (n < 0)
        return NULL;
    }

  if (n == 0)
    {
      /* the file shrank while it was being sent: fewer bytes than the
       * announced representation must not look complete */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unexpected end of %s", pull->path ?: "file");
      return NULL;
    }

  pull->offset += n;
  pull->remaining -= n;

  return g_bytes_new_take (g_steal_pointer (&buffer), n);
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
  GetFilePull *pull;

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

  pull = g_new0 (GetFilePull, 1);
  pull->path = g_strdup (path);
  pull->offset = first;
  pull->remaining = count;
  pull->fd = -1;

#ifdef G_OS_UNIX
  /* the native path avoids going through GIO for the common case */
  if (path && g_file_is_native (file))
    pull->fd = g_open (path, O_RDONLY, 0);
#endif

  if (pull->fd < 0)
    {
      pull->stream = g_file_read (file, phodav_request_get_cancellable (req), NULL);
      if (!pull->stream)
        {
          get_file_pull_free (pull);
          return SOUP_STATUS_INTERNAL_SERVER_ERROR;
        }

      if (first > 0)
        {
          if (G_IS_SEEKABLE (pull->stream))
            {
              if (!g_seekable_seek (G_SEEKABLE (pull->stream), first, G_SEEK_SET,
                                    phodav_request_get_cancellable (req), NULL))
                {
                  get_file_pull_free (pull);
                  return SOUP_STATUS_INTERNAL_SERVER_ERROR;
                }
            }
          else
            {
              /* a non-seekable backend cannot jump to the start of the
               * range: consume the bytes before it */
              g_autofree guint8 *buffer = g_malloc (STREAM_CHUNK_SIZE);
              guint64 skip = first;

              while (skip > 0)
                {
                  gsize want = MIN ((guint64) STREAM_CHUNK_SIZE, skip);
                  gssize n = g_input_stream_read (G_INPUT_STREAM (pull->stream),
                                                  buffer, want,
                                                  phodav_request_get_cancellable (req),
                                                  NULL);

                  if (n <= 0)
                    {
                      get_file_pull_free (pull);
                      return SOUP_STATUS_INTERNAL_SERVER_ERROR;
                    }

                  skip -= n;
                }
            }
        }
    }

  phodav_request_stream_start (req, status, NULL,
                               get_file_pull, pull, get_file_pull_free);

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
  PhodavServer *server = handler_get_server (handler);
  g_autoptr(PhodavPath) handle = NULL;
  GCancellable *cancellable = phodav_request_get_cancellable (req);
  gint status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

  /* the metadata and the file handle are taken under the read lock;
   * the stream keeps reading from the opened handle afterwards */
  phodav_server_read_lock (server);

  handle = phodav_path_resolve (handler, phodav_request_get_path (req),
                                PHODAV_PATH_EXISTING, &status);
  if (!handle)
    {
      phodav_server_read_unlock (server);
      return status;
    }

  status = method_get (req, phodav_path_get_file (handle),
                       cancellable, err);
  phodav_server_read_unlock (server);

  return status;
}
