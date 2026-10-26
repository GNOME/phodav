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
  PhodavRequest *req; /* borrowed, the method data owner */
  SoupServerMessage *msg;
  GCancellable *cancellable;
  PhodavServer *server;
  gchar *path;
  GList *submitted; /* the lock tokens accepted at the start */
  guint64 max_upload;
  PhodavPath *handle; /* keeps the destination and its parent open */
  GFile *temp_file;   /* the staged upload, renamed over the destination on success */
  GFileOutputStream *output;
  gchar *if_match; /* the conditional headers, rechecked before the rename */
  gchar *if_none_match;
  GQueue *chunks;      /* the received chunks waiting for their turn on disk */
  GBytes *chunk;       /* the chunk being written */
  guint64 written;     /* bytes streamed to the file so far */
  gint status;         /* the success status, answered once the file is committed */
  gboolean closed;     /* the output stream was closed */
  PutPhase phase;      /* the upload outcome, see above */
  gboolean writing;    /* an asynchronous write is in flight */
  gboolean body_done;  /* the whole request body was received */
  gboolean paused;     /* the message I/O is paused while a write is in flight */
  gboolean finished;   /* the message is done, it must not be touched anymore */
  gboolean committing; /* the close/sync and commit are running or queued */
} PutContext;

static void put_write_next (PutContext *ctx);
static void put_chunk_written_cb (GObject *source_object,
                                  GAsyncResult *res,
                                  gpointer user_data);
static void put_finish (PutContext *ctx);
static void put_context_abort (PutContext *ctx);
static void put_method_free (PutContext *ctx);
static void method_put_got_chunk (SoupServerMessage *msg,
                                  GBytes *chunk,
                                  gpointer user_data);
static void method_put_got_body (SoupServerMessage *msg,
                                 gpointer user_data);
static void method_put_finished (SoupServerMessage *msg,
                                 gpointer user_data);
static gboolean put_context_close (PutContext *ctx,
                                   GCancellable *cancellable,
                                   GError **error);
static gint put_commit_staged (PutContext *ctx,
                               GCancellable *cancellable,
                               GError **err);

static PutContext *
put_context_ref (PutContext *ctx)
{
  ctx->ref_count++;

  return ctx;
}

/* remove the staged file, once no write is in flight anymore */
static void
put_context_cleanup_files (PutContext *ctx)
{
  g_autoptr(GError) close_error = NULL;
  g_autoptr(GError) delete_error = NULL;

  if (!ctx->closed && ctx->output)
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
  g_clear_object (&ctx->server);
  g_free (ctx->path);
  g_list_free_full (ctx->submitted, (GDestroyNotify) lock_submitted_free);
  g_clear_pointer (&ctx->handle, phodav_path_unref);
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
  if (--ctx->ref_count == 0)
    put_context_free (ctx);
}

