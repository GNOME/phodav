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

#include "phodav-priv.h"
#include "phodav-utils.h"

static void
method_put_finished (SoupServerMessage *msg,
                     gpointer           user_data)
{
  GFileOutputStream *output = user_data;

  g_debug ("PUT finished %p", output);

  g_object_unref (output);
}

static void
method_put_got_chunk (SoupServerMessage *msg,
                      GBytes            *chunk,
                      gpointer           user_data)
{
  GFileOutputStream *output = user_data;
  PathHandler *handler = g_object_get_data (user_data, "handler");
  GCancellable *cancellable = handler_get_cancellable (handler);
  GError *err = NULL;
  gsize bytes_written;
  gconstpointer data;
  gsize data_length;

  g_debug ("PUT got chunk");

  data = g_bytes_get_data (chunk, &data_length);

  if (!g_output_stream_write_all (G_OUTPUT_STREAM (output),
                                  data, data_length,
                                  &bytes_written, cancellable, &err))
    goto end;

end:
  if (err)
    {
      g_warning ("error: %s", err->message);
      g_clear_error (&err);
      soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
    }
}

static gint
put_start (SoupServerMessage *msg, GFile *file,
           GFileOutputStream **output, GCancellable *cancellable,
           GError **err)
{
  GFileOutputStream *s = NULL;
  gchar *etag = NULL;
  gboolean created = TRUE;
  SoupMessageHeaders *headers = soup_server_message_get_request_headers (msg);
  gint status = SOUP_STATUS_INTERNAL_SERVER_ERROR;

  if (g_file_query_exists (file, cancellable))
    created = FALSE;

  if (soup_message_headers_get_list (headers, "If-Match"))
    g_warn_if_reached ();
  else if (soup_message_headers_get_list (headers, "If-None-Match"))
    g_warn_if_reached ();
  else if (soup_message_headers_get_list (headers, "Expect"))
    g_warn_if_reached ();

  s = g_file_replace (file, etag, FALSE, G_FILE_CREATE_PRIVATE, cancellable, err);
  if (!s)
    goto end;

  status = created ? SOUP_STATUS_CREATED : SOUP_STATUS_OK;

end:
  *output = s;
  return status;
}

void
phodav_method_put (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  GFile *file = NULL;
  GList *submitted = NULL;
  GFileOutputStream *output = NULL;
  gint status;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);

  g_debug ("%s %s HTTP/1.%d %s %s", soup_server_message_get_method (msg), path, soup_server_message_get_http_version (msg),
           soup_message_headers_get_one (request_headers, "X-Litmus") ? : "",
           soup_message_headers_get_one (request_headers, "X-Litmus-Second") ? : "");

  if (handler_get_readonly(handler))
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
  g_object_set_data (G_OBJECT (output), "handler", handler);
  g_signal_connect (msg, "got-chunk", G_CALLBACK (method_put_got_chunk), output);
  g_signal_connect (msg, "finished", G_CALLBACK (method_put_finished), output);

end:
  soup_server_message_set_status (msg, status, NULL);
  g_clear_object (&file);
  g_debug ("  -> %d %s\n", soup_server_message_get_status (msg), soup_server_message_get_reason_phrase (msg));
}
