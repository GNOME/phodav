/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-priv.h"
#include "phodav-utils.h"

typedef struct _DavMethod DavMethod;

/* the DAV methods handled by the server, in the order of the Allow
 * header of the OPTIONS answer.
 *
 * The mutating methods, which modify the shared files, are answered
 * with FORBIDDEN when the server is read-only. UNLOCK is not one of
 * them: it only releases a lock, and does not modify the files.
 *
 * A method with a headers_handler streams its request body out
 * while it is received: it is started from got_headers(), before
 * its body arrives, and it is answered from its got-body callback,
 * once the file it writes is committed (PUT answers its success
 * status only then). It has no handler: server_callback() runs
 * none for it either. It is listed like the other methods, so that
 * the Allow header covers it. */
struct _DavMethod
{
  const gchar *name;
  gboolean mutating;
  gint (*handler) (PhodavRequest *req, GError **err);
  void (*headers_handler) (PhodavRequest *req, GError **err);
};

static const DavMethod dav_methods[] = {
  { "GET", FALSE, phodav_method_get, NULL },
  { "HEAD", FALSE, phodav_method_get, NULL },
  { "PUT", TRUE, NULL, phodav_method_put },
  { "PROPFIND", FALSE, phodav_method_propfind, NULL },
  { "PROPPATCH", TRUE, phodav_method_proppatch, NULL },
  { "MKCOL", TRUE, phodav_method_mkcol, NULL },
  { "DELETE", TRUE, phodav_method_delete, NULL },
  { "MOVE", TRUE, phodav_method_movecopy, NULL },
  { "COPY", TRUE, phodav_method_movecopy, NULL },
  { "LOCK", TRUE, phodav_method_lock, NULL },
  { "UNLOCK", FALSE, phodav_method_unlock, NULL },
};

/* the per-message cancellation state, owned by the SoupServerMessage */
typedef struct _RequestCancel
{
  GCancellable *cancellable;
  GCancellable *server_cancellable;
  gulong server_handler;
  GWeakRef server;
  gint server_cancelled;
  gint finished;
} RequestCancel;

struct _PhodavRequest
{
  gint ref_count;

  PhodavServer *self;
  PathHandler *handler;
  SoupServerMessage *msg;
  gchar *method;
  gchar *path;
  SoupMessageHeaders *request_headers;
  SoupMessageBody *request_body;
  GUri *uri;
  GCancellable *cancellable;
  GMainContext *context;
  gboolean streamed;

  SoupMessageHeaders *response_headers;
  GBytes *body;
  guint status;
  gchar *reason;

  gint (*handler_func) (PhodavRequest *req, GError **err);
  GError *error;
};

static const DavMethod *
dav_method_lookup (const gchar *method)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (dav_methods); i++)
    if (g_strcmp0 (method, dav_methods[i].name) == 0)
      return &dav_methods[i];

  return NULL;
}

/* the comma separated names of the methods, for the Allow header */
static gchar *
dav_methods_allow (void)
{
  g_autoptr(GString) allow = g_string_sized_new (128);
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (dav_methods); i++)
    {
      if (i > 0)
        g_string_append (allow, ", ");
      g_string_append (allow, dav_methods[i].name);
    }

  return g_string_free_and_steal (g_steal_pointer (&allow));
}

/* the DAV requests operate on the resources under the share root:
 * their path must be absolute (it begins with a "/"), but relative
 * to the root (no second "/" may follow the first one, or the path
 * would read as absolute on Windows too), carry no dot-dot segment,
 * and their URI must carry no fragment. The dot segments are
 * removed when the request URI is parsed, and the ones surviving
 * the path decoding are refused by libsoup: checking them here too
 * guards the traversal on its own, should the URI handling change.
 * A request which does not qualify is answered with 400 BAD
 * REQUEST. */
static gboolean
request_path_is_valid (SoupServerMessage *msg, const gchar *path)
{
  GUri *uri = soup_server_message_get_uri (msg);

  if (!phodav_path_is_safe (path))
    g_debug ("invalid request path: %s", path ?: "");
  else if (!(uri && g_uri_get_fragment (uri) == NULL))
    g_debug ("using fragments in query is not supported");
  else
    return TRUE;

  return FALSE;
}

