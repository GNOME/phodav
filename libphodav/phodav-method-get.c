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

typedef struct _GetJob
{
  PhodavRequest *req;
  PathHandler *handler;
  gchar *path;
  gboolean head;
  GCancellable *cancellable;
  PhodavPath *handle;
  GFileInfo *info;
  GFileEnumerator *e;
  GFileInputStream *stream;
  gchar *file_path;
  int fd;
  guint64 first;
  guint64 count;
  gint status;
  GError *error;
} GetJob;

static void
get_job_free (GetJob *job)
{
  g_clear_pointer (&job->req, phodav_request_unref);
  g_clear_pointer (&job->handler, path_handler_unref);
  g_free (job->path);
  g_clear_object (&job->cancellable);
  g_clear_pointer (&job->handle, phodav_path_unref);
  g_clear_object (&job->info);
  g_clear_object (&job->e);
  g_clear_object (&job->stream);
  g_free (job->file_path);
#ifdef G_OS_UNIX
  if (job->fd >= 0)
    close (job->fd);
#endif
  g_clear_error (&job->error);
  g_free (job);
}

static void
get_run (gpointer data)
{
  GetJob *job = data;
  g_autoptr(GError) error = NULL;

  job->status = SOUP_STATUS_NOT_FOUND;
  job->handle = phodav_path_resolve (job->handler, job->path,
                                     PHODAV_PATH_EXISTING, &job->status);
  if (!job->handle)
    return;

  job->info = phodav_path_query_info (job->handle,
                                      "standard::*,etag::*,time::modified,unix::inode",
                                      G_FILE_QUERY_INFO_NONE,
                                      job->cancellable, &error);
  if (!job->info)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        job->error = g_steal_pointer (&error);
      job->status = SOUP_STATUS_NOT_FOUND;
      return;
    }

  if (g_file_info_get_file_type (job->info) == G_FILE_TYPE_DIRECTORY)
    {
      if (job->head)
        {
          job->status = SOUP_STATUS_OK;
          return;
        }

      job->e = phodav_path_enumerate (job->handle, "standard::*",
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      job->cancellable, &error);
      if (!job->e)
        {
          job->status = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
                          ? SOUP_STATUS_NOT_FOUND
                          : SOUP_STATUS_FORBIDDEN;
          return;
        }
    }

  job->status = SOUP_STATUS_OK;
}

static void
get_open_run (gpointer data)
{
  GetJob *job = data;
  g_autoptr(GError) error = NULL;

  job->file_path = g_file_get_path (phodav_path_get_file (job->handle));
  job->fd = -1;

#ifdef G_OS_UNIX
  if (phodav_path_get_fd (job->handle) >= 0)
    job->fd = fcntl (phodav_path_get_fd (job->handle), F_DUPFD_CLOEXEC, 0);
#endif

  if (job->fd >= 0)
    return;

  job->stream = phodav_path_open (job->handle, job->cancellable, &error);
  if (!job->stream)
    {
      job->status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
      job->error = g_steal_pointer (&error);
      return;
    }

  if (job->first > 0)
    {
      if (G_IS_SEEKABLE (job->stream))
        {
          if (!g_seekable_seek (G_SEEKABLE (job->stream), job->first, G_SEEK_SET,
                                job->cancellable, &error))
            {
              job->status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
              job->error = g_steal_pointer (&error);
              return;
            }
        }
      else
        {
          /* a non-seekable backend cannot jump to the start of the
           * range: consume the bytes before it */
          g_autofree guint8 *buffer = g_malloc (STREAM_CHUNK_SIZE);
          guint64 skip = job->first;

          while (skip > 0)
            {
              gsize want = MIN ((guint64) STREAM_CHUNK_SIZE, skip);
              gssize n = g_input_stream_read (G_INPUT_STREAM (job->stream),
                                              buffer, want,
                                              job->cancellable, &error);

              if (n <= 0)
                {
                  job->status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
                  if (error)
                    job->error = g_steal_pointer (&error);
                  return;
                }

              skip -= n;
            }
        }
    }
}

static void
get_done (gpointer data);

static void
get_open_done (gpointer data)
{
  GetJob *job = data;
  GetFilePull *pull;

  if (job->error)
    goto complete;

  pull = g_new0 (GetFilePull, 1);
  pull->stream = g_steal_pointer (&job->stream);
  pull->path = g_steal_pointer (&job->file_path);
  pull->fd = job->fd;
  job->fd = -1;
  pull->offset = job->first;
  pull->remaining = job->count;

  phodav_request_stream_start (job->req, job->status, NULL,
                               get_file_pull, pull, get_file_pull_free);

complete:
  phodav_request_complete (job->req, job->status,
                           g_steal_pointer (&job->error));
  get_job_free (job);
}

