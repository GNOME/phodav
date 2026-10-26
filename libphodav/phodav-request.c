/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-priv.h"
#include "phodav-server-priv.h"
#include "phodav-utils.h"

typedef struct _DavMethod DavMethod;

/* the DAV methods handled by the server, in the order of the Allow
 * header of the OPTIONS answer.
 *
 * The mutating methods, which modify the shared files, are answered
 * with FORBIDDEN when the server is read-only. UNLOCK is not one of
 * them: it only releases a lock, and does not modify the files.
 *
 * A streamed method consumes its request body as it arrives (PUT): it
 * is started from got_headers(), before its body arrives, and its work
 * step runs again once the body has been written, before the commit.
 * A serialised method takes the namespace around validate, work and
 * commit; reads do not take it. */
struct _DavMethod
{
  const gchar *name;
  gboolean mutating;
  gboolean serialized;
  gboolean streamed;
  const PhodavMethodOps *ops;
};

static const DavMethod dav_methods[] = {
  { "GET", FALSE, FALSE, FALSE, &phodav_method_get_ops },
  { "HEAD", FALSE, FALSE, FALSE, &phodav_method_get_ops },
  { "PUT", TRUE, TRUE, TRUE, &phodav_method_put_ops },
  { "PROPFIND", FALSE, FALSE, FALSE, &phodav_method_propfind_ops },
  { "PROPPATCH", TRUE, TRUE, FALSE, &phodav_method_proppatch_ops },
  { "MKCOL", TRUE, TRUE, FALSE, &phodav_method_mkcol_ops },
  { "DELETE", TRUE, TRUE, FALSE, &phodav_method_delete_ops },
  { "MOVE", TRUE, TRUE, FALSE, &phodav_method_movecopy_ops },
  { "COPY", TRUE, TRUE, FALSE, &phodav_method_movecopy_ops },
  { "LOCK", TRUE, TRUE, FALSE, &phodav_method_lock_ops },
  { "UNLOCK", FALSE, TRUE, FALSE, &phodav_method_unlock_ops },
};

/* the request lifecycle, kept in one state field. All the transitions
 * happen on the main context; the filesystem job only sees the
 * immutable request target and its cancellable. */
typedef enum
{
  PHODAV_REQUEST_ADMITTED = 1 << 0,
  PHODAV_REQUEST_RUNNING = 1 << 1,
  PHODAV_REQUEST_STREAMING = 1 << 2,
  PHODAV_REQUEST_DONE = 1 << 3,
  PHODAV_REQUEST_CLIENT_GONE = 1 << 4,
  PHODAV_REQUEST_SERVER_STOPPING = 1 << 5,
} G_GNUC_FLAG_ENUM PhodavRequestState;

struct _PhodavRequest
{
  gint ref_count;

  PhodavServer *self;
  PathHandler *handler;
  SoupServerMessage *msg;
  gchar *method_name;
  gchar *path;
  SoupMessageHeaders *request_headers;
  SoupMessageBody *request_body;
  GUri *uri;
  GCancellable *cancellable;
  GCancellable *server_cancellable;
  gulong server_handler;
  GMainContext *context;
  gint state;

  gboolean readonly;
  guint64 max_upload;

  SoupMessageHeaders *response_headers;
  GBytes *body;
  guint status;
  gchar *reason;

  const DavMethod *method;
  gpointer method_data;
  GDestroyNotify method_data_free;
  gboolean namespace_held;
  gboolean body_done;
  gboolean method_work_active;
  gboolean method_finish_pending;
  gint pending_status;
  GError *pending_error;
  GError *error;
};

typedef enum
{
  PHODAV_STREAM_STARTED = 1 << 0,
  PHODAV_STREAM_ENDED = 1 << 1,
  PHODAV_STREAM_PULLING = 1 << 2,
} G_GNUC_FLAG_ENUM PhodavStreamState;

