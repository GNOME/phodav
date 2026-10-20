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

#include "phodav-lock.h"
#include "phodav-priv.h"
#include "phodav-utils.h"

typedef struct _PutContext
{
  PathHandler *handler; /* borrowed, owned by the server */
  GFile *file;
  GFileOutputStream *output;
} PutContext;

static PutContext *
put_context_new (PathHandler *handler, GFile *file, GFileOutputStream *output)
{
  PutContext *ctx = g_new0 (PutContext, 1);

  ctx->handler = handler;
  ctx->file = g_object_ref (file);
  ctx->output = g_object_ref (output);

  return ctx;
}

static void
put_context_free (PutContext *ctx)
{
  g_object_unref (ctx->file);
  g_object_unref (ctx->output);
  g_free (ctx);
}

/* the upload cannot be completed: the file content is incomplete.
 * The output stream of a g_file_replace() commits its content when
 * closed or finalized, so the partial file must be removed after
 * closing the stream. */
static void
put_context_abort (PutContext *ctx)
{
  g_autoptr(GError) close_error = NULL;
  g_autoptr(GError) delete_error = NULL;

  if (!g_output_stream_close (G_OUTPUT_STREAM (ctx->output), NULL, &close_error))
    g_debug ("PUT: failed to close the partial file: %s", close_error->message);

  if (!g_file_delete (ctx->file, NULL, &delete_error))
    g_debug ("PUT: failed to remove the partial file: %s", delete_error->message);
}

static void
method_put_finished (SoupServerMessage *msg,
                     gpointer user_data)
{
  PutContext *ctx = user_data;

  g_debug ("PUT finished %p", ctx->output);

  put_context_free (ctx);
}

static void
method_put_got_chunk (SoupServerMessage *msg,
                      GBytes *chunk,
                      gpointer user_data)
{
  PutContext *ctx = user_data;
  GCancellable *cancellable = handler_get_cancellable (ctx->handler);
  g_autoptr(GError) err = NULL;
  gsize bytes_written;
  gconstpointer data;
  gsize data_length;

  g_debug ("PUT got chunk");

  data = g_bytes_get_data (chunk, &data_length);

  if (g_output_stream_write_all (G_OUTPUT_STREAM (ctx->output),
                                 data, data_length,
                                 &bytes_written, cancellable, &err))
    return;

  g_warning ("PUT: write error: %s", err->message);

  /* answer right away, ignore the remaining chunks and discard the
   * partial file, the client has to retry the upload from scratch */
  soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
  g_signal_handlers_disconnect_by_func (msg, method_put_got_chunk, ctx);
  put_context_abort (ctx);
}

/* does an If-Match / If-None-Match header value (RFC 9110 section 13.1)
 * match the current @etag of the resource, which exists per @exists? */
static gboolean
etag_header_matches (const gchar *header, const gchar *etag, gboolean exists)
{
  g_auto(GStrv) etags = NULL;

  if (!g_strcmp0 (header, "*"))
    return exists;

  if (!etag)
    return FALSE;

  /* a comma separated list of quoted entity tags; the comparison is
   * weak, so the weakness markers are ignored. (GIO etags do not
   * contain commas, splitting on them is unambiguous.) */
  etags = g_strsplit (header, ",", -1);
  for (guint i = 0; etags[i]; i++)
    {
      const gchar *h = g_strstrip (etags[i]);
      gsize len = strlen (h);

      if (g_str_has_prefix (h, "W/"))
        {
          h += 2;
          len -= 2;
        }

      if (len >= 2 && h[0] == '"' && h[len - 1] == '"')
        {
          h++;
          len -= 2;
        }

      if (strlen (etag) == len && strncmp (etag, h, len) == 0)
        return TRUE;
    }

  return FALSE;
}

static gint
put_start (SoupServerMessage *msg, GFile *file, GFileOutputStream **output, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = NULL;
  GFileOutputStream *s = NULL;
  gboolean exists = FALSE;
  const gchar *etag = NULL;
  const gchar *if_match, *if_none_match;
  SoupMessageHeaders *headers = soup_server_message_get_request_headers (msg);
  gint status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

  info = g_file_query_info (file, "etag::*",
                            G_FILE_QUERY_INFO_NONE, cancellable, &error);
  if (info)
    {
      exists = TRUE;
      etag = g_file_info_get_etag (info);
    }
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_clear_error (&error);
  else
    {
      g_propagate_error (err, g_steal_pointer (&error));
      goto end;
    }

  /* conditional requests, RFC 9110 section 13.1: a failing
   * precondition must be answered with 412, not by overwriting */
  if_match = soup_message_headers_get_one (headers, "If-Match");
  if_none_match = soup_message_headers_get_one (headers, "If-None-Match");

  if (if_match && !etag_header_matches (if_match, etag, exists))
    {
      status = SOUP_STATUS_PRECONDITION_FAILED;
      goto end;
    }

  if (if_none_match && etag_header_matches (if_none_match, etag, exists))
    {
      status = SOUP_STATUS_PRECONDITION_FAILED;
      goto end;
    }

  s = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_PRIVATE, cancellable, &error);
  if (!s)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          /* missing intermediate collection, see RFC 4918 section 9.7.1 */
          g_clear_error (&error);
          status = SOUP_STATUS_CONFLICT;
        }
      else
        g_propagate_error (err, g_steal_pointer (&error));

      goto end;
    }

  status = exists ? SOUP_STATUS_OK : SOUP_STATUS_CREATED;

end:
  *output = s;
  return status;
}

void
phodav_method_put (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  GFile *file = NULL;
  g_autolist(LockSubmitted) submitted = NULL;
  GFileOutputStream *output = NULL;
  gint status;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);

  g_debug ("%s %s HTTP/1.%d %s %s", soup_server_message_get_method (msg), path, soup_server_message_get_http_version (msg),
           soup_message_headers_get_one (request_headers, "X-Litmus") ?: "",
           soup_message_headers_get_one (request_headers, "X-Litmus-Second") ?: "");

  if (handler_get_readonly (handler))
    {
      status = SOUP_STATUS_FORBIDDEN;
      goto end;
    }

  status = phodav_check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  status = put_start (msg, file, &output, cancellable, err);
  if (!output || *err)
    goto end;

  g_debug ("PUT output %p", output);
  soup_message_body_set_accumulate (soup_server_message_get_request_body (msg), FALSE);
  {
    PutContext *ctx = put_context_new (handler, file, output);

    g_signal_connect (msg, "got-chunk", G_CALLBACK (method_put_got_chunk), ctx);
    g_signal_connect (msg, "finished", G_CALLBACK (method_put_finished), ctx);
    g_object_unref (output); /* the context owns it now */
  }

end:
  soup_server_message_set_status (msg, status, NULL);
  g_clear_object (&file);
  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
}
