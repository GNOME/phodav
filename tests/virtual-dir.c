/*
 * Copyright (C) 2020 Red Hat, Inc.
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
#include <stdlib.h>
#include <glib.h>

#include "libphodav/phodav.h"

#define SERVER_URI "dav://localhost:8080/"

static SoupSession *session;

static const gchar test_put_data[] = "test_put: test data";

typedef struct _TestCase {
  const gchar *method;
  const gchar *path;
  guint        status_code;
  const gchar *destination;
} TestCase;

static void
test_generic (gconstpointer data)
{
  const TestCase *test = data;
  gchar *uri = g_build_path ("/", SERVER_URI, test->path, NULL);
  SoupMessage *msg = soup_message_new (test->method, uri);
  g_free (uri);

  if (test->method == SOUP_METHOD_COPY)
    {
      gchar *dest_uri = g_build_path ("/", SERVER_URI, test->destination, NULL);
      soup_message_headers_append (soup_message_get_request_headers (msg), "Destination", dest_uri);
      g_free (dest_uri);
    }

  if (test->method == SOUP_METHOD_PUT)
    {
      GBytes *bytes;

      bytes = g_bytes_new_static (test_put_data, strlen (test_put_data));
      soup_message_set_request_body_from_bytes (msg, NULL, bytes);
      g_bytes_unref (bytes);
    }

  GError *error = NULL;
  GInputStream *in = soup_session_send (session, msg, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpint (soup_message_get_status (msg), ==, test->status_code);

  g_object_unref (in);
  g_object_unref (msg);
}

static gchar *
replace_char_dup (const gchar *str, gchar old, gchar new)
{
  gchar *copy = g_strdup (str);
  gchar *pos = strchr (copy, old);
  while (pos)
    {
      *pos = new;
      pos = strchr (pos, old);
    }
  return copy;
}

static gchar *
get_test_path (const TestCase *test)
{
  gchar *path = replace_char_dup (test->path, '/', '\\');
  gchar *test_path = g_build_path ("/", "/", test->method, path, NULL);
  g_free (path);
  if (test->method == SOUP_METHOD_COPY)
    {
      gchar *dest = replace_char_dup (test->destination, '/', '\\');
      gchar *tmp = g_strconcat (test_path, "->", dest, NULL);
      g_free (dest);
      g_free (test_path);
      test_path = tmp;
    }
  return test_path;
}

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GSubprocess *server_subproc;
  server_subproc = g_subprocess_new (
    G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error,
    "tests/virtual-dir-server", "--quit-on-stdin", NULL);

  if (error) {
    g_printerr ("Failed to launch virtual-dir-server: %s\n", error->message);
    g_error_free (error);
    return 1;
  }

  /* wait for the server to start */
  GDataInputStream *stream = g_data_input_stream_new (g_subprocess_get_stdout_pipe (server_subproc));
  g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (stream), FALSE);
  gboolean server_started = FALSE;
  while (!server_started) {
    char *line = g_data_input_stream_read_line_utf8 (stream, NULL, NULL, &error);

    if (error) {
      g_printerr ("Failed to start virtual-dir-server: %s\n", error->message);
      g_error_free (error);
      return 1;
    }

    if (line == NULL) {
      g_printerr ("Failed to start virtual-dir-server\n");
      return 1;
    }

    server_started = g_strcmp0 (line, "OK") == 0;
    g_free (line);
  }
  g_object_unref (stream);

  g_test_init (&argc, &argv, NULL);

  session = soup_session_new ();

  /* the order matters, some tests depend on the previous ones */
  const TestCase tests[] = {
    {SOUP_METHOD_GET, "/", SOUP_STATUS_OK},
    {SOUP_METHOD_GET, "/virtual", SOUP_STATUS_OK},
    {SOUP_METHOD_GET, "/non-existent", SOUP_STATUS_NOT_FOUND},
    {SOUP_METHOD_GET, "/virtual/non-existent", SOUP_STATUS_NOT_FOUND},
    {SOUP_METHOD_GET, "/virtual/real", SOUP_STATUS_OK},

    {SOUP_METHOD_MKCOL, "/A", SOUP_STATUS_CREATED},
    {SOUP_METHOD_MKCOL, "/virtual/B", SOUP_STATUS_FORBIDDEN},
    {SOUP_METHOD_MKCOL, "/virtual/real/B", SOUP_STATUS_CREATED},

    {SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_CREATED, "/test-copy.txt"},
    {SOUP_METHOD_COPY, "/virtual", SOUP_STATUS_FORBIDDEN, "/virtual-copy"},
    {SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_FORBIDDEN, "/virtual/test-copy.txt"},
    {SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_CREATED, "/virtual/real/test-copy.txt"},

    {SOUP_METHOD_PUT, "/test-put.txt", SOUP_STATUS_CREATED},
    {SOUP_METHOD_PUT, "/virtual/test-put.txt", SOUP_STATUS_INTERNAL_SERVER_ERROR},
    {SOUP_METHOD_PUT, "/virtual/real/test-put.txt", SOUP_STATUS_CREATED},

    {SOUP_METHOD_DELETE, "/A", SOUP_STATUS_NO_CONTENT},
    {SOUP_METHOD_DELETE, "/virtual/real/B", SOUP_STATUS_NO_CONTENT},
    {SOUP_METHOD_DELETE, "/virtual", SOUP_STATUS_FORBIDDEN},
  };

  for (guint i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      gchar *test_path = get_test_path (&tests[i]);
      g_test_add_data_func (test_path, &tests[i], test_generic);
      g_free (test_path);
    }

  gint res = g_test_run ();
  g_object_unref (session);
  g_object_unref (server_subproc);
  return res;
}
