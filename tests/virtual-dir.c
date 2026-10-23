/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <stdlib.h>

#include <glib.h>

#include "libphodav/phodav.h"

static SoupSession *session;
static gchar *server_uri;

static const gchar test_put_data[] = "test_put: test data";

typedef struct _TestCase
{
  const gchar *method;
  const gchar *path;
  guint status_code;
  const gchar *destination;
  const gchar *if_header;
} TestCase;

/* files that must never be created: the server root is ./phodav-virtual-root,
 * nothing may escape it, and nothing may be written without authentication */
static const gchar *escaped_files[] = {
  "escaped-copy.txt",
  "escaped-move.txt",
  "escaped-put.txt",
  "escaped-mkcol",
  "phodav-virtual-root/..%2fescaped-put.txt",
  "phodav-virtual-root/..%2Fescaped-put.txt",
  "phodav-virtual-root/protected/test-put.txt",
};

static void
check_no_escape (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (escaped_files); i++)
    g_assert_false (g_file_test (escaped_files[i], G_FILE_TEST_EXISTS));
}

static void
test_generic (gconstpointer data)
{
  const TestCase *test = data;
  g_autofree gchar *uri = g_build_path ("/", server_uri, test->path, NULL);
  g_autoptr(SoupMessage) msg = soup_message_new (test->method, uri);

  if (test->method == SOUP_METHOD_COPY ||
      test->method == SOUP_METHOD_MOVE)
    {
      g_autofree gchar *dest_uri = g_build_path ("/", server_uri, test->destination, NULL);
      soup_message_headers_append (soup_message_get_request_headers (msg), "Destination", dest_uri);
    }

  if (test->if_header)
    soup_message_headers_append (soup_message_get_request_headers (msg), "If", test->if_header);

  if (test->method == SOUP_METHOD_PUT)
    {
      g_autoptr(GBytes) bytes = NULL;

      bytes = g_bytes_new_static (test_put_data, strlen (test_put_data));
      soup_message_set_request_body_from_bytes (msg, NULL, bytes);
    }

  GError *error = NULL;
  GInputStream *in = soup_session_send (session, msg, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpint (soup_message_get_status (msg), ==, test->status_code);
  check_no_escape ();

  g_object_unref (in);
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
  if (test->method == SOUP_METHOD_COPY ||
      test->method == SOUP_METHOD_MOVE)
    {
      gchar *dest = replace_char_dup (test->destination, '/', '\\');
      gchar *tmp = g_strconcat (test_path, "->", dest, NULL);
      g_free (dest);
      g_free (test_path);
      test_path = tmp;
    }
  return test_path;
}

/* PROPPATCH on the virtual directories: the root forwards dead
 * property removal to its real root, the other directories refuse
 * with a per-property error in the multistatus response */
static void
test_proppatch (void)
{
  const gchar *body = "<D:propertyupdate xmlns:D=\"DAV:\"><D:remove><D:prop>"
                      "<user.phodav-test/>"
                      "</D:prop></D:remove></D:propertyupdate>";
  g_autoptr(GBytes) bytes = g_bytes_new (body, strlen (body));

  struct
  {
    const gchar *path;
    const gchar *status; /* the per-property status in the response */
  } cases[] = {
    { "/", "200" },
    { "/virtual", "403" },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree gchar *uri = g_build_path ("/", server_uri, cases[i].path, NULL);
      g_autoptr(SoupMessage) msg = soup_message_new ("PROPPATCH", uri);
      g_autoptr(GError) error = NULL;
      GInputStream *in;

      soup_message_set_request_body_from_bytes (msg, NULL, bytes);
      in = soup_session_send (session, msg, NULL, &error);
      g_assert_no_error (error);
      g_assert_cmpint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);

      /* the per-property status is reported inside the response body */
      {
        gchar buf[4096];
        gssize n = g_input_stream_read (in, buf, sizeof (buf) - 1, NULL, &error);

        g_assert_no_error (error);
        g_assert_cmpint (n, >, 0);
        buf[n] = '\0';
        g_assert_nonnull (strstr (buf, cases[i].status));
      }
      g_object_unref (in);
    }
}

