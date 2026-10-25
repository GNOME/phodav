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
  gint (*handler) (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
  void (*headers_handler) (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err);
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
got_headers (SoupServerMessage *msg,
             gpointer user_data)
{
  PhodavServer *self = user_data;
  GUri *uri = soup_server_message_get_uri (msg);
  const gchar *path = g_uri_get_path (uri);
  const gchar *method = soup_server_message_get_method (msg);
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  const DavMethod *m = dav_method_lookup (method);

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
  if (!path || !path_is_covered (path, self->base_path))
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

      m->headers_handler (self->root_handler, msg, path, &err);

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

  g_signal_connect (message, "got-headers", G_CALLBACK (got_headers), self);
}

void
phodav_server_callback (SoupServer *server, SoupServerMessage *msg, const gchar *path, GHashTable *query, gpointer user_data)
{
  g_autoptr(GError) err = NULL;
  PathHandler *handler = user_data;
  gint status = SOUP_STATUS_NOT_IMPLEMENTED;

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

  params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (params, g_strdup ("charset"), g_strdup ("utf-8"));
  response_headers = soup_server_message_get_response_headers (msg);
  soup_message_headers_set_content_type (response_headers,
                                         "text/xml", params);

  if (method == SOUP_METHOD_OPTIONS)
    {
      g_autofree gchar *allow = dav_methods_allow ();

      soup_message_headers_append (response_headers, "DAV", "1,2");

      /* according to http://code.google.com/p/sabredav/wiki/Windows */
      soup_message_headers_append (response_headers, "MS-Author-Via", "DAV");

      soup_message_headers_append (response_headers, "Allow", allow);
      status = SOUP_STATUS_OK;
    }
  else
    {
      if (m == NULL)
        {
          /* rather than warning, answer that the method is not implemented */
          g_debug ("unsupported method %s", method);
        }
      else
        status = m->handler (handler, msg, path, &err);
    }

  soup_server_message_set_status (msg, status, NULL);

  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
  if (err)
    g_debug ("%s %s: %s", method, path, err->message);
}
