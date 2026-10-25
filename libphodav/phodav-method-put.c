/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-priv.h"
#include "phodav-utils.h"

/* the upload phases:
 *
 * RECEIVING -> DONE        the staged file was committed
 * RECEIVING -> DISCARDING  the request failed; cleanup waits for writes
 * DISCARDING -> DISCARDED  no write is in flight, the staged file is gone
 *
 * The phase is never left once it is decided. */
typedef enum
{
  PUT_PHASE_RECEIVING,
  PUT_PHASE_DONE,
  PUT_PHASE_DISCARDING,
  PUT_PHASE_DISCARDED,
} PutPhase;

typedef struct _PutContext
{
  gint ref_count;
  SoupServerMessage *msg;
  GCancellable *cancellable;
  guint64 max_upload;
  GFile *file;      /* the destination of the upload */
  GFile *temp_file; /* the staged upload, renamed over the destination on success */
  GFileOutputStream *output;
  gchar *if_match; /* the conditional headers, rechecked before the rename */
  gchar *if_none_match;
  GQueue *chunks;     /* the received chunks waiting for their turn on disk */
  GBytes *chunk;      /* the chunk being written */
  guint64 written;    /* bytes streamed to the file so far */
  gint status;        /* the success status, answered once the file is committed */
  gboolean closed;    /* the output stream was closed */
  PutPhase phase;     /* the upload outcome, see above */
  gboolean writing;   /* an asynchronous write is in flight */
  gboolean body_done; /* the whole request body was received */
  gboolean paused;    /* the message I/O is paused while a write is in flight */
  gboolean finished;  /* the message is done, it must not be touched anymore */
} PutContext;

static void put_write_next (PutContext *ctx);
static void put_chunk_written_cb (GObject *source_object,
                                  GAsyncResult *res,
                                  gpointer user_data);
static void put_finish (PutContext *ctx);
static void put_context_abort (PutContext *ctx);
static void method_put_got_chunk (SoupServerMessage *msg,
                                  GBytes *chunk,
                                  gpointer user_data);
static gboolean put_context_close (PutContext *ctx,
                                   GCancellable *cancellable,
                                   GError **error);
static gint put_commit (PutContext *ctx,
                        GCancellable *cancellable,
                        GError **err);

static PutContext *
put_context_ref (PutContext *ctx)
{
  g_atomic_int_inc (&ctx->ref_count);

  return ctx;
}