/* PROPFIND reports the validators of the virtual directories, so
 * that the clients can validate their cached listings of them */
static void
test_propfind_validators (void)
{
  const gchar *body = "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
                      "<D:getetag/><D:getlastmodified/>"
                      "</D:prop></D:propfind>";
  g_autoptr(GBytes) bytes = g_bytes_new (body, strlen (body));
  g_autofree gchar *uri = g_build_path ("/", server_uri, "virtual", NULL);
  g_autoptr(SoupMessage) msg = soup_message_new ("PROPFIND", uri);
  g_autoptr(GError) error = NULL;
  GInputStream *in;

  soup_message_headers_append (soup_message_get_request_headers (msg), "Depth", "0");
  soup_message_set_request_body_from_bytes (msg, NULL, bytes);
  in = soup_session_send (session, msg, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);

  {
    gchar buf[4096];
    gssize n = g_input_stream_read (in, buf, sizeof (buf) - 1, NULL, &error);

    g_assert_no_error (error);
    g_assert_cmpint (n, >, 0);
    buf[n] = '\0';

    /* both properties are answered, without any per-property error */
    g_assert_nonnull (strstr (buf, "<D:getetag>"));
    g_assert_nonnull (strstr (buf, "<D:getlastmodified>"));
    g_assert_null (strstr (buf, "404"));
  }
  g_object_unref (in);
}

#define LOCK_BODY                                                          \
  "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:exclusive/></D:lockscope>" \
  "<D:locktype><D:write/></D:locktype></D:lockinfo>"

/* send a LOCK or UNLOCK request for @path; @token is the Lock-Token
 * header of an UNLOCK, @lock_token receives the token of a LOCK */
static guint
send_lock_request (const gchar *method, const gchar *path, const gchar *token, gchar **lock_token)
{
  g_autoptr(GBytes) bytes = NULL;
  g_autofree gchar *uri = g_build_path ("/", server_uri, path, NULL);
  g_autoptr(SoupMessage) msg = soup_message_new (method, uri);
  g_autoptr(GError) error = NULL;
  GInputStream *in;
  gchar buf[4096];
  gssize n;

  if (g_strcmp0 (method, "LOCK") == 0)
    {
      bytes = g_bytes_new_static (LOCK_BODY, strlen (LOCK_BODY));
      soup_message_headers_append (soup_message_get_request_headers (msg),
                                   "Depth", "0");
      soup_message_set_request_body_from_bytes (msg, NULL, bytes);
    }
  else
    soup_message_headers_append (soup_message_get_request_headers (msg),
                                 "Lock-Token", token);

  in = soup_session_send (session, msg, NULL, &error);
  g_assert_no_error (error);

  while ((n = g_input_stream_read (in, buf, sizeof (buf), NULL, &error)) > 0)
    ;
  g_assert_no_error (error);
  g_object_unref (in);

  if (lock_token)
    g_set_str (lock_token, soup_message_headers_get_one (
                             soup_message_get_response_headers (msg), "Lock-Token"));

  return soup_message_get_status (msg);
}

/* the virtual directories do not implement GFile create(): LOCK must
 * not try to create an existing one, and must not fail on the missing
 * create() for an unmapped resource */