typedef enum
{
  REQUEST_ADMISSION_IGNORED,
  REQUEST_ADMISSION_REFUSED,
  REQUEST_ADMISSION_PENDING,
  REQUEST_ADMISSION_STREAMED,
} RequestAdmission;

static void request_server_cancelled (GCancellable *cancellable,
                                      gpointer user_data);
static RequestAdmission request_admit (PhodavRequest *req);
static void request_update_target (PhodavRequest *req);
static void request_method_start (PhodavRequest *req);
static void request_method_start_streamed (PhodavRequest *req);
static void request_method_finish (PhodavRequest *req,
                                   gint status,
                                   GError *error);

static gboolean
request_state_has (const PhodavRequest *req, PhodavRequestState state)
{
  return (req->state & state) != 0;
}

static void
request_state_add (PhodavRequest *req, PhodavRequestState state)
{
  req->state |= state;
}

/* break the message -> request -> message reference cycle once the
 * request no longer needs to be found from its message */
static void
request_detach (PhodavRequest *req)
{
  if (g_object_get_data (G_OBJECT (req->msg), "phodav-request") == req)
    g_object_set_data (G_OBJECT (req->msg), "phodav-request", NULL);
}

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

PhodavRequest *
phodav_request_new (PhodavServer *self,
                    PathHandler *handler,
                    SoupServerMessage *msg,
                    const gchar *path)
{
  PhodavRequest *req = g_new0 (PhodavRequest, 1);
  SoupMessageHeaders *request_headers;
  SoupMessageBody *request_body;
  GUri *uri;
  g_autoptr(GHashTable) params = NULL;

  req->ref_count = 1;
  req->self = g_object_ref (self);
  req->handler = handler ? path_handler_ref (handler) : NULL;
  req->msg = g_object_ref (msg);
  req->method_name = g_strdup (soup_server_message_get_method (msg));
  req->path = g_strdup (path);

  /* the request keeps the configuration it was admitted with, so a
   * property change cannot alter an operation in flight */
  req->readonly = self->readonly;
  req->max_upload = self->max_upload;

  request_headers = soup_server_message_get_request_headers (msg);
  req->request_headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);
  soup_message_headers_foreach (request_headers, copy_header, req->request_headers);

  request_body = soup_server_message_get_request_body (msg);
  req->request_body = soup_message_body_ref (request_body);

  uri = soup_server_message_get_uri (msg);
  req->uri = uri ? g_uri_ref (uri) : NULL;

  req->context = g_main_context_ref (g_main_context_get_thread_default () ?: g_main_context_default ());
  req->cancellable = g_cancellable_new ();
  req->server_cancellable = g_object_ref (self->cancellable);
  req->server_handler = g_cancellable_connect (req->server_cancellable,
                                               G_CALLBACK (request_server_cancelled),
                                               req, NULL);

  req->response_headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_RESPONSE);
  params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (params, g_strdup ("charset"), g_strdup ("utf-8"));
  soup_message_headers_set_content_type (req->response_headers,
                                         "text/xml", params);

  /* the GET/HEAD handler evaluates the range itself, on the copied
   * headers above; remove the header from the real message so that
   * libsoup does not apply its own range handling on top of the answer */
  if (g_strcmp0 (req->method_name, "GET") == 0 ||
      g_strcmp0 (req->method_name, "HEAD") == 0)
    soup_message_headers_remove (request_headers, "Range");

  return req;
}

PhodavRequest *
phodav_request_ref (PhodavRequest *req)
{
  req->ref_count++;

  return req;
}

