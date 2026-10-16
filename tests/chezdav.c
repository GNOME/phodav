/*
 * Copyright (C) 2026 Red Hat, Inc.
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

/* End-to-end test of the chezdav htdigest authentication: it launches
 * the real chezdav binary with an htdigest file and checks which
 * credentials are accepted over HTTP.
 *
 * The htdigest file contains two users of the server realm and one
 * user of another realm, to exercise the file parsing too. */

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <libsoup/soup.h>

#ifdef G_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

/* keep in sync with the virtual-dir tests, which listen on 8080 */
#define PORT 8081
#define SERVER_URI "http://localhost:" G_STRINGIFY (PORT) "/"
#define REALM "test"

static gchar *share_dir;
static gchar *work_dir;
static GSubprocess *server;
static gboolean server_exited;
#ifdef G_OS_UNIX
static pid_t server_pid;
#endif

static const gchar test_put_data[] = "test_put: test data";

typedef struct _AuthCase {
  const gchar *username; /* NULL: don't send credentials */
  const gchar *password;
  const gchar *method;
  const gchar *path;
  guint        status_code;
} AuthCase;

static void
create_test_file (const gchar *dir, const gchar *name)
{
  g_autofree gchar *path = g_build_filename (dir, name, NULL);
  g_autoptr(GFile) f = g_file_new_for_path (path);
  g_autoptr(GFileOutputStream) out = g_file_replace (f, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL);
  g_output_stream_printf (G_OUTPUT_STREAM (out), NULL, NULL, NULL, "test data");
}

/* Taken from tests/virtual-dir-server.c */
static void
delete_file_recursive (GFile *file)
{
  GFileEnumerator *e;
  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (e)
    {
      while (TRUE)
        {
          g_autoptr(GFileInfo) info = g_file_enumerator_next_file (e, NULL, NULL);
          if (!info)
            break;
          g_autoptr(GFile) del = g_file_get_child (file, g_file_info_get_name (info));
          delete_file_recursive (del);
        }

      g_file_enumerator_close (e, NULL, NULL);
      g_clear_object (&e);
    }

  g_file_delete (file, NULL, NULL);
}

static void
delete_dir (const gchar *path)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  delete_file_recursive (file);
}

/* returns an htdigest line "user:realm:md5(user:realm:password)" */
static gchar *
htdigest_line (const gchar *user, const gchar *realm, const gchar *password)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_MD5);
  g_autofree gchar *entry = g_strdup_printf ("%s:%s:%s", user, realm, password);

  g_checksum_update (checksum, (const guchar *) entry, -1);

  return g_strdup_printf ("%s:%s:%s\n", user, realm, g_checksum_get_string (checksum));
}

static void
server_wait_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  server_exited = TRUE;
}

#ifdef G_OS_UNIX
/* kill the server on fatal signals, so that a failing test doesn't
 * leave it running and holding the port */
static void
fatal_signal_cb (int sig)
{
  if (server_pid > 0)
    kill (server_pid, SIGKILL);

  signal (sig, SIG_DFL);
  raise (sig);
}

static void
install_fatal_signal_handlers (void)
{
  const gint signals[] = { SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGINT, SIGTERM };
  struct sigaction action;
  guint i;

  memset (&action, 0, sizeof (action));
  action.sa_handler = fatal_signal_cb;

  for (i = 0; i < G_N_ELEMENTS (signals); i++)
    sigaction (signals[i], &action, NULL);
}
#endif

