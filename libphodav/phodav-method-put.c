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
  GFile *file;          /* the destination of the upload */
  GFile *temp_file;     /* the staged upload, renamed over the destination on success */
  GFileOutputStream *output;
  gchar *if_match; /* the conditional headers, rechecked before the rename */
  gchar *if_none_match;
  guint64 written; /* bytes streamed to the file so far */
  gint status;     /* the success status, answered once the file is committed */
  gboolean closed; /* the output stream was closed */
  gboolean done;   /* the outcome is decided: the file is committed or discarded */
} PutContext;

static PutContext *
put_context_new (PathHandler *handler, GFile *file, GFile *temp_file, GFileOutputStream *output, gint status, const gchar *if_match, const gchar *if_none_match)
{
  PutContext *ctx = g_new0 (PutContext, 1);

  ctx->handler = handler;
  ctx->file = g_object_ref (file);
  ctx->temp_file = g_object_ref (temp_file);
  ctx->output = g_object_ref (output);
  ctx->if_match = g_strdup (if_match);
  ctx->if_none_match = g_strdup (if_none_match);
  ctx->status = status;

  return ctx;
}

static void
put_context_free (PutContext *ctx)
{
  g_clear_object (&ctx->file);
  g_clear_object (&ctx->temp_file);
  g_clear_object (&ctx->output);
  g_free (ctx->if_match);
  g_free (ctx->if_none_match);
  g_free (ctx);
}

/* the preconditions of a PUT, RFC 9110 section 13.1, evaluated
 * against the current state of the destination */
static gboolean
put_preconditions_hold (const gchar *if_match, const gchar *if_none_match, const gchar *etag, gboolean exists)
{
  if (if_match && !etag_header_matches (if_match, etag, exists))
    return FALSE;

  if (if_none_match && etag_header_matches (if_none_match, etag, exists))
    return FALSE;

  return TRUE;
}

/* the upload cannot be completed: close the staged file and remove it.
 * The destination is never touched until the whole body has been
 * received and committed by put_commit(). */