/* refuse a request before its body is parsed, and drain whatever the
 * client is still sending rather than accumulating it */
static void
request_refuse (SoupServerMessage *msg, guint status)
{
  soup_message_body_set_accumulate (soup_server_message_get_request_body (msg), FALSE);
  soup_server_message_set_status (msg, status, NULL);
}

/* a body which is not delimited by a Content-Length cannot be
 * checked before it is received: refuse it as soon as it grows over
 * the parse limit, so that a client cannot make the server
 * accumulate an unbounded request body in memory */
static void
got_chunk (SoupServerMessage *msg,
           GBytes *chunk,
           gpointer user_data)
{
  SoupMessageBody *body = soup_server_message_get_request_body (msg);

  /* the accumulated length includes the chunk being signaled, the
   * body is appended before the signal is emitted */
  if (body->length <= PHODAV_MAX_REQUEST_BODY)
    return;

  g_debug ("refusing the oversized request body");

  g_signal_handlers_disconnect_by_func (msg, got_chunk, user_data);

  /* stop accumulating the rest of the body: it is drained, and the
   * request is answered with 413 rather than parsed */
  soup_message_body_set_accumulate (body, FALSE);
  soup_server_message_set_status (msg, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE, NULL);
}

static void
copy_header (const gchar *name,
             const gchar *value,
             gpointer user_data)
{
  soup_message_headers_append (user_data, name, value);
}

static RequestCancel *
request_cancel_get (SoupServerMessage *msg)
{
  return g_object_get_data (G_OBJECT (msg), "phodav-request-cancel");
}

static GCancellable *
request_cancellable_get (SoupServerMessage *msg)
{
  RequestCancel *cancel = request_cancel_get (msg);

  if (cancel)
    return cancel->cancellable;

  return NULL;
}

PhodavRequest *
phodav_request_new (PathHandler *handler,
                    SoupServerMessage *msg,
                    const gchar *path)
{
  PhodavRequest *req = g_new0 (PhodavRequest, 1);
  SoupMessageHeaders *request_headers;
  SoupMessageBody *request_body;
  GUri *uri;
  GCancellable *cancellable;
  g_autoptr(GHashTable) params = NULL;

  req->ref_count = 1;
  req->self = g_object_ref (handler_get_server (handler));
  req->handler = path_handler_ref (handler);
  req->msg = g_object_ref (msg);
  req->method = g_strdup (soup_server_message_get_method (msg));
  req->path = g_strdup (path);

  request_headers = soup_server_message_get_request_headers (msg);
  req->request_headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);
  soup_message_headers_foreach (request_headers, copy_header, req->request_headers);

  request_body = soup_server_message_get_request_body (msg);
  req->request_body = soup_message_body_ref (request_body);

  uri = soup_server_message_get_uri (msg);
  req->uri = uri ? g_uri_ref (uri) : NULL;

  req->context = g_main_context_ref (g_main_context_get_thread_default () ?: g_main_context_default ());

  cancellable = request_cancellable_get (msg);
  req->cancellable = cancellable ? g_object_ref (cancellable) : g_cancellable_new ();

  req->response_headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_RESPONSE);
  params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (params, g_strdup ("charset"), g_strdup ("utf-8"));
  soup_message_headers_set_content_type (req->response_headers,
                                         "text/xml", params);

  /* the GET/HEAD handler evaluates the range itself, on the copied
   * headers above; remove the header from the real message so that
   * libsoup does not apply its own range handling on top of the answer */
  if (g_strcmp0 (req->method, "GET") == 0 ||
      g_strcmp0 (req->method, "HEAD") == 0)
    soup_message_headers_remove (request_headers, "Range");

  return req;
}

PhodavRequest *
phodav_request_ref (PhodavRequest *req)
{
  g_atomic_int_inc (&req->ref_count);

  return req;
}