static void
launch_server (const gchar *htdigest)
{
  const gchar *chezdav = g_getenv ("CHEZDAV");
  g_autoptr(GPtrArray) args = NULL;
  g_autoptr(GError) error = NULL;

  /* the binary is built in the build root, the current directory of the test */
  if (!chezdav)
    chezdav = "bin/chezdav";

  args = g_ptr_array_new ();
  g_ptr_array_add (args, (gchar *) chezdav);
  g_ptr_array_add (args, (gchar *) "--local");
  g_ptr_array_add (args, (gchar *) "--port");
  g_ptr_array_add (args, (gchar *) G_STRINGIFY (PORT));
  g_ptr_array_add (args, (gchar *) "--path");
  g_ptr_array_add (args, share_dir);
  g_ptr_array_add (args, (gchar *) "--realm");
  g_ptr_array_add (args, (gchar *) REALM);
  g_ptr_array_add (args, (gchar *) "--htdigest");
  g_ptr_array_add (args, (gchar *) htdigest);

  /* when chezdav is built with avahi, skip the mDNS announcement,
   * which requires a running avahi daemon */
  if (g_getenv ("CHEZDAV_TEST_NO_MDNS"))
    g_ptr_array_add (args, (gchar *) "--no-mdns");

  g_ptr_array_add (args, NULL);

  server = g_subprocess_newv ((const gchar * const *) args->pdata,
                              G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
                              &error);

  if (error)
    {
      g_printerr ("Failed to launch chezdav: %s\n", error->message);
      exit (1);
    }

#ifdef G_OS_UNIX
  server_pid = (pid_t) g_ascii_strtoll (g_subprocess_get_identifier (server), NULL, 10);
#endif
  g_subprocess_wait_async (server, NULL, server_wait_cb, NULL);
}

/* stop the server and wait for it, printing the output it produced
 * for debugging purposes */
static void
stop_server (void)
{
  g_autofree gchar *output = NULL;
  g_autoptr(GError) error = NULL;

  if (!server_exited)
    {
#ifdef G_OS_UNIX
      g_subprocess_send_signal (server, SIGINT);
#else
      g_subprocess_force_exit (server);
#endif
    }

  g_subprocess_communicate_utf8 (server, NULL, NULL, &output, NULL, &error);

  if (error)
    g_printerr ("Failed to wait for chezdav: %s\n", error->message);
  else if (output && *output)
    g_printerr ("chezdav output:\n%s", output);
}

static void
wait_for_server (void)
{
  g_autoptr(GSocketClient) client = g_socket_client_new ();
  guint i;

  for (i = 0; i < 100; i++)
    {
      g_autoptr(GSocketConnection) connection =
        g_socket_client_connect_to_host (client, "127.0.0.1", PORT, NULL, NULL);

      if (connection)
        {
          /* make sure the listener is our server, and not some leftover
           * process that our server failed to replace (port in use) */
          g_usleep (200 * 1000);
          while (g_main_context_pending (NULL))
            g_main_context_iteration (NULL, FALSE);

          if (!server_exited)
            return;

          g_printerr ("chezdav exited early with status %d, is something else listening on port %d?\n",
                      g_subprocess_get_exit_status (server), PORT);
          break;
        }

      /* let the wait callback run, to notice an early exit */
      while (g_main_context_pending (NULL))
        g_main_context_iteration (NULL, FALSE);

      if (server_exited)
        {
          g_printerr ("chezdav exited early with status %d\n",
                      g_subprocess_get_exit_status (server));
          break;
        }

      g_usleep (100 * 1000);
    }

  g_printerr ("chezdav did not start listening\n");
  stop_server ();
  exit (1);
}

static gboolean
authenticate_cb (SoupMessage *msg, SoupAuth *auth, gboolean retrying,
                 gpointer user_data)
{
  const AuthCase *test = user_data;

  /* no credentials available, or they were rejected already */
  if (retrying || !test->username)
    return FALSE;

  soup_auth_authenticate (auth, test->username, test->password);
  return FALSE;
}

/* the server replies to a PUT before its body is streamed to the file,
 * which is renamed into place only once the transfer is complete */
static gboolean
wait_for_file (const gchar *path)
{
  guint i;

  for (i = 0; i < 500; i++)
    {
      if (g_file_test (path, G_FILE_TEST_EXISTS))
        return TRUE;

      g_usleep (10 * 1000);
    }

  return FALSE;
}

static void
set_request_body (SoupMessage *msg)
{
  g_autoptr(GBytes) bytes = g_bytes_new_static (test_put_data, strlen (test_put_data));

  soup_message_set_request_body_from_bytes (msg, NULL, bytes);
}

static void
msg_restarted_cb (SoupMessage *msg, gpointer user_data)
{
  /* the request body must be set again when a message is restarted */
  set_request_body (msg);
}