static void
test_lock (void)
{
  g_autofree gchar *token = NULL;

  g_assert_cmpint (send_lock_request ("LOCK", "virtual", NULL, &token),
                   ==, SOUP_STATUS_OK);
  g_assert_nonnull (token);

  /* a second exclusive lock conflicts with the first one */
  g_assert_cmpint (send_lock_request ("LOCK", "virtual", NULL, NULL),
                   ==, SOUP_STATUS_LOCKED);

  g_assert_cmpint (send_lock_request ("UNLOCK", "virtual", token, NULL),
                   ==, SOUP_STATUS_NO_CONTENT);

  /* a virtual child cannot be created: the lock is taken on the
   * resource as it is, rather than failing on the missing create() */
  g_assert_cmpint (send_lock_request ("LOCK", "virtual/missing", NULL, &token),
                   ==, SOUP_STATUS_OK);
  g_assert_nonnull (token);
  g_assert_cmpint (send_lock_request ("UNLOCK", "virtual/missing", token, NULL),
                   ==, SOUP_STATUS_NO_CONTENT);
}

gint
main (gint argc, gchar *argv[])
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) server_subproc = NULL;
  server_subproc = g_subprocess_new (
    G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error,
    "tests/virtual-dir-server", "--quit-on-stdin", NULL);

  if (error)
    {
      g_printerr ("Failed to launch virtual-dir-server: %s\n", error->message);
      return 1;
    }

  /* wait for the server to start */
  g_autoptr(GDataInputStream) stream = g_data_input_stream_new (g_subprocess_get_stdout_pipe (server_subproc));
  g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (stream), FALSE);
  gboolean server_started = FALSE;
  while (!server_started)
    {
      gchar *line = g_data_input_stream_read_line_utf8 (stream, NULL, NULL, &error);

      if (error)
        {
          g_printerr ("Failed to start virtual-dir-server: %s\n", error->message);
          return 1;
        }

      if (line == NULL)
        {
          g_printerr ("Failed to start virtual-dir-server\n");
          return 1;
        }

      /* the server prints "OK <port>" once it listens */
      if (g_str_has_prefix (line, "OK "))
        {
          server_uri = g_strdup_printf ("dav://127.0.0.1:%s/", line + 3);
          server_started = TRUE;
        }
      g_free (line);
    }
  g_clear_object (&stream);

  g_test_init (&argc, &argv, NULL);

  /* fail rather than hang if the server stops responding */
  session = soup_session_new_with_options ("timeout", 10, NULL);

  /* the order matters, some tests depend on the previous ones */
  const TestCase tests[] = {
    { SOUP_METHOD_GET, "/", SOUP_STATUS_OK },
    { SOUP_METHOD_GET, "/virtual", SOUP_STATUS_OK },
    { SOUP_METHOD_GET, "/non-existent", SOUP_STATUS_NOT_FOUND },
    { SOUP_METHOD_GET, "/virtual/non-existent", SOUP_STATUS_NOT_FOUND },
    { SOUP_METHOD_GET, "/virtual/real", SOUP_STATUS_OK },

    { SOUP_METHOD_MKCOL, "/A", SOUP_STATUS_CREATED },
    { SOUP_METHOD_MKCOL, "/virtual/B", SOUP_STATUS_FORBIDDEN },
    { SOUP_METHOD_MKCOL, "/virtual/real/B", SOUP_STATUS_CREATED },

    { SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_CREATED, "/test-copy.txt" },
    { SOUP_METHOD_COPY, "/virtual", SOUP_STATUS_FORBIDDEN, "/virtual-copy" },
    { SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_FORBIDDEN, "/virtual/test-copy.txt" },
    { SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_CREATED, "/virtual/real/test-copy.txt" },

    { SOUP_METHOD_PUT, "/test-put.txt", SOUP_STATUS_CREATED },
    { SOUP_METHOD_PUT, "/virtual/test-put.txt", SOUP_STATUS_INTERNAL_SERVER_ERROR },
    { SOUP_METHOD_PUT, "/virtual/real/test-put.txt", SOUP_STATUS_CREATED },

    { SOUP_METHOD_DELETE, "/A", SOUP_STATUS_NO_CONTENT },
    { SOUP_METHOD_DELETE, "/virtual/real/B", SOUP_STATUS_NO_CONTENT },
    { SOUP_METHOD_DELETE, "/virtual", SOUP_STATUS_FORBIDDEN },

    /* encoded slashes must not allow escaping the root */
    { SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_BAD_REQUEST, "/x%2f..%2f..%2fescaped-copy.txt" },
    { SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_BAD_REQUEST, "/..%2fescaped-copy.txt" },
    { SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_BAD_REQUEST, "/x%2F..%2F..%2Fescaped-copy.txt" },
    { SOUP_METHOD_COPY, "/test.txt", SOUP_STATUS_CREATED, "/%2e%2e/escaped-copy.txt" },
    { SOUP_METHOD_MOVE, "/test.txt", SOUP_STATUS_BAD_REQUEST, "/x%2f..%2f..%2fescaped-move.txt" },
    { SOUP_METHOD_PUT, "/..%2fescaped-put.txt", SOUP_STATUS_BAD_REQUEST },
    { SOUP_METHOD_MKCOL, "/..%2fescaped-mkcol", SOUP_STATUS_BAD_REQUEST },
    { SOUP_METHOD_GET, "/..%2fphodav-virtual-root%2ftest.txt", SOUP_STATUS_BAD_REQUEST },

    /* libsoup authentication must be enforced */
    { SOUP_METHOD_GET, "/protected", SOUP_STATUS_UNAUTHORIZED },
    { SOUP_METHOD_PUT, "/protected/test-put.txt", SOUP_STATUS_UNAUTHORIZED },

    /* invalid If headers must be rejected, without crashing or hanging the server */
    { SOUP_METHOD_DELETE, "/if-1", SOUP_STATUS_BAD_REQUEST, NULL, "(x)" },
    { SOUP_METHOD_DELETE, "/if-2", SOUP_STATUS_BAD_REQUEST, NULL, "(" },
    { SOUP_METHOD_DELETE, "/if-3", SOUP_STATUS_BAD_REQUEST, NULL, "(<opaquelocktoken:x>" },
    { SOUP_METHOD_DELETE, "/if-4", SOUP_STATUS_BAD_REQUEST, NULL, "<http://localhost/x" },
    { SOUP_METHOD_DELETE, "/if-5", SOUP_STATUS_BAD_REQUEST, NULL, "([\"etag" },
    { SOUP_METHOD_DELETE, "/if-6", SOUP_STATUS_BAD_REQUEST, NULL, "</a%2f..%2f..%2fx> ([\"etag\"])" },
    { SOUP_METHOD_DELETE, "/if-7", SOUP_STATUS_PRECONDITION_FAILED, NULL, "(<opaquelocktoken:x>)" },
    { SOUP_METHOD_DELETE, "/if-8", SOUP_STATUS_PRECONDITION_FAILED, NULL, "</if-8> (<opaquelocktoken:x>)" },
    { SOUP_METHOD_DELETE, "/if-9", SOUP_STATUS_PRECONDITION_FAILED, NULL, "<if-9> (<opaquelocktoken:x>)" },
    /* conditions that hold, the file doesn't exist */
    { SOUP_METHOD_DELETE, "/if-10", SOUP_STATUS_NOT_FOUND, NULL, "(Not <DAV:no-lock>)" },
    { SOUP_METHOD_DELETE, "/if-11", SOUP_STATUS_NOT_FOUND, NULL, "</if-11> (Not <DAV:no-lock>)" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      gchar *test_path = get_test_path (&tests[i]);
      g_test_add_data_func (test_path, &tests[i], test_generic);
      g_free (test_path);
    }

  g_test_add_func ("/virtual-dir/proppatch", test_proppatch);
  g_test_add_func ("/virtual-dir/propfind-validators", test_propfind_validators);
  g_test_add_func ("/virtual-dir/lock", test_lock);

  gint res = g_test_run ();
  g_object_unref (session);
  g_free (server_uri);
  return res;
}