static void
phodav_request_free (PhodavRequest *req)
{
  g_clear_object (&req->self);
  g_clear_pointer (&req->handler, path_handler_unref);
  g_clear_object (&req->msg);
  g_free (req->method);
  g_free (req->path);
  g_clear_pointer (&req->request_headers, soup_message_headers_unref);
  g_clear_pointer (&req->request_body, soup_message_body_unref);
  g_clear_pointer (&req->uri, g_uri_unref);
  g_clear_object (&req->cancellable);
  g_clear_pointer (&req->context, g_main_context_unref);
  g_clear_pointer (&req->response_headers, soup_message_headers_unref);
  g_clear_pointer (&req->body, g_bytes_unref);
  g_free (req->reason);
  g_clear_error (&req->error);
  g_free (req);
}

void
phodav_request_unref (PhodavRequest *req)
{
  if (g_atomic_int_dec_and_test (&req->ref_count))
    phodav_request_free (req);
}

PathHandler *
phodav_request_get_handler (PhodavRequest *req)
{
  return req->handler;
}

SoupServerMessage *
phodav_request_get_message (PhodavRequest *req)
{
  return req->msg;
}

const gchar *
phodav_request_get_method (PhodavRequest *req)
{
  return req->method;
}

const gchar *
phodav_request_get_path (PhodavRequest *req)
{
  return req->path;
}

SoupMessageHeaders *
phodav_request_get_request_headers (PhodavRequest *req)
{
  return req->request_headers;
}

SoupMessageBody *
phodav_request_get_request_body (PhodavRequest *req)
{
  return req->request_body;
}

GUri *
phodav_request_get_uri (PhodavRequest *req)
{
  return req->uri;
}

GCancellable *
phodav_request_get_cancellable (PhodavRequest *req)
{
  return req->cancellable;
}

SoupMessageHeaders *
phodav_request_get_response_headers (PhodavRequest *req)
{
  return req->response_headers;
}

void
phodav_request_set_status (PhodavRequest *req,
                           guint status,
                           const gchar *reason)
{
  req->status = status;
  g_free (req->reason);
  req->reason = g_strdup (reason);
}

guint
phodav_request_get_status (PhodavRequest *req)
{
  return req->status;
}

void
phodav_request_set_body_bytes (PhodavRequest *req,
                               GBytes *bytes)
{
  g_clear_pointer (&req->body, g_bytes_unref);
  req->body = bytes;
}

void
phodav_request_set_response (PhodavRequest *req,
                             const gchar *content_type,
                             SoupMemoryUse use,
                             const gchar *body,
                             gsize len)
{
  if (content_type)
    soup_message_headers_set_content_type (req->response_headers,
                                           content_type, NULL);

  g_clear_pointer (&req->body, g_bytes_unref);
  if (use == SOUP_MEMORY_TAKE)
    req->body = g_bytes_new_take ((gpointer) body, len);
  else
    req->body = g_bytes_new (body, len);
}

/* apply the response computed by a worker thread back to the message,
 * on the main context which owns it */
static void
request_apply (PhodavRequest *req)
{
  SoupServerMessage *msg = req->msg;
  SoupMessageHeaders *response_headers;

  if (req->streamed)
    return;

  soup_server_message_set_status (msg, req->status ?: SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                  req->reason);

  response_headers = soup_server_message_get_response_headers (msg);
  soup_message_headers_foreach (req->response_headers, copy_header, response_headers);

  /* append the bytes the handler prepared, rather than copying them
   * again: the GET path wraps the mapped file in a GBytes, and a large
   * file must not be duplicated on the heap */
  if (req->body && g_bytes_get_size (req->body) > 0)
    soup_message_body_append_bytes (soup_server_message_get_response_body (msg),
                                    req->body);

  soup_server_message_unpause (msg);
}

struct _PhodavStream
{
  gint ref_count;
  PhodavRequest *req;
  guint status;
  gchar *content_type;
  PhodavStreamPull pull;
  gpointer pull_data;
  GDestroyNotify pull_data_free;

  gulong wrote_id;
  gulong finished_id;

  gboolean started;
  gboolean ended;
  gboolean pulling;
};

typedef struct _StreamPull
{
  PhodavStream *stream;
  GBytes *bytes;
  GError *error;
} StreamPull;

static PhodavStream *stream_ref (PhodavStream *stream);
static void stream_unref (PhodavStream *stream);
static void stream_schedule_pull (PhodavStream *stream);
static void stream_stop (PhodavStream *stream, gboolean complete, gboolean cut);