static void
test_chezdav (gconstpointer data)
{
  const AuthCase *test = data;
  g_autoptr(SoupSession) session = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GError) error = NULL;
  gchar buf[4096];
  g_autofree gchar *uri = NULL;

  uri = g_build_path ("/", SERVER_URI, test->path, NULL);
  msg = soup_message_new (test->method, uri);
  g_assert_nonnull (msg);

  g_signal_connect (msg, "authenticate", G_CALLBACK (authenticate_cb), (gpointer) test);

  if (test->method == SOUP_METHOD_PUT)
    {
      set_request_body (msg);
      g_signal_connect (msg, "restarted", G_CALLBACK (msg_restarted_cb), NULL);
    }

  session = soup_session_new ();
  in = soup_session_send (session, msg, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpint (soup_message_get_status (msg), ==, test->status_code);

  /* read the response body to completion, to finish the exchange */
  while (g_input_stream_read (in, buf, sizeof (buf), NULL, &error) > 0);
  g_assert_no_error (error);

  if (test->method == SOUP_METHOD_PUT)
    {
      /* the file must have been created if and only if the PUT succeeded */
      g_autofree gchar *local = g_build_filename (share_dir, "put.txt", NULL);
      if (test->status_code == SOUP_STATUS_CREATED)
        {
          g_autofree gchar *contents = NULL;

          g_assert_true (wait_for_file (local));
          g_assert_true (g_file_get_contents (local, &contents, NULL, NULL));
          g_assert_cmpstr (contents, ==, test_put_data);
        }
      else
        g_assert_false (g_file_test (local, G_FILE_TEST_EXISTS));
    }
}

gint
main (gint argc, gchar *argv[])
{
  g_autofree gchar *htdigest = NULL;
  gchar *contents, *line;
  g_autoptr(GError) error = NULL;

  g_test_init (&argc, &argv, NULL);

  share_dir = g_dir_make_tmp ("chezdav-share-XXXXXX", &error);
  g_assert_no_error (error);
  work_dir = g_dir_make_tmp ("chezdav-test-XXXXXX", &error);
  g_assert_no_error (error);

  create_test_file (share_dir, "test.txt");

  /* user1 and user2 belong to the server realm, "other" to another one */
  htdigest = g_build_filename (work_dir, "htdigest", NULL);
  line = htdigest_line ("user1", REALM, "password1");
  contents = line;
  line = htdigest_line ("user2", REALM, "password2");
  contents = g_strconcat (contents, line, NULL);
  g_free (line);
  line = htdigest_line ("other", "otherrealm", "password3");
  contents = g_strconcat (contents, line, NULL);
  g_free (line);
  g_file_set_contents (htdigest, contents, -1, &error);
  g_assert_no_error (error);
  g_free (contents);

#ifdef G_OS_UNIX
  install_fatal_signal_handlers ();
#endif
  launch_server (htdigest);
  wait_for_server ();

  /* the order matters: the PUT cases check that "put.txt" was created
   * or not by the previous requests */
  const AuthCase tests[] = {
    /* without credentials, nothing is accessible */
    {NULL, NULL, SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_UNAUTHORIZED},
    {NULL, NULL, SOUP_METHOD_PUT, "/put.txt", SOUP_STATUS_UNAUTHORIZED},

    /* the digest password must be checked */
    {"user1", "wrong", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_UNAUTHORIZED},
    {"user1", "wrong", SOUP_METHOD_PUT, "/put.txt", SOUP_STATUS_UNAUTHORIZED},

    /* unknown users are rejected */
    {"nobody", "password1", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_UNAUTHORIZED},

    /* users of another realm are not accepted */
    {"other", "password3", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_UNAUTHORIZED},

    /* valid credentials are accepted */
    {"user1", "password1", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_OK},
    /* the second entry of the htdigest file works too, on first attempt */
    {"user2", "password2", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_OK},
    {"user1", "password1", SOUP_METHOD_GET, "/non-existent", SOUP_STATUS_NOT_FOUND},
    {"user2", "password2", SOUP_METHOD_PUT, "/put.txt", SOUP_STATUS_CREATED},
  };

  for (guint i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      g_autofree gchar *test_path = g_strdup_printf ("/chezdav/%s-%s/%s%s",
                                          tests[i].username ? tests[i].username : "anonymous",
                                          tests[i].password ? tests[i].password : "",
                                          tests[i].method, tests[i].path);
      g_test_add_data_func (test_path, &tests[i], test_chezdav);
    }

  gint res = g_test_run ();

  stop_server ();
  g_object_unref (server);

  delete_dir (share_dir);
  delete_dir (work_dir);
  g_free (share_dir);
  g_free (work_dir);

  return res;
}