static void
get_done (gpointer data)
{
  GetJob *job = data;
  PhodavServer *server = handler_get_server (job->handler);

  if (job->status != SOUP_STATUS_OK)
    goto complete;

  if (g_file_info_get_file_type (job->info) == G_FILE_TYPE_DIRECTORY)
    {
      GetDirectoryPull *pull;

      method_get_directory_set_headers (job->req);

      if (job->head)
        {
          job->status = SOUP_STATUS_OK;
          goto complete;
        }

      pull = g_new0 (GetDirectoryPull, 1);
      pull->e = g_steal_pointer (&job->e);
      pull->buffer = get_directory_header (job->path);

      phodav_request_stream_start (job->req, SOUP_STATUS_OK,
                                   "text/html; charset=utf-8",
                                   get_directory_pull, pull,
                                   get_directory_pull_free);
      job->status = SOUP_STATUS_OK;
      goto complete;
    }

  {
    SoupMessageHeaders *response_headers = phodav_request_get_response_headers (job->req);
    g_autofree gchar *etag = phodav_etag_from_info (job->info);
    guint64 mtime = g_file_info_get_attribute_uint64 (job->info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    guint64 size = (guint64) g_file_info_get_size (job->info);
    guint64 first = 0, last = 0;
    const gchar *content_type;
    HttpRangeType range;

    method_get_add_validators (response_headers, etag, mtime);

    if (http_request_not_modified (phodav_request_get_request_headers (job->req), etag, mtime))
      {
        soup_message_headers_remove (response_headers, "Content-Type");
        job->status = SOUP_STATUS_NOT_MODIFIED;
        goto complete;
      }

    content_type = g_file_info_get_content_type (job->info);
    soup_message_headers_set_content_type (response_headers,
                                           content_type ? content_type : "application/octet-stream",
                                           NULL);
    soup_message_headers_append (response_headers, "Accept-Ranges", "bytes");

    if (job->head)
      {
        g_autofree gchar *length = g_strdup_printf ("%" G_GUINT64_FORMAT, size);

        soup_message_headers_append (response_headers, "Content-Length", length);
        job->status = SOUP_STATUS_OK;
        goto complete;
      }

    range = http_request_range (phodav_request_get_request_headers (job->req),
                                etag, mtime, size, &first, &last);
    if (range == HTTP_RANGE_UNSATISFIABLE)
      {
        g_autofree gchar *content_range = g_strdup_printf ("bytes */%" G_GUINT64_FORMAT, size);

        soup_message_headers_append (response_headers, "Content-Range", content_range);
        job->status = SOUP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE;
        goto complete;
      }

    if (range == HTTP_RANGE_PARTIAL)
      {
        g_autofree gchar *content_range =
          g_strdup_printf ("bytes %" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT,
                           first, last, size);

        soup_message_headers_append (response_headers, "Content-Range", content_range);
        job->status = SOUP_STATUS_PARTIAL_CONTENT;
        job->count = last - first + 1;
      }
    else
      {
        job->status = SOUP_STATUS_OK;
        first = 0;
        job->count = size;
      }

    job->first = first;

    /* an empty file has no body to stream */
    if (job->count == 0)
      goto complete;

    phodav_server_dispatch_stream (server, get_open_run, get_open_done, job);
    return;
  }

complete:
  phodav_request_complete (job->req, job->status,
                           g_steal_pointer (&job->error));
  get_job_free (job);
}

static void
get_start (gpointer data)
{
  GetJob *job = data;
  PhodavServer *server = handler_get_server (job->handler);

  if (phodav_request_is_finished (job->req))
    {
      phodav_request_complete (job->req, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
      get_job_free (job);
      return;
    }

  /* reads do not take the namespace: their filesystem work runs on
   * the parallel worker pool, beside the serialised mutations */
  phodav_server_dispatch_stream (server, get_run, get_done, job);
}

gint
phodav_method_get (PhodavRequest *req, GError **err)
{
  GetJob *job = g_new0 (GetJob, 1);

  (void) err;

  job->req = phodav_request_ref (req);
  job->handler = path_handler_ref (phodav_request_get_handler (req));
  job->path = g_strdup (phodav_request_get_path (req));
  job->head = g_strcmp0 (phodav_request_get_method (req), "HEAD") == 0;
  job->cancellable = g_object_ref (phodav_request_get_cancellable (req));
  job->fd = -1;

  get_start (job);

  return 0;
}