static PhodavStream *
stream_ref (PhodavStream *stream)
{
  g_atomic_int_inc (&stream->ref_count);

  return stream;
}

static void
stream_unref (PhodavStream *stream)
{
  if (!g_atomic_int_dec_and_test (&stream->ref_count))
    return;

  g_clear_pointer (&stream->content_type, g_free);
  if (stream->pull_data_free)
    stream->pull_data_free (stream->pull_data);
  g_clear_pointer (&stream->req, phodav_request_unref);
  g_free (stream);
}

static void
stream_cut_connection (SoupServerMessage *msg)
{
  GIOStream *connection;
  GSocket *socket;

  /* The headers of a chunked response are already on the wire: the
   * only way to signal that the body is not the complete
   * representation is to cut the connection, rather than to send
   * the terminating zero-length chunk. Stealing the connection
   * makes libsoup flush and terminate the body being written, so
   * shut the socket down first to make that flush fail. */
  socket = soup_server_message_get_socket (msg);
  if (socket)
    g_socket_shutdown (socket, TRUE, TRUE, NULL);

  connection = soup_server_message_steal_connection (msg);

  if (connection)
    {
      g_io_stream_close (connection, NULL, NULL);
      g_object_unref (connection);
    }
}

static void
stream_stop (PhodavStream *stream, gboolean complete, gboolean cut)
{
  SoupServerMessage *msg = stream->req->msg;

  if (stream->ended)
    return;

  stream->ended = TRUE;

  if (stream->wrote_id)
    {
      g_signal_handler_disconnect (msg, stream->wrote_id);
      stream->wrote_id = 0;
    }
  if (stream->finished_id)
    {
      g_signal_handler_disconnect (msg, stream->finished_id);
      stream->finished_id = 0;
    }

  if (cut)
    stream_cut_connection (msg);
  else if (complete && stream->started)
    {
      soup_message_body_complete (soup_server_message_get_response_body (msg));
      soup_server_message_unpause (msg);
    }

  /* the main context reference was the stream's owner */
  stream_unref (stream);
}

static gboolean
stream_abort_cb (gpointer data)
{
  stream_stop (data, FALSE, TRUE);

  return G_SOURCE_REMOVE;
}

static void
stream_wrote_chunk (SoupServerMessage *msg,
                    gpointer user_data)
{
  PhodavStream *stream = user_data;

  stream_schedule_pull (stream);
}

static void
stream_finished (SoupServerMessage *msg,
                 gpointer user_data)
{
  PhodavStream *stream = user_data;

  /* the client went away: release the stream without trying to
   * complete or cut the connection it already dropped */
  stream_stop (stream, FALSE, FALSE);
}

static void
stream_pull_run (gpointer data)
{
  StreamPull *pull = data;
  PhodavStream *stream = pull->stream;

  pull->bytes = stream->pull (stream->pull_data,
                              phodav_request_get_cancellable (stream->req),
                              &pull->error);
}

static void
stream_pull_done (gpointer data)
{
  StreamPull *pull = data;
  PhodavStream *stream = pull->stream;
  gboolean finished;

  stream->pulling = FALSE;

  if (stream->ended)
    goto out;

  if (pull->error)
    {
      gboolean cancelled = g_cancellable_is_cancelled (phodav_request_get_cancellable (stream->req));

      finished = phodav_request_is_finished (stream->req);
      if (!cancelled || !finished)
        g_debug ("streaming failed: %s", pull->error->message);

      stream_stop (stream, FALSE, !(cancelled && finished));
      goto out;
    }

  if (!pull->bytes)
    {
      stream_stop (stream, TRUE, FALSE);
      goto out;
    }

  if (g_bytes_get_size (pull->bytes) > 0)
    {
      soup_message_body_append_bytes (soup_server_message_get_response_body (stream->req->msg),
                                      pull->bytes);
      soup_server_message_unpause (stream->req->msg);
    }
  else
    {
      /* an empty chunk is not an end of body: ask for the next one */
      stream_schedule_pull (stream);
    }

out:
  g_clear_pointer (&pull->bytes, g_bytes_unref);
  g_clear_error (&pull->error);
  stream_unref (stream);
  g_free (pull);
}