static void
phodav_request_free (PhodavRequest *req)
{
  g_signal_handlers_disconnect_by_data (req->msg, req);
  g_clear_object (&req->self);
  g_clear_pointer (&req->handler, path_handler_unref);
  g_clear_object (&req->msg);
  g_free (req->method_name);
  g_free (req->path);
  g_clear_pointer (&req->request_headers, soup_message_headers_unref);
  g_clear_pointer (&req->request_body, soup_message_body_unref);
  g_clear_pointer (&req->uri, g_uri_unref);
  if (req->server_handler)
    g_cancellable_disconnect (req->server_cancellable, req->server_handler);
  g_clear_object (&req->server_cancellable);
  g_clear_object (&req->cancellable);
  g_clear_pointer (&req->context, g_main_context_unref);
  g_clear_pointer (&req->response_headers, soup_message_headers_unref);
  g_clear_pointer (&req->body, g_bytes_unref);
  g_free (req->reason);
  if (req->method_data && req->method_data_free)
    req->method_data_free (req->method_data);
  g_clear_error (&req->pending_error);
  g_clear_error (&req->error);
  g_free (req);
}

void
phodav_request_unref (PhodavRequest *req)
{
  if (--req->ref_count == 0)
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
  return req->method_name;
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

guint64
phodav_request_get_max_upload (PhodavRequest *req)
{
  return req->max_upload;
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
  g_set_str (&req->reason, reason);
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

/* apply the response computed by a method back to the message, on the
 * main context which owns it */
static void
request_apply (PhodavRequest *req)
{
  SoupServerMessage *msg = req->msg;
  SoupMessageHeaders *response_headers;

  if (request_state_has (req, PHODAV_REQUEST_STREAMING))
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

  guint state;
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
  stream->ref_count++;

  return stream;
}

static void
stream_unref (PhodavStream *stream)
{
  if (--stream->ref_count > 0)
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

  if (stream->state & PHODAV_STREAM_ENDED)
    return;

  stream->state |= PHODAV_STREAM_ENDED;

  g_clear_signal_handler (&stream->wrote_id, msg);
  g_clear_signal_handler (&stream->finished_id, msg);

  if (cut)
    stream_cut_connection (msg);
  else if (complete && (stream->state & PHODAV_STREAM_STARTED))
    {
      soup_message_body_complete (soup_server_message_get_response_body (msg));
      soup_server_message_unpause (msg);
    }

  request_detach (stream->req);

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

  stream->state &= ~PHODAV_STREAM_PULLING;

  if (stream->state & PHODAV_STREAM_ENDED)
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

  if (stream->state & (PHODAV_STREAM_ENDED | PHODAV_STREAM_PULLING))
    return;

  if (g_cancellable_is_cancelled (phodav_request_get_cancellable (stream->req)))
    {
      stream_stop (stream, FALSE,
                   !phodav_request_is_finished (stream->req));
      return;
    }

  stream->state |= PHODAV_STREAM_PULLING;
  pull = g_new0 (StreamPull, 1);
  pull->stream = stream_ref (stream);

  phodav_server_dispatch_stream (stream->req->self, stream_pull_run,
                                 stream_pull_done, pull);
}

static gboolean
stream_start_cb (gpointer data)
{
  PhodavStream *stream = data;
  PhodavRequest *req = stream->req;
  SoupServerMessage *msg = req->msg;
  SoupMessageHeaders *response_headers;

  if (stream->state & PHODAV_STREAM_ENDED)
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
  stream->state |= PHODAV_STREAM_STARTED;

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
  request_state_add (req, PHODAV_REQUEST_STREAMING);

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

static PhodavRequest *
request_get (SoupServerMessage *msg)
{
  return g_object_get_data (G_OBJECT (msg), "phodav-request");
}

static void
request_store (SoupServerMessage *msg, PhodavRequest *req)
{
  g_object_set_data_full (G_OBJECT (msg), "phodav-request",
                          req, (GDestroyNotify) phodav_request_unref);
}

gboolean
phodav_request_is_finished (PhodavRequest *req)
{
  return request_state_has (req, PHODAV_REQUEST_CLIENT_GONE);
}

gboolean
phodav_request_is_server_cancelled (PhodavRequest *req)
{
  return request_state_has (req, PHODAV_REQUEST_SERVER_STOPPING);
}

/* apply the outcome of a method on the main context. The method calls
 * this once, either directly when it has nothing to wait for, or from
 * the completion of its one-shot filesystem job. */
void
phodav_request_complete (PhodavRequest *req,
                         gint status,
                         GError *error)
{
  gboolean finished = request_state_has (req, PHODAV_REQUEST_CLIENT_GONE);
  gboolean server_cancelled = request_state_has (req, PHODAV_REQUEST_SERVER_STOPPING);
  gboolean streaming = request_state_has (req, PHODAV_REQUEST_STREAMING);

  g_clear_error (&req->error);
  req->error = error;
  req->status = status;

  if (req->error)
    g_debug ("%s %s: %s", req->method_name, req->path, req->error->message);

  /* a client which went away must not be answered, but a server-side
   * cancellation (dispose) must release the paused message instead of
   * leaving the client waiting for a response which never comes */
  if (!finished)
    {
      if (g_cancellable_is_cancelled (req->cancellable))
        {
          g_debug ("request cancelled: %s %s", req->method_name, req->path);
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

  /* a streamed response keeps the request reachable from its message
   * until the stream stops, so that the client-going-away transition
   * is still observed */
  if (!streaming)
    {
      request_state_add (req, PHODAV_REQUEST_DONE);
      request_detach (req);
    }
}

void
phodav_request_set_method_data (PhodavRequest *req,
                                gpointer data,
                                GDestroyNotify data_free)
{
  g_return_if_fail (req->method_data == NULL);

  req->method_data = data;
  req->method_data_free = data_free;
}

gpointer
phodav_request_get_method_data (PhodavRequest *req)
{
  return req->method_data;
}

/* release everything the request owns, and publish its outcome; this
 * is the only path which releases the namespace, frees the method
 * data and completes the request */
static void
request_method_finalize (PhodavRequest *req,
                         gint status,
                         GError *error)
{
  if (request_state_has (req, PHODAV_REQUEST_DONE))
    {
      g_clear_error (&error);
      return;
    }

  if (req->namespace_held)
    {
      phodav_server_namespace_release (req->self);
      req->namespace_held = FALSE;
    }

  if (req->method_data)
    {
      GDestroyNotify data_free = req->method_data_free;
      gpointer data = req->method_data;

      req->method_data = NULL;
      req->method_data_free = NULL;
      if (data_free)
        data_free (data);
    }

  phodav_request_complete (req, status, error);
}

/* stop a method which is still waiting for its worker. The result of
 * the running work is discarded when it returns. */
static void
request_method_finish (PhodavRequest *req,
                       gint status,
                       GError *error)
{
  if (req->method_work_active)
    {
      req->method_finish_pending = TRUE;
      req->pending_status = status;
      g_clear_error (&req->pending_error);
      req->pending_error = error;
      return;
    }

  request_method_finalize (req, status, error);
}

static void
request_method_commit (PhodavRequest *req);

static void
request_method_commit_ready (gpointer data)
{
  PhodavRequest *req = data;

  req->namespace_held = TRUE;
  request_method_commit (req);
}

static void
request_method_commit (PhodavRequest *req)
{
  g_autoptr(GError) error = NULL;
  gint status = SOUP_STATUS_OK;

  if (req->method_finish_pending)
    {
      status = req->pending_status;
      error = g_steal_pointer (&req->pending_error);
      request_method_finalize (req, status, g_steal_pointer (&error));
      return;
    }

  if (req->method->ops->commit)
    status = req->method->ops->commit (req, &error);

  request_method_finalize (req, status, g_steal_pointer (&error));
}

static void
request_method_after_work (PhodavRequest *req,
                           gint status,
                           GError *error)
{
  if (req->method_finish_pending)
    {
      g_clear_error (&error);
      status = req->pending_status;
      error = g_steal_pointer (&req->pending_error);
      request_method_finalize (req, status, error);
      return;
    }

  if (status != SOUP_STATUS_OK)
    {
      request_method_finalize (req, status, error);
      return;
    }

  /* a streamed method has only staged the upload: wait for the body,
   * then run the same work step again to close and sync it */
  if (req->method->streamed && !req->body_done)
    {
      if (phodav_request_is_finished (req) ||
          g_cancellable_is_cancelled (req->cancellable))
        {
          request_method_finalize (req, 0, NULL);
          return;
        }

      soup_server_message_unpause (req->msg);
      return;
    }

  /* the final commit of a streamed method, or a method without work,
   * takes the namespace only now */
  if (req->method->serialized && !req->namespace_held)
    phodav_server_namespace_acquire (req->self,
                                     request_method_commit_ready, req);
  else
    request_method_commit (req);
}

typedef struct _MethodJob
{
  PhodavRequest *req;
  gint status;
  GError *error;
} MethodJob;

static void
request_method_work_run (gpointer data)
{
  MethodJob *job = data;

  job->status = job->req->method->ops->work (job->req, &job->error);
}

static void
request_method_work_done (gpointer data)
{
  MethodJob *job = data;
  PhodavRequest *req = job->req;

  req->method_work_active = FALSE;
  request_method_after_work (req, job->status, g_steal_pointer (&job->error));

  phodav_request_unref (req);
  g_free (job);
}

static void
request_method_dispatch_work (PhodavRequest *req)
{
  MethodJob *job;

  if (!req->method->ops->work)
    {
      request_method_after_work (req, SOUP_STATUS_OK, NULL);
      return;
    }

  job = g_new0 (MethodJob, 1);
  job->req = phodav_request_ref (req);
  req->method_work_active = TRUE;

  if (req->method->serialized && !req->method->streamed)
    phodav_server_dispatch (req->self,
                            request_method_work_run,
                            request_method_work_done, job);
  else
    phodav_server_dispatch_stream (req->self,
                                   request_method_work_run,
                                   request_method_work_done, job);
}

static void
request_method_validate (PhodavRequest *req)
{
  g_autoptr(GError) error = NULL;
  gint status;

  if (phodav_request_is_finished (req) ||
      g_cancellable_is_cancelled (req->cancellable))
    {
      request_method_finish (req, 0, NULL);
      return;
    }

  status = req->method->ops->validate (req, &error);
  if (status != SOUP_STATUS_OK)
    {
      request_method_finish (req, status, g_steal_pointer (&error));
      return;
    }

  request_method_dispatch_work (req);
}

static void
request_method_validate_ready (gpointer data)
{
  PhodavRequest *req = data;

  req->namespace_held = TRUE;
  request_method_validate (req);
}

static void
request_method_start (PhodavRequest *req)
{
  if (!req->method || req->method->streamed)
    return;

  request_state_add (req, PHODAV_REQUEST_RUNNING);
  soup_server_message_pause (req->msg);

  if (req->method->serialized)
    phodav_server_namespace_acquire (req->self,
                                     request_method_validate_ready, req);
  else
    request_method_validate (req);
}

static void
request_method_start_streamed (PhodavRequest *req)
{
  if (!req->method || !req->method->streamed)
    return;

  request_state_add (req, PHODAV_REQUEST_RUNNING);
  soup_message_body_set_accumulate (soup_server_message_get_request_body (req->msg), FALSE);
  soup_server_message_pause (req->msg);
  request_method_validate (req);
}

void
phodav_request_method_body_done (PhodavRequest *req)
{
  if (!req->method || !req->method->streamed || req->body_done)
    return;

  req->body_done = TRUE;

  if (phodav_request_is_finished (req) ||
      g_cancellable_is_cancelled (req->cancellable))
    {
      request_method_finish (req, 0, NULL);
      return;
    }

  soup_server_message_pause (req->msg);
  request_method_dispatch_work (req);
}

void
phodav_request_method_abort (PhodavRequest *req)
{
  if (!req->method)
    return;

  request_method_finish (req, 0, NULL);
}

gboolean
phodav_request_method_is_working (PhodavRequest *req)
{
  return req->method_work_active;
}

static void
request_server_cancelled (GCancellable *cancellable,
                          gpointer user_data)
{
  PhodavRequest *req = user_data;

  request_state_add (req, PHODAV_REQUEST_SERVER_STOPPING);
  g_cancellable_cancel (req->cancellable);

  /* a streamed body may be waiting with no worker to notice the
   * cancellation; stop it here so its staged file is released */
  if (req->method && req->method->streamed)
    phodav_request_method_abort (req);
}

static void
request_message_finished (SoupServerMessage *msg,
                          gpointer user_data)
{
  PhodavRequest *req = user_data;
  g_autoptr(PhodavRequest) hold = phodav_request_ref (req);

  request_state_add (req, PHODAV_REQUEST_CLIENT_GONE);
  g_cancellable_cancel (req->cancellable);
  request_detach (req);
}

/* the one target snapshot: request-started() runs before libsoup has
 * published the method, URI and headers, so the request is filled from
 * got-headers(), the first point where they are final */
static void
request_update_target (PhodavRequest *req)
{
  SoupMessageHeaders *headers = soup_server_message_get_request_headers (req->msg);
  SoupMessageBody *body = soup_server_message_get_request_body (req->msg);
  GUri *uri = soup_server_message_get_uri (req->msg);

  g_set_str (&req->method_name, soup_server_message_get_method (req->msg));

  g_set_str (&req->path, uri ? g_uri_get_path (uri) : NULL);

  g_clear_pointer (&req->uri, g_uri_unref);
  req->uri = uri ? g_uri_ref (uri) : NULL;

  g_clear_pointer (&req->request_headers, soup_message_headers_unref);
  req->request_headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);
  soup_message_headers_foreach (headers, copy_header, req->request_headers);

  g_clear_pointer (&req->request_body, soup_message_body_unref);
  req->request_body = soup_message_body_ref (body);

  /* the GET/HEAD handler evaluates the range itself, on the copied
   * headers above; remove the header from the real message so that
   * libsoup does not apply its own range handling on top of the answer */
  if (g_strcmp0 (req->method_name, "GET") == 0 ||
      g_strcmp0 (req->method_name, "HEAD") == 0)
    soup_message_headers_remove (headers, "Range");

  if (!req->handler && req->self->root_handler)
    req->handler = path_handler_ref (req->self->root_handler);
}

/* the single admission path for the got-headers and handler entry
 * points. It snapshots the target once, validates the request before
 * its body is parsed and starts the streamed methods which have to run
 * from got-headers(). */
static RequestAdmission
request_admit (PhodavRequest *req)
{
  SoupMessageHeaders *request_headers;

  if (request_state_has (req, PHODAV_REQUEST_ADMITTED))
    {
      if (request_state_has (req, PHODAV_REQUEST_CLIENT_GONE |
                                    PHODAV_REQUEST_SERVER_STOPPING |
                                    PHODAV_REQUEST_DONE))
        return REQUEST_ADMISSION_REFUSED;

      return req->method && req->method->streamed
               ? REQUEST_ADMISSION_STREAMED
               : REQUEST_ADMISSION_PENDING;
    }

  request_update_target (req);
  req->method = dav_method_lookup (req->method_name ? req->method_name : "");
  request_headers = req->request_headers;

  /* the requests already answered (an authentication challenge, an
   * invalid path...) are not phodav's to handle */
  if (soup_server_message_get_status (req->msg) != 0)
    return REQUEST_ADMISSION_IGNORED;

  if (request_state_has (req, PHODAV_REQUEST_CLIENT_GONE |
                                PHODAV_REQUEST_SERVER_STOPPING))
    return REQUEST_ADMISSION_IGNORED;

  /* without a handler the request is not under the share mounted by
   * this server */
  if (!req->handler || !req->path ||
      !handler_path_is_covered (req->handler, req->path))
    return REQUEST_ADMISSION_IGNORED;

  if (!request_path_is_valid (req->msg, req->path))
    {
      request_refuse (req->msg, SOUP_STATUS_BAD_REQUEST);
      return REQUEST_ADMISSION_REFUSED;
    }

  if (req->method && req->readonly && req->method->mutating)
    {
      request_refuse (req->msg, SOUP_STATUS_FORBIDDEN);
      return REQUEST_ADMISSION_REFUSED;
    }

  request_state_add (req, PHODAV_REQUEST_ADMITTED);

  /* a streamed method is started from here, before the body arrives;
   * its work step runs again when the body has been written */
  if (req->method && req->method->streamed)
    {
      request_method_start_streamed (req);
      return REQUEST_ADMISSION_STREAMED;
    }

  /* the other methods have their whole request body parsed in
   * memory: refuse an unreasonable Content-Length before the body
   * is received (RFC 9110 section 15.5.15), and drain it without
   * accumulating it */
  if (soup_message_headers_get_content_length (request_headers) > PHODAV_MAX_REQUEST_BODY)
    {
      g_debug ("refusing the oversized request body");
      request_refuse (req->msg, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);
      return REQUEST_ADMISSION_REFUSED;
    }

  /* a chunked body has no Content-Length to check in advance: watch
   * it while it is received */
  if (soup_message_headers_get_encoding (request_headers) == SOUP_ENCODING_CHUNKED)
    g_signal_connect (req->msg, "got-chunk", G_CALLBACK (got_chunk), req);

  return REQUEST_ADMISSION_PENDING;
}

static void
request_got_headers (SoupServerMessage *msg,
                     gpointer user_data)
{
  PhodavRequest *req = user_data;
  g_autoptr(PhodavRequest) hold = phodav_request_ref (req);

  request_admit (req);
}

void
phodav_server_request_started (SoupServer *server,
                               SoupServerMessage *message,
                               gpointer user_data)
{
  PhodavServer *self = user_data;
  GUri *uri = soup_server_message_get_uri (message);
  const gchar *path = uri ? g_uri_get_path (uri) : NULL;
  PhodavRequest *req = phodav_request_new (self, self->root_handler,
                                           message, path);

  request_store (message, req);

  g_signal_connect (message, "finished",
                    G_CALLBACK (request_message_finished), req);
  g_signal_connect (message, "got-headers",
                    G_CALLBACK (request_got_headers), req);
}

void
phodav_server_callback (SoupServer *server, SoupServerMessage *msg, const gchar *path, GHashTable *query, gpointer user_data)
{
  PathHandler *handler = user_data;
  const gchar *method = soup_server_message_get_method (msg);
  const DavMethod *m = dav_method_lookup (method);
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  SoupMessageHeaders *response_headers;
  g_autoptr(GHashTable) params = NULL;
  PhodavRequest *req;
  RequestAdmission admission;

  g_debug ("%s %s HTTP/1.%d %s %s", method, path, soup_server_message_get_http_version (msg),
           soup_message_headers_get_one (request_headers, "X-Litmus") ?: "",
           soup_message_headers_get_one (request_headers, "X-Litmus-Second") ?: "");

  req = request_get (msg);
  if (!req)
    {
      PhodavServer *self = handler_get_server (handler);

      req = phodav_request_new (self, handler, msg, path);
      request_store (msg, req);
      g_signal_connect (msg, "finished",
                        G_CALLBACK (request_message_finished), req);
    }

  admission = request_admit (req);

  /* got_headers() may have already rejected the request (oversized
   * body, or a streamed method refused before its upload was
   * started); do not overwrite its status with the result of the
   * method driver */
  if (soup_server_message_get_status (msg) != 0)
    return;

  if (admission == REQUEST_ADMISSION_IGNORED)
    {
      /* libsoup matches handlers by a plain string prefix, so it can
       * also route a path which merely starts with the base path here
       * ("/davos" for "/dav"): it is not part of the share */
      soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
      return;
    }

  if (admission != REQUEST_ADMISSION_PENDING)
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
      /* the driver validates, runs the blocking work and commits,
       * and owns the namespace and the method state throughout */
      request_method_start (req);
    }
}
