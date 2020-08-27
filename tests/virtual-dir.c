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
#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
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
      soup_message_headers_append (msg->request_headers, "Destination", dest_uri);
      g_free (dest_uri);
    }

  if (test->method == SOUP_METHOD_PUT)
    {
      soup_message_body_append (msg->request_body, SOUP_MEMORY_STATIC,
                                test_put_data, strlen (test_put_data));
    }

  GInputStream *in = soup_session_send (session, msg, NULL, NULL);

  g_assert_cmpint (msg->status_code, ==, test->status_code);

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
  pid_t child_pid = fork ();
  g_assert_cmpint (child_pid, !=, -1);
  if (child_pid == 0)
    {
      prctl(PR_SET_PDEATHSIG, SIGTERM);
      execl ("tests/virtual-dir-server", "virtual-dir-server", NULL);
      g_printerr ("Error launching virtual-dir-server\n");
      return 1;
    }

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
  return res;
}