static void
stream_schedule_pull (PhodavStream *stream)
{
  StreamPull *pull;

  if (stream->ended || stream->pulling)
    return;

  if (g_cancellable_is_cancelled (phodav_request_get_cancellable (stream->req)))
    {
      stream_stop (stream, FALSE,
                   !phodav_request_is_finished (stream->req));
      return;
    }

  stream->pulling = TRUE;
  pull = g_new0 (StreamPull, 1);
  pull->stream = stream_ref (stream);

  phodav_server_dispatch (stream->req->self, stream_pull_run,
                          stream_pull_done, pull);
}

static gboolean
stream_start_cb (gpointer data)
{
  PhodavStream *stream = data;
  PhodavRequest *req = stream->req;
  SoupServerMessage *msg = req->msg;
  SoupMessageHeaders *response_headers;

  if (stream->ended)
    return G_SOURCE_REMOVE;

  soup_server_message_set_status (msg, stream->status, NULL);
  response_headers = soup_server_message_get_response_headers (msg);
  soup_message_headers_foreach (req->response_headers, copy_header, response_headers);

  if (stream->content_type)
    soup_message_headers_set_content_type (response_headers, stream->content_type, NULL);
  soup_message_headers_set_encoding (response_headers, SOUP_ENCODING_CHUNKED);

  stream->wrote_id = g_signal_connect (msg, "wrote-chunk",
                                       G_CALLBACK (stream_wrote_chunk), stream);
  stream->finished_id = g_signal_connect (msg, "finished",
                                          G_CALLBACK (stream_finished), stream);
  stream->started = TRUE;

  stream_schedule_pull (stream);

  return G_SOURCE_REMOVE;
}

PhodavStream *
phodav_request_stream_start (PhodavRequest *req,
                             guint status,
                             const gchar *content_type,
                             PhodavStreamPull pull,
                             gpointer pull_data,
                             GDestroyNotify pull_data_free)
{
  PhodavStream *stream = g_new0 (PhodavStream, 1);

  stream->ref_count = 1;
  stream->req = phodav_request_ref (req);
  stream->status = status;
  stream->content_type = g_strdup (content_type);
  stream->pull = pull;
  stream->pull_data = pull_data;
  stream->pull_data_free = pull_data_free;

  /* the response is streamed even before the main context has started
   * it: request_apply() must not replace it with an empty body */
  req->streamed = TRUE;

  g_main_context_invoke_full (req->context, G_PRIORITY_DEFAULT,
                              stream_start_cb, stream_ref (stream),
                              (GDestroyNotify) stream_unref);

  return stream;
}

void
phodav_stream_abort (PhodavStream *stream)
{
  if (!stream)
    return;

  g_main_context_invoke_full (stream->req->context, G_PRIORITY_DEFAULT,
                              stream_abort_cb, stream_ref (stream),
                              (GDestroyNotify) stream_unref);
}

static void
request_task (gpointer data)
{
  PhodavRequest *req = data;
  g_autoptr(GError) error = NULL;
  gint status;

  /* the request may have been cancelled while it was queued: do not
   * start a filesystem operation for a client which is already gone */
  if (g_cancellable_is_cancelled (req->cancellable))
    return;

  status = req->handler_func (req, &error);

  req->status = status;
  req->error = g_steal_pointer (&error);
}

gboolean
phodav_request_is_finished (PhodavRequest *req)
{
  RequestCancel *cancel = request_cancel_get (req->msg);

  return cancel && g_atomic_int_get (&cancel->finished);
}

gboolean
phodav_request_is_server_cancelled (PhodavRequest *req)
{
  RequestCancel *cancel = request_cancel_get (req->msg);

  return cancel && g_atomic_int_get (&cancel->server_cancelled);
}