/* remove the staged file, once no write is in flight anymore */
static void
put_context_cleanup_files (PutContext *ctx)
{
  g_autoptr(GError) close_error = NULL;
  g_autoptr(GError) delete_error = NULL;

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

static gboolean
put_context_is_decided (const PutContext *ctx)
{
  return ctx->phase != PUT_PHASE_RECEIVING;
}

/* the staged file is removed once no write is in flight anymore:
 * closing a GFileOutputStream while an asynchronous write is pending
 * races with that write */
static void
put_context_discard_if_idle (PutContext *ctx)
{
  if (ctx->phase == PUT_PHASE_DISCARDING && !ctx->writing)
    {
      put_context_cleanup_files (ctx);
      ctx->phase = PUT_PHASE_DISCARDED;
    }
}

static void
put_context_free (PutContext *ctx)
{
  /* the outcome was not decided by put_finish() or put_context_abort():
   * never leave the staged file behind */
  if (ctx->temp_file)
    put_context_cleanup_files (ctx);

  g_clear_object (&ctx->msg);
  g_clear_object (&ctx->cancellable);
  g_clear_object (&ctx->file);
  g_clear_object (&ctx->output);
  g_clear_pointer (&ctx->chunk, g_bytes_unref);
  if (ctx->chunks)
    {
      g_queue_free_full (ctx->chunks, (GDestroyNotify) g_bytes_unref);
      ctx->chunks = NULL;
    }
  g_free (ctx->if_match);
  g_free (ctx->if_none_match);
  g_free (ctx);
}

static void
put_context_unref (PutContext *ctx)
{
  if (g_atomic_int_dec_and_test (&ctx->ref_count))
    put_context_free (ctx);
}

static PutContext *
put_context_new (PathHandler *handler,
                 SoupServerMessage *msg,
                 GFile *file,
                 GFile *temp_file,
                 GFileOutputStream *output,
                 gint status,
                 const gchar *if_match,
                 const gchar *if_none_match,
                 GCancellable *cancellable)
{
  PutContext *ctx = g_new0 (PutContext, 1);

  ctx->ref_count = 1;
  ctx->msg = g_object_ref (msg);
  /* the handler is owned by the SoupServer registration, and may be
   * freed before the asynchronous write completes: keep what the
   * callbacks need */
  ctx->cancellable = g_object_ref (cancellable);
  ctx->max_upload = handler_get_max_upload (handler);
  ctx->file = g_object_ref (file);
  ctx->temp_file = g_object_ref (temp_file);
  ctx->output = g_object_ref (output);
  ctx->chunks = g_queue_new ();
  ctx->if_match = g_strdup (if_match);
  ctx->if_none_match = g_strdup (if_none_match);
  ctx->status = status;

  return ctx;
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

/* decide that the upload cannot be completed, and discard it. The
 * staged file is only closed and removed once the write in flight, if
 * any, has completed: closing a GFileOutputStream while an
 * asynchronous write is pending races with that write. */
static void
put_context_abort (PutContext *ctx)
{
  if (put_context_is_decided (ctx))
    {
      put_context_discard_if_idle (ctx);
      return;
    }

  ctx->phase = PUT_PHASE_DISCARDING;
  g_signal_handlers_disconnect_by_func (ctx->msg, method_put_got_chunk, ctx);

  put_context_discard_if_idle (ctx);
}

static void
method_put_finished (SoupServerMessage *msg,
                     gpointer user_data)
{
  PutContext *ctx = user_data;

  g_debug ("PUT finished %p", ctx->output);

  ctx->finished = TRUE;

  /* the body was never fully received (the client hung up, or the
   * connection was interrupted): discard the partial file, rather
   * than committing it by finalizing the stream */
  put_context_abort (ctx);

  put_context_unref (ctx);
}

/* resume the request I/O after a chunk was written, unless the
 * message is already gone */
static void
put_resume (PutContext *ctx)
{
  if (!ctx->paused || ctx->finished)
    return;

  ctx->paused = FALSE;
  soup_server_message_unpause (ctx->msg);
}

/* write the next received chunk, keeping the message paused while the
 * write is in flight: the request body is not read further until the
 * previous chunk reached the file, so the upload cannot queue an
 * unbounded amount of data in memory */
static void
put_write_next (PutContext *ctx)
{
  gconstpointer data;
  gsize data_length;

  if (put_context_is_decided (ctx) || ctx->writing)
    return;

  if (!ctx->chunk)
    ctx->chunk = g_queue_pop_head (ctx->chunks);

  if (!ctx->chunk)
    return;

  if (!ctx->paused)
    {
      soup_server_message_pause (ctx->msg);
      ctx->paused = TRUE;
    }

  data = g_bytes_get_data (ctx->chunk, &data_length);
  ctx->writing = TRUE;

  g_output_stream_write_all_async (G_OUTPUT_STREAM (ctx->output),
                                   data, data_length,
                                   G_PRIORITY_DEFAULT, ctx->cancellable,
                                   put_chunk_written_cb,
                                   put_context_ref (ctx));
}

static void
put_chunk_written_cb (GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
{
  PutContext *ctx = user_data;
  g_autoptr(GError) error = NULL;
  gsize written = 0;
  gboolean ok;

  ok = g_output_stream_write_all_finish (G_OUTPUT_STREAM (source_object),
                                         res, &written, &error);

  g_clear_pointer (&ctx->chunk, g_bytes_unref);
  ctx->writing = FALSE;

  if (!ok)
    {
      if (!ctx->finished)
        {
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_debug ("PUT: write error: %s", error->message);
          soup_server_message_set_status (ctx->msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
        }

      put_context_abort (ctx);
    }
  else
    {
      ctx->written += written;

      /* a chunked body has no Content-Length to check in advance: stop
       * at the limit and discard the staged file, like a write error */
      if (ctx->max_upload > 0 && ctx->written > ctx->max_upload)
        {
          g_debug ("PUT: upload over the %" G_GUINT64_FORMAT " bytes limit", ctx->max_upload);

          if (!ctx->finished)
            soup_server_message_set_status (ctx->msg, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, NULL);
          put_context_abort (ctx);
        }
    }

  /* the request was refused: disconnect the chunk handler already, and
   * drain what the client still sends without staging it */
  if (put_context_is_decided (ctx))
    {
      put_resume (ctx);
      put_context_discard_if_idle (ctx);
      put_context_unref (ctx);
      return;
    }

  /* another chunk was received before the write completed: write it
   * before resuming the message I/O, so that at most one write is in
   * flight and the chunks keep their order */
  if (ctx->chunks->length > 0)
    {
      put_write_next (ctx);
      put_context_unref (ctx);
      return;
    }

  if (ctx->body_done)
    {
      put_finish (ctx);
      put_resume (ctx);
      put_context_unref (ctx);
      return;
    }

  put_resume (ctx);
  put_context_unref (ctx);
}

static void
method_put_got_chunk (SoupServerMessage *msg,
                      GBytes *chunk,
                      gpointer user_data)
{
  PutContext *ctx = user_data;
  gsize data_length;

  if (put_context_is_decided (ctx) || ctx->finished)
    return;

  data_length = g_bytes_get_size (chunk);
  if (data_length == 0)
    return;

  g_queue_push_tail (ctx->chunks, g_bytes_ref (chunk));

  if (!ctx->writing)
    put_write_next (ctx);
}

/* the whole body has been received and written to the staged file:
 * close it, rename it over the destination, and answer the success
 * status. This happens before the response is written, so that a
 * client which sees a success knows its upload is durable; a failure
 * to commit the file is answered with the matching error status, and
 * the staged file is discarded. */
static void
put_finish (PutContext *ctx)
{
  g_autoptr(GError) error = NULL;
  gint status;

  if (put_context_is_decided (ctx) || ctx->writing || ctx->chunks->length > 0)
    return;

  g_signal_handlers_disconnect_by_func (ctx->msg, method_put_got_chunk, ctx);

  if (!put_context_close (ctx, ctx->cancellable, &error))
    {
      g_debug ("PUT: failed to close the staged file: %s", error->message);

      put_context_abort (ctx);
      soup_server_message_set_status (ctx->msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
      goto end;
    }

  status = put_commit (ctx, ctx->cancellable, &error);
  if (status != SOUP_STATUS_OK)
    {
      g_debug ("PUT: failed to commit the uploaded file: %s",
               error ? error->message : "precondition failed");

      put_context_abort (ctx);
      soup_server_message_set_status (ctx->msg, status, NULL);
      goto end;
    }

  ctx->phase = PUT_PHASE_DONE;
  soup_server_message_set_status (ctx->msg, ctx->status, NULL);

end:
  g_debug ("  -> %d %s\n", soup_server_message_get_status (ctx->msg), soup_server_message_get_reason_phrase (ctx->msg));
}

static void
method_put_got_body (SoupServerMessage *msg,
                     gpointer user_data)
{
  PutContext *ctx = user_data;

  /* the chunks are all received, but their writes may still be in
   * flight: put_finish() is called once the last one completed */
  ctx->body_done = TRUE;
  g_signal_handlers_disconnect_by_func (msg, method_put_got_chunk, ctx);

  /* the upload was already discarded by a got-chunk handler: keep
   * the error status it set */
  if (put_context_is_decided (ctx))
    return;

  if (ctx->writing || ctx->chunks->length > 0)
    return;

  put_finish (ctx);
  put_resume (ctx);
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

  info = g_file_query_info (file, "standard::type,standard::size,etag::*,unix::inode",
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
  g_autofree gchar *etag = NULL;
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
      etag = phodav_etag_from_info (info);

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
put_start (PhodavRequest *req, GFile *file, PutContext **out, GCancellable *cancellable, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  SoupServerMessage *msg = phodav_request_get_message (req);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GFile) temp_file = NULL;
  GFileOutputStream *output = NULL;
  gboolean exists = FALSE;
  g_autofree gchar *etag = NULL;
  const gchar *if_match, *if_none_match;
  SoupMessageHeaders *headers = phodav_request_get_request_headers (req);
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
      etag = phodav_etag_from_info (info);

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
  *out = put_context_new (handler, msg, file, temp_file, output, status,
                          if_match, if_none_match, cancellable);

end:
  g_clear_object (&output);
  return status;
}

/* the lock check and the staged file creation run on the serialized
 * pool, so that no LOCK/UNLOCK/MOVE can slip between them and PUT does
 * not block the main loop while another serialized method runs */
typedef struct _PutStart
{
  PhodavRequest *req;
  PutContext *ctx;
  gint status;
  GError *error;
} PutStart;

static void
put_start_free (PutStart *start)
{
  if (start->ctx)
    put_context_unref (start->ctx);
  g_clear_pointer (&start->req, phodav_request_unref);
  g_clear_error (&start->error);
  g_free (start);
}

static void
put_start_task (GTask *task,
                gpointer source_object,
                gpointer task_data,
                GCancellable *cancellable)
{
  PutStart *start = task_data;
  PhodavRequest *req = start->req;
  PathHandler *handler = phodav_request_get_handler (req);
  PhodavServer *server = handler_get_server (handler);
  const gchar *path = phodav_request_get_path (req);
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  guint64 max_upload = handler_get_max_upload (handler);
  g_autolist(LockSubmitted) submitted = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GError) error = NULL;

  g_mutex_lock (&server->mutation_mutex);

  if (g_cancellable_is_cancelled (cancellable))
    goto out;

  /* an upload which is over the limit is refused before anything is
   * written to disk, RFC 9110 section 15.5.15; a chunked body has
   * no Content-Length and is cut off while it is streamed */
  if (max_upload > 0 &&
      (guint64) soup_message_headers_get_content_length (request_headers) > max_upload)
    {
      start->status = SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE;
      goto out;
    }

  start->status = phodav_check_if (req, &submitted);
  if (start->status != SOUP_STATUS_OK)
    goto out;

  file = handler_get_file_for_path (handler, path);
  start->status = put_start (req, file, &start->ctx, cancellable, &error);
  if (!start->ctx)
    start->error = g_steal_pointer (&error);

out:
  g_mutex_unlock (&server->mutation_mutex);
  g_task_return_boolean (task, TRUE);
}

static void
put_start_done (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  PutStart *start = user_data;
  PhodavRequest *req = start->req;
  SoupServerMessage *msg = phodav_request_get_message (req);
  gboolean finished = phodav_request_is_finished (req);
  gboolean cancelled = g_cancellable_is_cancelled (phodav_request_get_cancellable (req));
  g_autoptr(GError) task_error = NULL;
  gint status = start->status;

  g_task_propagate_boolean (G_TASK (res), &task_error);

  if (task_error)
    g_debug ("PUT start failed: %s", task_error->message);
  if (start->error)
    g_debug ("%s %s: %s", phodav_request_get_method (req),
             phodav_request_get_path (req), start->error->message);

  if (finished)
    {
      /* the client is gone, do not touch its message */
    }
  else if (cancelled || task_error || !start->ctx)
    {
      if (cancelled)
        status = phodav_request_is_server_cancelled (req)
                   ? SOUP_STATUS_SERVICE_UNAVAILABLE
                   : SOUP_STATUS_INTERNAL_SERVER_ERROR;
      else if (status == 0)
        status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

      soup_server_message_set_status (msg, status, NULL);
      soup_server_message_unpause (msg);
    }
  else
    {
      PutContext *ctx = start->ctx;

      start->ctx = NULL;
      g_debug ("PUT temp %p", ctx->output);

      /* the request is answered from method_put_got_body(), once its
       * whole body has been received and the staged file committed:
       * answering here would tell a "Expect: 100-continue" client its
       * upload succeeded before it even sent the body */
      g_signal_connect (msg, "got-chunk", G_CALLBACK (method_put_got_chunk), ctx);
      g_signal_connect (msg, "got-body", G_CALLBACK (method_put_got_body), ctx);
      g_signal_connect (msg, "finished", G_CALLBACK (method_put_finished), ctx);

      soup_server_message_unpause (msg);
    }

  put_start_free (start);
}

void
phodav_method_put (PhodavRequest *req, GError **err)
{
  SoupServerMessage *msg = phodav_request_get_message (req);
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  PutStart *start = g_new0 (PutStart, 1);
  GTask *task;

  (void) err;

  g_debug ("%s %s HTTP/1.%d %s %s",
           phodav_request_get_method (req), phodav_request_get_path (req),
           soup_server_message_get_http_version (msg),
           soup_message_headers_get_one (request_headers, "X-Litmus") ?: "",
           soup_message_headers_get_one (request_headers, "X-Litmus-Second") ?: "");

  /* the request body is streamed to the file, or drained without
   * being accumulated when the request is refused below and the
   * client is still sending it. Pause until the serialized start has
   * run, so no body chunk is missed before the handlers are connected */
  soup_message_body_set_accumulate (soup_server_message_get_request_body (msg), FALSE);
  soup_server_message_pause (msg);

  start->req = phodav_request_ref (req);
  start->status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

  task = g_task_new (NULL, phodav_request_get_cancellable (req),
                     put_start_done, start);
  g_task_set_task_data (task, start, NULL);
  phodav_request_run_serialized (req, task, put_start_task);
  g_object_unref (task);
}