static void
put_context_discard (PutContext *ctx)
{
  g_autoptr(GError) close_error = NULL;
  g_autoptr(GError) delete_error = NULL;

  if (ctx->done)
    return;
  ctx->done = TRUE;

  if (!ctx->closed)
    {
      ctx->closed = TRUE;
      if (!g_output_stream_close (G_OUTPUT_STREAM (ctx->output), NULL, &close_error))
        g_debug ("PUT: failed to close the staged file: %s", close_error->message);
    }

  if (ctx->temp_file &&
      !g_file_delete (ctx->temp_file, NULL, &delete_error) &&
      !g_error_matches (delete_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_debug ("PUT: failed to remove the staged file: %s", delete_error->message);

  g_clear_object (&ctx->temp_file);
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
      g_debug ("PUT: write error: %s", err->message);
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

static gboolean put_context_close (PutContext *ctx, GCancellable *cancellable, GError **error);
static gint put_commit (PutContext *ctx, GCancellable *cancellable, GError **err);

/* the whole body has been received and written to the staged file:
 * close it, rename it over the destination, and answer the success
 * status. This happens before the response is written, so that a
 * client which sees a success knows its upload is durable; a failure
 * to commit the file is answered with the matching error status, and
 * the staged file is discarded. */
static void
method_put_got_body (SoupServerMessage *msg,
                     gpointer user_data)
{
  PutContext *ctx = user_data;
  GCancellable *cancellable = handler_get_cancellable (ctx->handler);
  g_autoptr(GError) error = NULL;
  gint status;

  /* the chunks are all written by now */
  g_signal_handlers_disconnect_by_func (msg, method_put_got_chunk, ctx);

  /* the upload was already discarded by a got-chunk handler: keep
   * the error status it set */
  if (ctx->done)
    return;

  if (!put_context_close (ctx, cancellable, &error))
    {
      g_debug ("PUT: failed to close the staged file: %s", error->message);

      put_context_discard (ctx);
      soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
      goto end;
    }

  status = put_commit (ctx, cancellable, &error);
  if (status != SOUP_STATUS_OK)
    {
      g_debug ("PUT: failed to commit the uploaded file: %s",
               error ? error->message : "precondition failed");

      put_context_discard (ctx);
      soup_server_message_set_status (msg, status, NULL);
      goto end;
    }

  ctx->done = TRUE;
  soup_server_message_set_status (msg, ctx->status, NULL);

end:
  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
}

/* close the staged file, once */
static gboolean
put_context_close (PutContext *ctx, GCancellable *cancellable, GError **error)
{
  if (ctx->closed)
    return TRUE;

  ctx->closed = TRUE;
  return g_output_stream_close (G_OUTPUT_STREAM (ctx->output), cancellable, error);
}

/* query the current destination; a missing destination is not an
 * error, and returns NULL with @error unset */
static GFileInfo *
put_query_destination (GFile *file, GCancellable *cancellable, GError **error)
{
  g_autoptr(GError) local_error = NULL;
  GFileInfo *info;

  info = g_file_query_info (file, "standard::type,etag::*",
                            G_FILE_QUERY_INFO_NONE, cancellable, &local_error);
  if (info)
    return info;

  if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_propagate_error (error, g_steal_pointer (&local_error));

  return NULL;
}

/* commit the staged file: re-evaluate the preconditions against the
 * destination as it is now, then atomically rename the staged file
 * over it. Nothing is committed when the conditions no longer hold. */
static gint
put_commit (PutContext *ctx, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = NULL;
  const gchar *etag = NULL;
  gboolean exists = FALSE;

  info = put_query_destination (ctx->file, cancellable, &error);
  if (error)
    {
      g_propagate_error (err, g_steal_pointer (&error));
      return SOUP_STATUS_INTERNAL_SERVER_ERROR;
    }

  if (info)
    {
      exists = TRUE;
      etag = g_file_info_get_etag (info);

      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        return SOUP_STATUS_METHOD_NOT_ALLOWED;
    }

  if (!put_preconditions_hold (ctx->if_match, ctx->if_none_match, etag, exists))
    return SOUP_STATUS_PRECONDITION_FAILED;

  if (!g_file_move (ctx->temp_file, ctx->file, G_FILE_COPY_OVERWRITE,
                    cancellable, NULL, NULL, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return SOUP_STATUS_CONFLICT;

      g_propagate_error (err, g_steal_pointer (&error));
      return SOUP_STATUS_INTERNAL_SERVER_ERROR;
    }

  /* the rename consumed the staged file */
  g_clear_object (&ctx->temp_file);

  return SOUP_STATUS_OK;
}

/* open a uniquely named temporary file next to the destination, so
 * that the final rename stays on the same filesystem */
static gboolean
put_temp_open (GFile *file, GFile **temp_file, GFileOutputStream **output, GCancellable *cancellable, GError **err)
{
  g_autoptr(GFile) parent = g_file_get_parent (file);
  guint i;

  *temp_file = NULL;
  *output = NULL;

  if (!parent)
    {
      g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "The upload has no parent directory");
      return FALSE;
    }

  for (i = 0; i < 8; i++)
    {
      g_autofree gchar *uuid = g_uuid_string_random ();
      g_autofree gchar *name = g_strdup_printf (".phodav-%s.tmp", uuid);
      g_autoptr(GFile) temp = g_file_get_child (parent, name);
      g_autoptr(GError) error = NULL;
      GFileOutputStream *s;

      s = g_file_create (temp, G_FILE_CREATE_PRIVATE, cancellable, &error);
      if (s)
        {
          *temp_file = g_steal_pointer (&temp);
          *output = s;
          return TRUE;
        }

      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          g_propagate_error (err, g_steal_pointer (&error));
          return FALSE;
        }
    }

  g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_EXISTS,
                       "Cannot find a free name for the upload");
  return FALSE;
}

static gint
put_start (SoupServerMessage *msg, PathHandler *handler, GFile *file, PutContext **out, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GFile) temp_file = NULL;
  GFileOutputStream *output = NULL;
  gboolean exists = FALSE;
  const gchar *etag = NULL;
  const gchar *if_match, *if_none_match;
  SoupMessageHeaders *headers = soup_server_message_get_request_headers (msg);
  gint status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

  *out = NULL;

  info = put_query_destination (file, cancellable, &error);
  if (error)
    {
      g_propagate_error (err, g_steal_pointer (&error));
      goto end;
    }

  if (info)
    {
      exists = TRUE;
      etag = g_file_info_get_etag (info);

      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        {
          /* RFC 4918 section 9.7.2: PUT cannot replace a collection */
          status = SOUP_STATUS_METHOD_NOT_ALLOWED;
          goto end;
        }
    }

  /* conditional requests, RFC 9110 section 13.1: a failing
   * precondition must be answered with 412, not by overwriting */
  if_match = soup_message_headers_get_one (headers, "If-Match");
  if_none_match = soup_message_headers_get_one (headers, "If-None-Match");

  if (!put_preconditions_hold (if_match, if_none_match, etag, exists))
    {
      status = SOUP_STATUS_PRECONDITION_FAILED;
      goto end;
    }

  if (!put_temp_open (file, &temp_file, &output, cancellable, &error))
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
  *out = put_context_new (handler, file, temp_file, output, status,
                          if_match, if_none_match);

end:
  g_clear_object (&output);
  return status;
}

void
phodav_method_put (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  GFile *file = NULL;
  g_autolist(LockSubmitted) submitted = NULL;
  PutContext *ctx = NULL;
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

  /* the read-only refusal is enforced by the method dispatch, in
   * got_headers(), before the upload is started */

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

  file = handler_get_file_for_path (handler, path);
  status = put_start (msg, handler, file, &ctx, cancellable, err);
  if (!ctx)
    goto end;

  g_debug ("PUT temp %p", ctx->output);

  /* the request is answered from method_put_got_body(), once its
   * whole body has been received and the staged file committed:
   * answering here would tell a "Expect: 100-continue" client its
   * upload succeeded before it even sent the body */
  g_signal_connect (msg, "got-chunk", G_CALLBACK (method_put_got_chunk), ctx);
  g_signal_connect (msg, "got-body", G_CALLBACK (method_put_got_body), ctx);
  g_signal_connect (msg, "finished", G_CALLBACK (method_put_finished), ctx);

  g_clear_object (&file);
  return;

end:
  /* the request is refused: answer right away, the client may still
   * be sending its body, which is drained without being accumulated */
  soup_server_message_set_status (msg, status, NULL);
  g_clear_object (&file);
  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
}