static void
request_task_done (gpointer data)
{
  PhodavRequest *req = data;
  RequestCancel *cancel = request_cancel_get (req->msg);
  gboolean finished = cancel && g_atomic_int_get (&cancel->finished);
  gboolean server_cancelled = cancel && g_atomic_int_get (&cancel->server_cancelled);

  if (req->error)
    g_debug ("%s %s: %s", req->method, req->path, req->error->message);

  /* a client which went away must not be answered, but a server-side
   * cancellation (dispose) must release the paused message instead of
   * leaving the client waiting for a response which never comes */
  if (!finished)
    {
      if (g_cancellable_is_cancelled (req->cancellable))
        {
          g_debug ("request cancelled: %s %s", req->method, req->path);
          phodav_request_set_status (req,
                                     server_cancelled
                                       ? SOUP_STATUS_SERVICE_UNAVAILABLE
                                       : SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                     NULL);
        }
      else if (req->status == 0)
        phodav_request_set_status (req, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);

      request_apply (req);
    }

  phodav_request_unref (req);
}

static void
request_cancelled (GCancellable *cancellable,
                   gpointer user_data)
{
  RequestCancel *cancel = user_data;

  g_atomic_int_set (&cancel->server_cancelled, TRUE);
  g_cancellable_cancel (cancel->cancellable);
}

static void
request_cancel_free (gpointer data)
{
  RequestCancel *cancel = data;

  if (cancel->server_handler)
    g_cancellable_disconnect (cancel->server_cancellable, cancel->server_handler);
  g_clear_object (&cancel->server_cancellable);
  g_weak_ref_clear (&cancel->server);
  g_object_unref (cancel->cancellable);
  g_free (cancel);
}

static void
message_finished (SoupServerMessage *msg,
                  gpointer user_data)
{
  RequestCancel *cancel = request_cancel_get (msg);

  if (cancel)
    {
      g_atomic_int_set (&cancel->finished, TRUE);
      g_cancellable_cancel (cancel->cancellable);
    }
}

static void
got_headers (SoupServerMessage *msg,
             gpointer user_data)
{
  RequestCancel *cancel = user_data;
  g_autoptr(PhodavServer) self = g_weak_ref_get (&cancel->server);
  GUri *uri = soup_server_message_get_uri (msg);
  const gchar *path = g_uri_get_path (uri);
  const gchar *method = soup_server_message_get_method (msg);
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  const DavMethod *m = dav_method_lookup (method);

  /* the server was stopped and disposed while this request was in
   * flight: there is nothing left to handle it */
  if (!self)
    return;

  /* the requests already answered (an authentication challenge, an
   * invalid path...) are not phodav's to handle */
  if (soup_server_message_get_status (msg) != 0)
    return;

  /* without a shared root, no handler is registered: the requests
   * are answered by libsoup, phodav has nothing to do with them */
  if (!self->root_handler)
    return;

  /* only the requests under the base path are phodav's. This is
   * decided by the configured prefix, not by the routing libsoup
   * performed: an auth domain makes libsoup return from its headers
   * processing before the early handlers, so the routing cannot be
   * observed here. When the SoupServer is shared with other
   * handlers, the base path must be a prefix no other handler
   * covers. */
  if (!path || !handler_path_is_covered (self->root_handler, path))
    return;

  /* the request admission policy is applied here, before any body is
   * received: the streamed and non-streamed methods cannot diverge on
   * the path and read-only guards */
  if (!request_path_is_valid (msg, path))
    {
      request_refuse (msg, SOUP_STATUS_BAD_REQUEST);
      return;
    }

  if (m && self->readonly && m->mutating)
    {
      request_refuse (msg, SOUP_STATUS_FORBIDDEN);
      return;
    }

  /* a method which streams its request body out while it is
   * received (PUT) is started from here, before the body arrives:
   * it is answered from its got-body callback, not by
   * server_callback() */
  if (m && m->headers_handler)
    {
      g_autoptr(GError) err = NULL;
      g_autoptr(PhodavRequest) req = phodav_request_new (self->root_handler, msg, path);

      m->headers_handler (req, &err);

      if (err)
        g_debug ("%s %s: %s", method, path, err->message);

      return;
    }

  /* the other methods have their whole request body parsed in
   * memory: refuse an unreasonable Content-Length before the body
   * is received (RFC 9110 section 15.5.15), and drain it without
   * accumulating it */
  if (soup_message_headers_get_content_length (request_headers) > PHODAV_MAX_REQUEST_BODY)
    {
      g_debug ("refusing the oversized request body");
      request_refuse (msg, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);
      return;
    }

  /* a chunked body has no Content-Length to check in advance: watch
   * it while it is received */
  if (soup_message_headers_get_encoding (request_headers) == SOUP_ENCODING_CHUNKED)
    g_signal_connect (msg, "got-chunk", G_CALLBACK (got_chunk), self);
}