static PutContext *
put_context_new (PhodavRequest *req,
                 const gchar *if_match,
                 const gchar *if_none_match)
{
  PutContext *ctx = g_new0 (PutContext, 1);
  SoupServerMessage *msg = phodav_request_get_message (req);

  ctx->ref_count = 1;
  ctx->req = req;
  ctx->msg = g_object_ref (msg);
  /* the handler is owned by the SoupServer registration, and may be
   * freed before the asynchronous write completes: keep what the
   * callbacks need */
  ctx->cancellable = g_object_ref (phodav_request_get_cancellable (req));
  ctx->server = g_object_ref (handler_get_server (phodav_request_get_handler (req)));
  ctx->path = g_strdup (phodav_request_get_path (req));
  ctx->max_upload = phodav_request_get_max_upload (req);
  ctx->chunks = g_queue_new ();
  ctx->if_match = g_strdup (if_match);
  ctx->if_none_match = g_strdup (if_none_match);
  ctx->status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

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
   * than committing it by finalizing the stream. A close/sync or
   * commit already running decides the outcome. */
  if (!ctx->committing)
    {
      if (!phodav_request_method_is_working (ctx->req))
        put_context_abort (ctx);

      phodav_request_method_abort (ctx->req);
    }
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
 * the driver runs the work step again to close and sync it, then takes
 * the namespace for the lock recheck and the rename */
static void
put_finish (PutContext *ctx)
{
  if (put_context_is_decided (ctx) || ctx->writing || ctx->chunks->length > 0 ||
      ctx->committing)
    return;

  ctx->committing = TRUE;
  g_signal_handlers_disconnect_by_func (ctx->msg, method_put_got_chunk, ctx);

  /* hold the response until the close/sync and commit have decided
   * the outcome */
  soup_server_message_pause (ctx->msg);

  phodav_request_method_body_done (ctx->req);
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
put_query_destination (PhodavPath *path, GFileQueryInfoFlags flags, GCancellable *cancellable, GError **error)
{
  g_autoptr(GError) local_error = NULL;
  GFileInfo *info;

  info = phodav_path_query_info (path,
                                 "standard::type,standard::size,etag::*,unix::inode",
                                 flags, cancellable, &local_error);
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
put_commit_staged (PutContext *ctx, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autofree gchar *etag = NULL;
  gboolean exists = FALSE;

  info = put_query_destination (ctx->handle,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                cancellable, &error);
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
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK)
        return SOUP_STATUS_FORBIDDEN;
    }

  if (!put_preconditions_hold (ctx->if_match, ctx->if_none_match, etag, exists))
    return SOUP_STATUS_PRECONDITION_FAILED;

  if (!phodav_path_rename_files (ctx->temp_file,
                                 phodav_path_get_named_file (ctx->handle),
                                 cancellable, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return SOUP_STATUS_CONFLICT;

      gint status = phodav_status_from_error (error);

      g_propagate_error (err, g_steal_pointer (&error));
      return status;
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

/* open the staging file and remember the destination; the early lock
 * and conditional checks are advisory, and are evaluated again under
 * the namespace before the rename */
static gint
put_stage (PhodavRequest *req, PhodavPath *handle, PutContext *ctx, GCancellable *cancellable, GError **err)
{
  GFile *dest_file = phodav_path_get_named_file (handle);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GFile) temp_file = NULL;
  GFileOutputStream *output = NULL;
  gboolean exists = FALSE;
  g_autofree gchar *etag = NULL;
  gint status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

  info = put_query_destination (handle, G_FILE_QUERY_INFO_NONE,
                                cancellable, &error);
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
  if (!put_preconditions_hold (ctx->if_match, ctx->if_none_match, etag, exists))
    {
      status = SOUP_STATUS_PRECONDITION_FAILED;
      goto end;
    }

  if (!put_temp_open (dest_file, &temp_file, &output, cancellable, &error))
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
  ctx->handle = phodav_path_ref (handle);
  ctx->temp_file = g_steal_pointer (&temp_file);
  ctx->output = g_steal_pointer (&output);
  ctx->status = status;

end:
  g_clear_object (&output);
  return status;
}

static gint
put_work (PhodavRequest *req, GError **err)
{
  PutContext *ctx = phodav_request_get_method_data (req);
  g_autoptr(PhodavPath) handle = NULL;
  g_autoptr(GError) error = NULL;
  gint status;

  if (ctx->handle == NULL)
    {
      /* stage the upload before the body arrives */
      if (g_cancellable_is_cancelled (ctx->cancellable))
        return 0;

      status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
      handle = phodav_path_resolve (phodav_request_get_handler (req),
                                    phodav_request_get_path (req),
                                    PHODAV_PATH_CREATE, &status);
      if (!handle)
        return status;

      status = put_stage (req, handle, ctx, ctx->cancellable, &error);
      if (error && err)
        *err = g_steal_pointer (&error);
      return status == SOUP_STATUS_OK || status == SOUP_STATUS_CREATED
               ? SOUP_STATUS_OK
               : status;
    }

  /* the body has been written: close and sync the staged file, still
   * outside the namespace */
  if (g_cancellable_is_cancelled (ctx->cancellable))
    return 0;

  if (!put_context_close (ctx, ctx->cancellable, &error))
    {
      if (err)
        *err = g_steal_pointer (&error);
      return SOUP_STATUS_INTERNAL_SERVER_ERROR;
    }

  if (!phodav_path_sync (ctx->temp_file, ctx->cancellable, &error))
    {
      if (err)
        *err = g_steal_pointer (&error);
      return SOUP_STATUS_INTERNAL_SERVER_ERROR;
    }

  return SOUP_STATUS_OK;
}

static gint
put_commit (PhodavRequest *req, GError **err)
{
  PutContext *ctx = phodav_request_get_method_data (req);
  gint status;

  if (ctx->finished || g_cancellable_is_cancelled (ctx->cancellable))
    status = 0;
  else if (phodav_lock_store_path_has_other_locks (ctx->server, ctx->path,
                                                   ctx->submitted))
    status = SOUP_STATUS_LOCKED;
  else
    status = put_commit_staged (ctx, ctx->cancellable, err);

  if (status == SOUP_STATUS_OK)
    {
      ctx->phase = PUT_PHASE_DONE;
      return ctx->status;
    }

  if (g_cancellable_is_cancelled (ctx->cancellable))
    status = SOUP_STATUS_SERVICE_UNAVAILABLE;
  else if (status == 0)
    status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

  put_context_abort (ctx);
  return status;
}

static gint
put_validate (PhodavRequest *req, GError **err)
{
  SoupServerMessage *msg = phodav_request_get_message (req);
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  guint64 max_upload = phodav_request_get_max_upload (req);
  const gchar *if_match, *if_none_match;
  PutContext *ctx;
  gint status;

  (void) err;

  if (max_upload > 0 &&
      (guint64) soup_message_headers_get_content_length (request_headers) > max_upload)
    return SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE;

  if_match = soup_message_headers_get_one (request_headers, "If-Match");
  if_none_match = soup_message_headers_get_one (request_headers, "If-None-Match");

  ctx = put_context_new (req, if_match, if_none_match);
  phodav_request_set_method_data (req, ctx, (GDestroyNotify) put_method_free);

  status = phodav_check_if (req, &ctx->submitted);
  if (status != SOUP_STATUS_OK)
    return status;

  g_signal_connect (msg, "got-chunk", G_CALLBACK (method_put_got_chunk), ctx);
  g_signal_connect (msg, "got-body", G_CALLBACK (method_put_got_body), ctx);
  g_signal_connect (msg, "finished", G_CALLBACK (method_put_finished), ctx);

  return SOUP_STATUS_OK;
}

static void
put_method_free (PutContext *ctx)
{
  if (ctx->msg)
    {
      g_signal_handlers_disconnect_by_func (ctx->msg, method_put_got_chunk, ctx);
      g_signal_handlers_disconnect_by_func (ctx->msg, method_put_got_body, ctx);
      g_signal_handlers_disconnect_by_func (ctx->msg, method_put_finished, ctx);
    }

  put_context_unref (ctx);
}

const PhodavMethodOps phodav_method_put_ops = {
  .validate = put_validate,
  .work = put_work,
  .commit = put_commit,
};
