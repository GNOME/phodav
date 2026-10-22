/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
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
  guint64 written; /* bytes streamed to the file so far */
  gint status;     /* the success status, answered once the file is committed */
  gboolean done;   /* the outcome is decided: the file is committed or discarded */
} PutContext;

static PutContext *
put_context_new (PathHandler *handler, GFile *file, GFileOutputStream *output, gint status)
{
  PutContext *ctx = g_new0 (PutContext, 1);

  ctx->handler = handler;
  ctx->file = g_object_ref (file);
  ctx->output = g_object_ref (output);
  ctx->status = status;

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
 * closing the stream, rather than left behind: truncated, or empty
 * when the body never made it. */
static void
put_context_discard (PutContext *ctx)
{
  g_autoptr(GError) close_error = NULL;
  g_autoptr(GError) delete_error = NULL;

  if (ctx->done)
    return;
  ctx->done = TRUE;

  if (!g_output_stream_close (G_OUTPUT_STREAM (ctx->output), NULL, &close_error))
    g_debug ("PUT: failed to close the partial file: %s", close_error->message);

  if (!g_file_delete (ctx->file, NULL, &delete_error) &&
      !g_error_matches (delete_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_debug ("PUT: failed to remove the partial file: %s", delete_error->message);
}

static void
method_put_finished (SoupServerMessage *msg,
                     gpointer user_data)
{
  PutContext *ctx = user_data;

  g_debug ("PUT finished %p", ctx->output);

  /* the body was never fully received (the client hung up, or the
   * connection was interrupted): discard the partial file, rather
   * than committing it by finalizing the stream */
  put_context_discard (ctx);

  put_context_free (ctx);
}

static void
method_put_got_chunk (SoupServerMessage *msg,
                      GBytes *chunk,
                      gpointer user_data)
{
  PutContext *ctx = user_data;
  GCancellable *cancellable = handler_get_cancellable (ctx->handler);
  guint64 max_upload = handler_get_max_upload (ctx->handler);
  g_autoptr(GError) err = NULL;
  gsize bytes_written;
  gconstpointer data;
  gsize data_length;

  g_debug ("PUT got chunk");

  data = g_bytes_get_data (chunk, &data_length);

  if (!g_output_stream_write_all (G_OUTPUT_STREAM (ctx->output),
                                  data, data_length,
                                  &bytes_written, cancellable, &err))
    {
      g_warning ("PUT: write error: %s", err->message);

      /* answer right away, ignore the remaining chunks and discard the
       * partial file, the client has to retry the upload from scratch */
      soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
      g_signal_handlers_disconnect_by_func (msg, method_put_got_chunk, ctx);
      put_context_discard (ctx);

      return;
    }

  ctx->written += data_length;

  /* a chunked body has no Content-Length to check in advance: stop
   * at the limit and discard the partial file, like a write error */
  if (max_upload > 0 && ctx->written > max_upload)
    {
      g_debug ("PUT: upload over the %" G_GUINT64_FORMAT " bytes limit", max_upload);

      soup_server_message_set_status (msg, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, NULL);
      g_signal_handlers_disconnect_by_func (msg, method_put_got_chunk, ctx);
      put_context_discard (ctx);
    }
}

/* the whole body has been received and written to the file: close
 * the output stream, which commits it to its final place, and answer
 * the success status. This happens before the response is written,
 * so that a client which sees a success knows its upload is durable;
 * a failure to commit the file is answered with 500, and the partial
 * file is discarded. */
static void
method_put_got_body (SoupServerMessage *msg,
                     gpointer user_data)
{
  PutContext *ctx = user_data;
  GCancellable *cancellable = handler_get_cancellable (ctx->handler);
  g_autoptr(GError) error = NULL;

  /* the chunks are all written by now */
  g_signal_handlers_disconnect_by_func (msg, method_put_got_chunk, ctx);

  /* the upload was already discarded by a got-chunk handler: keep
   * the error status it set */
  if (ctx->done)
    return;

  if (!g_output_stream_close (G_OUTPUT_STREAM (ctx->output), cancellable, &error))
    {
      g_warning ("PUT: failed to commit the uploaded file: %s", error->message);

      put_context_discard (ctx);
      soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
    }
  else
    {
      ctx->done = TRUE;
      soup_server_message_set_status (msg, ctx->status, NULL);
    }

  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
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
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  guint64 max_upload = handler_get_max_upload (handler);
  gint status;

  g_debug ("%s %s HTTP/1.%d %s %s", soup_server_message_get_method (msg), path, soup_server_message_get_http_version (msg),
           soup_message_headers_get_one (request_headers, "X-Litmus") ?: "",
           soup_message_headers_get_one (request_headers, "X-Litmus-Second") ?: "");

  /* the request body is streamed to the file, or drained without
   * being accumulated when the request is refused below and the
   * client is still sending it */
  soup_message_body_set_accumulate (soup_server_message_get_request_body (msg), FALSE);

  if (handler_get_readonly (handler))
    {
      status = SOUP_STATUS_FORBIDDEN;
      goto end;
    }

  /* an upload which is over the limit is refused before anything is
   * written to disk, RFC 9110 section 15.5.15; a chunked body has
   * no Content-Length and is cut off while it is streamed */
  if (max_upload > 0 && (guint64) soup_message_headers_get_content_length (request_headers) > max_upload)
    {
      status = SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE;
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
  {
    PutContext *ctx = put_context_new (handler, file, output, status);

    /* the request is answered from method_put_got_body(), once its
     * whole body has been received and the file committed: answering
     * here would tell a "Expect: 100-continue" client its upload
     * succeeded before it even sent the body */
    g_signal_connect (msg, "got-chunk", G_CALLBACK (method_put_got_chunk), ctx);
    g_signal_connect (msg, "got-body", G_CALLBACK (method_put_got_body), ctx);
    g_signal_connect (msg, "finished", G_CALLBACK (method_put_finished), ctx);
    g_object_unref (output); /* the context owns it now */
  }

  g_clear_object (&file);
  return;

end:
  /* the request is refused: answer right away, the client may still
   * be sending its body, which is drained without being accumulated */
  soup_server_message_set_status (msg, status, NULL);
  g_clear_object (&file);
  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
}