void
phodav_server_request_started (SoupServer *server,
                               SoupServerMessage *message,
                               gpointer user_data)
{
  PhodavServer *self = user_data;
  RequestCancel *cancel = g_new0 (RequestCancel, 1);

  cancel->cancellable = g_cancellable_new ();
  cancel->server_cancellable = g_object_ref (self->cancellable);
  cancel->server_handler = g_cancellable_connect (cancel->server_cancellable,
                                                  G_CALLBACK (request_cancelled),
                                                  cancel, NULL);
  g_weak_ref_init (&cancel->server, self);
  g_object_set_data_full (G_OBJECT (message), "phodav-request-cancel",
                          cancel, request_cancel_free);

  g_signal_connect (message, "finished", G_CALLBACK (message_finished), NULL);
  g_signal_connect (message, "got-headers", G_CALLBACK (got_headers), cancel);
}

void
phodav_server_callback (SoupServer *server, SoupServerMessage *msg, const gchar *path, GHashTable *query, gpointer user_data)
{
  g_autoptr(GError) err = NULL;
  PathHandler *handler = user_data;
  const gchar *method = soup_server_message_get_method (msg);
  const DavMethod *m = dav_method_lookup (method);
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  SoupMessageHeaders *response_headers;
  g_autoptr(GHashTable) params = NULL;

  g_debug ("%s %s HTTP/1.%d %s %s", method, path, soup_server_message_get_http_version (msg),
           soup_message_headers_get_one (request_headers, "X-Litmus") ?: "",
           soup_message_headers_get_one (request_headers, "X-Litmus-Second") ?: "");

  /* got_headers() may have already rejected the request (oversized
   * body, or a streamed method refused before its upload was
   * started); do not overwrite its status with the result of the
   * method handler */
  if (soup_server_message_get_status (msg) != 0)
    return;

  /* libsoup matches handlers by a plain string prefix, so it can
   * also route a path which merely starts with the base path here
   * ("/davos" for "/dav"): it is not part of the share */
  if (!handler_path_is_covered (handler, path))
    {
      soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
      return;
    }

  /* a streamed method was started from got_headers(), before its
   * request body was received, and is answered from its got-body
   * callback, once the file it writes is committed: there is no
   * handler to run for it here, and its answer must not be
   * overwritten */
  if (m && m->headers_handler)
    return;

  if (method == SOUP_METHOD_OPTIONS)
    {
      g_autofree gchar *allow = dav_methods_allow ();

      response_headers = soup_server_message_get_response_headers (msg);
      params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      g_hash_table_insert (params, g_strdup ("charset"), g_strdup ("utf-8"));
      soup_message_headers_set_content_type (response_headers,
                                             "text/xml", params);

      soup_message_headers_append (response_headers, "DAV", "1,2");

      /* according to http://code.google.com/p/sabredav/wiki/Windows */
      soup_message_headers_append (response_headers, "MS-Author-Via", "DAV");

      soup_message_headers_append (response_headers, "Allow", allow);
      soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
    }
  else if (m == NULL)
    {
      /* rather than warning, answer that the method is not implemented */
      g_debug ("unsupported method %s", method);
      soup_server_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED, NULL);
    }
  else
    {
      g_autoptr(PhodavRequest) req = phodav_request_new (handler, msg, path);

      /* the handlers run on the private worker pool: the response is
       * built on the request and applied back when the worker
       * completes. The filesystem operations are serialized by the
       * server-wide reader-writer lock. */
      req->handler_func = m->handler;

      soup_server_message_pause (msg);

      phodav_server_dispatch (req->self, request_task, request_task_done,
                              phodav_request_ref (req));
    }

  if (err)
    g_debug ("%s %s: %s", method, path, err->message);
}
