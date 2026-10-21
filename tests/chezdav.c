/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/* End-to-end test of the chezdav htdigest authentication: it launches
 * the real chezdav binary with an htdigest file and checks which
 * credentials are accepted over HTTP.
 *
 * The htdigest file contains two users of the server realm and one
 * user of another realm, to exercise the file parsing too.
 *
 * The TLS support is tested the same way, with the self-signed
 * certificate of tests/tls: the share is served over HTTPS, with
 * the digest authentication on top. */

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
/* the TLS test servers listen on their own ports */
#define TLS_PORT (PORT + 2)
#define TLS_COMBINED_PORT (PORT + 3)
#define SERVER_URI "http://localhost:" G_STRINGIFY (PORT) "/"
#define REALM "test"

static gchar *share_dir;
static gchar *work_dir;
static gchar *htdigest_file;
static GSubprocess *server;
static gboolean server_exited;
#ifdef G_OS_UNIX
static pid_t server_pid;
/* the TLS test servers, killed on fatal signals like the main one,
 * so that they do not keep holding their ports */
static pid_t tls_server_pids[4];
static guint n_tls_servers;
#endif

static const gchar test_put_data[] = "test_put: test data";

typedef struct _AuthCase
{
  const gchar *username; /* NULL: don't send credentials */
  const gchar *password;
  const gchar *method;
  const gchar *path;
  guint status_code;
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
fatal_signal_cb (gint sig)
{
  if (server_pid > 0)
    kill (server_pid, SIGKILL);

  /* the TLS test servers hold their own ports */
  for (guint i = 0; i < n_tls_servers; i++)
    kill (tls_server_pids[i], SIGKILL);

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

  server = g_subprocess_newv ((const gchar *const *) args->pdata,
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
authenticate_cb (SoupMessage *msg, SoupAuth *auth, gboolean retrying, gpointer user_data)
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
  g_assert_nonnull (in);

  g_assert_cmpint (soup_message_get_status (msg), ==, test->status_code);

  /* read the response body to completion, to finish the exchange */
  while (g_input_stream_read (in, buf, sizeof (buf), NULL, &error) > 0)
    ;
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

/* run chezdav with the given arguments, waiting for it to exit and
 * returning its exit status, with the output it produced */
static gint
run_chezdav (const gchar *const *args, gchar **stdout_str, gchar **stderr_str)
{
  const gchar *chezdav = g_getenv ("CHEZDAV");
  g_autoptr(GPtrArray) argv = NULL;
  g_autoptr(GSubprocess) proc = NULL;
  g_autoptr(GError) error = NULL;
  guint i;

  if (!chezdav)
    chezdav = "bin/chezdav";

  argv = g_ptr_array_new ();
  g_ptr_array_add (argv, (gchar *) chezdav);
  for (i = 0; args[i]; i++)
    g_ptr_array_add (argv, (gchar *) args[i]);
  if (g_getenv ("CHEZDAV_TEST_NO_MDNS"))
    g_ptr_array_add (argv, (gchar *) "--no-mdns");
  g_ptr_array_add (argv, NULL);

  proc = g_subprocess_newv ((const gchar *const *) argv->pdata,
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                            &error);
  g_assert_no_error (error);

  g_subprocess_communicate_utf8 (proc, NULL, NULL, stdout_str, stderr_str, &error);
  g_assert_no_error (error);

  return g_subprocess_get_exit_status (proc);
}

static void
test_cli (void)
{
  /* --version prints the version and exits successfully */
  {
    g_autofree gchar *out = NULL;
    g_autofree gchar *err = NULL;

    g_assert_cmpint (run_chezdav ((const gchar *[]){ "--version", NULL }, &out, &err), ==, 0);
    g_assert_nonnull (strstr (out, "phodav"));
  }

  /* the argument and option handling errors */
  const struct
  {
    const gchar *args[6];
    const gchar *message;
  } cases[] = {
    { { "--bogus", NULL }, "Option parsing failed" },
    { { "extra-arg", NULL }, "Unsupported extra arguments" },
    { { "--local", "--public", NULL }, "mutually exclusive" },
    { { "--max-upload", "-1", NULL }, "must not be negative" },
    { { "--local", "--port", "8099", "--htdigest", "/nonexistent", NULL }, "Failed to open htdigest" },
    { { "--tls-key", "key.pem", NULL }, "requires --tls-cert" },
    { { "--tls-cert", "/nonexistent", NULL }, "Failed to load the certificate" },
    /* the port of the running test server is already in use */
    { { "--local", "--port", G_STRINGIFY (PORT), "--path", ".", NULL }, "Listen failed" },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree gchar *out = NULL;
      g_autofree gchar *err = NULL;

      g_assert_cmpint (run_chezdav (cases[i].args, &out, &err), ==, 1);
      g_assert_nonnull (strstr (err, cases[i].message));
    }
}

/* the option defaults: serving the home directory on all interfaces,
 * with a realm derived from the user name */
static void
test_defaults (void)
{
  const gchar *chezdav = g_getenv ("CHEZDAV");
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) proc = NULL;
  g_autoptr(GSocketClient) client = NULL;
  g_autoptr(GSocketConnection) connection = NULL;
  g_autoptr(SoupSession) session = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autofree gchar *home = NULL;
  g_autofree gchar *port_str = NULL;
  g_autofree gchar *uri = NULL;
  g_autoptr(GError) error = NULL;
  guint i;
  guint16 port = PORT + 1;

  if (!chezdav)
    chezdav = "bin/chezdav";

  /* serve a temporary home directory, so that the test does not
   * depend on the contents of the real one */
  home = g_dir_make_tmp ("chezdav-home-XXXXXX", &error);
  g_assert_no_error (error);
  create_test_file (home, "test.txt");

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
  g_subprocess_launcher_setenv (launcher, "HOME", home, TRUE);

  {
    g_autoptr(GPtrArray) argv = g_ptr_array_new ();

    port_str = g_strdup_printf ("%u", port);
    g_ptr_array_add (argv, (gchar *) chezdav);
    g_ptr_array_add (argv, (gchar *) "--port");
    g_ptr_array_add (argv, port_str);
    if (g_getenv ("CHEZDAV_TEST_NO_MDNS"))
      g_ptr_array_add (argv, (gchar *) "--no-mdns");
    g_ptr_array_add (argv, NULL);

    proc = g_subprocess_launcher_spawnv (launcher, (const gchar *const *) argv->pdata, &error);
    g_assert_no_error (error);
  }

  /* wait for the server to listen */
  client = g_socket_client_new ();
  for (i = 0; i < 100; i++)
    {
      connection = g_socket_client_connect_to_host (client, "127.0.0.1", port, NULL, NULL);
      if (connection)
        break;
      g_usleep (100 * 1000);
    }
  g_assert_nonnull (connection);

  /* the home directory is served */
  session = soup_session_new ();
  uri = g_strdup_printf ("http://127.0.0.1:%u/", port);
  msg = soup_message_new ("PROPFIND", uri);
  soup_message_headers_append (soup_message_get_request_headers (msg), "Depth", "0");
  {
    g_autoptr(GBytes) body = soup_session_send_and_read (session, msg, NULL, &error);

    g_assert_no_error (error);
    g_assert_nonnull (body);
    g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  }

  /* SIGINT shuts the server down cleanly */
#ifdef G_OS_UNIX
  g_subprocess_send_signal (proc, SIGINT);
#else
  g_subprocess_force_exit (proc);
#endif
  g_subprocess_wait (proc, NULL, &error);
  g_assert_no_error (error);
#ifdef G_OS_UNIX
  g_assert_true (g_subprocess_get_if_exited (proc));
  g_assert_cmpint (g_subprocess_get_exit_status (proc), ==, 0);
#endif

  delete_dir (home);
}

/* stop a chezdav process, printing the output it produced for
 * debugging purposes */
static void
stop_chezdav_process (GSubprocess *proc)
{
  g_autofree gchar *output = NULL;
  g_autoptr(GError) error = NULL;

#ifdef G_OS_UNIX
  g_subprocess_send_signal (proc, SIGINT);
#else
  g_subprocess_force_exit (proc);
#endif
  g_subprocess_communicate_utf8 (proc, NULL, NULL, &output, NULL, &error);

  if (error)
    g_printerr ("Failed to wait for chezdav: %s\n", error->message);
  else if (output && *output)
    g_printerr ("chezdav output:\n%s", output);
}

/* run a chezdav server sharing the test directory over HTTPS with
 * digest authentication, using @cert (and the private key @key when
 * it is not included in the certificate file), and wait for it to
 * listen; returns NULL after printing an error */
static gboolean tls_server_exited;
static GSubprocess *tls_wait_server; /* borrowed */

static void
tls_server_wait_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  /* the previous server may exit while this one is started: only
   * report the exit of the server being waited for */
  if (user_data == tls_wait_server)
    tls_server_exited = TRUE;
}

static GSubprocess *
launch_tls_server (guint port, const gchar *cert, const gchar *key)
{
  const gchar *chezdav = g_getenv ("CHEZDAV");
  g_autoptr(GPtrArray) args = NULL;
  g_autoptr(GSocketClient) client = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *port_str = NULL;
  GSubprocess *proc;
  guint i;

  if (!chezdav)
    chezdav = "bin/chezdav";

  port_str = g_strdup_printf ("%u", port);
  args = g_ptr_array_new ();
  g_ptr_array_add (args, (gchar *) chezdav);
  g_ptr_array_add (args, (gchar *) "--local");
  g_ptr_array_add (args, (gchar *) "--port");
  g_ptr_array_add (args, port_str);
  g_ptr_array_add (args, (gchar *) "--path");
  g_ptr_array_add (args, share_dir);
  g_ptr_array_add (args, (gchar *) "--htdigest");
  g_ptr_array_add (args, htdigest_file);
  g_ptr_array_add (args, (gchar *) "--realm");
  g_ptr_array_add (args, (gchar *) REALM);
  g_ptr_array_add (args, (gchar *) "--tls-cert");
  g_ptr_array_add (args, (gchar *) cert);
  if (key)
    {
      g_ptr_array_add (args, (gchar *) "--tls-key");
      g_ptr_array_add (args, (gchar *) key);
    }
  if (g_getenv ("CHEZDAV_TEST_NO_MDNS"))
    g_ptr_array_add (args, (gchar *) "--no-mdns");
  g_ptr_array_add (args, NULL);

  proc = g_subprocess_newv ((const gchar *const *) args->pdata,
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
                            &error);
  if (error)
    {
      g_printerr ("Failed to launch chezdav: %s\n", error->message);
      return NULL;
    }

#ifdef G_OS_UNIX
  {
    const gchar *identifier = g_subprocess_get_identifier (proc);

    if (identifier && n_tls_servers < G_N_ELEMENTS (tls_server_pids))
      tls_server_pids[n_tls_servers++] = (pid_t) g_ascii_strtoll (identifier, NULL, 10);
  }
#endif

  tls_wait_server = proc;
  tls_server_exited = FALSE;
  g_subprocess_wait_async (proc, NULL, tls_server_wait_cb, proc);

  /* wait for the listener: a plain TCP connection succeeds without
   * a TLS handshake */
  client = g_socket_client_new ();
  for (i = 0; i < 100; i++)
    {
      g_autoptr(GSocketConnection) connection =
                  g_socket_client_connect_to_host (client, "127.0.0.1", port, NULL, NULL);

      if (connection)
        {
          /* make sure the listener is our server, and not some leftover
           * process that our server failed to replace (port in use) */
          g_usleep (200 * 1000);
          while (g_main_context_pending (NULL))
            g_main_context_iteration (NULL, FALSE);

          if (!tls_server_exited)
            return proc;

          g_printerr ("chezdav exited early with status %d, is something else listening on port %u?\n",
                      g_subprocess_get_exit_status (proc), port);
          break;
        }

      /* let the wait callback run, to notice an early exit */
      while (g_main_context_pending (NULL))
        g_main_context_iteration (NULL, FALSE);

      if (tls_server_exited)
        {
          g_printerr ("chezdav exited early with status %d\n",
                      g_subprocess_get_exit_status (proc));
          break;
        }

      g_usleep (100 * 1000);
    }

  g_printerr ("chezdav did not start listening over TLS\n");
  stop_chezdav_process (proc);
  g_object_unref (proc);
  return NULL;
}

/* send a GET request for the test file over HTTPS, with or without
 * credentials, using a session which validates the server against
 * @database; returns the response status, and its body in @body when
 * not NULL */
static guint
https_get_test_file (SoupSession *session, guint port, gboolean with_credentials, gchar **body, GError **error)
{
  const AuthCase auth = { "user1", "password1", NULL, NULL, 0 };
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autofree gchar *uri = NULL;

  uri = g_strdup_printf ("https://localhost:%u/test.txt", port);
  msg = soup_message_new ("GET", uri);

  if (with_credentials)
    g_signal_connect (msg, "authenticate", G_CALLBACK (authenticate_cb), (gpointer) &auth);

  bytes = soup_session_send_and_read (session, msg, NULL, error);
  if (!bytes)
    return 0;

  if (body)
    *body = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));

  return soup_message_get_status (msg);
}

/* the share is served over HTTPS: the certificate is validated, and
 * the digest authentication applies on top of it */
static void
test_tls (void)
{
  const gchar *cert = g_getenv ("CHEZDAV_TEST_CERT");
  const gchar *key = g_getenv ("CHEZDAV_TEST_KEY");
  g_autoptr(GSubprocess) proc = NULL;
  g_autoptr(GTlsDatabase) database = NULL;
  g_autoptr(SoupSession) session = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *uri = NULL;
  g_autofree gchar *body = NULL;

  if (!(cert && key && g_file_test (cert, G_FILE_TEST_EXISTS) && g_file_test (key, G_FILE_TEST_EXISTS)))
    {
      g_test_skip ("the TLS test certificate is not available");
      return;
    }

  if (!g_tls_backend_supports_tls (g_tls_backend_get_default ()))
    {
      g_test_skip ("TLS support is not available");
      return;
    }

  /* a certificate file without its private key is refused up front */
  {
    g_autofree gchar *out = NULL;
    g_autofree gchar *err = NULL;

    g_assert_cmpint (run_chezdav ((const gchar *[]){ "--tls-cert", cert, NULL }, &out, &err), ==, 1);
    g_assert_nonnull (strstr (err, "contains no private key"));
  }

  proc = launch_tls_server (TLS_PORT, cert, key);
  g_assert_nonnull (proc);

  /* a stalled connection fails the request after a while, rather
   * than hanging the test */
  session = soup_session_new ();
  g_object_set (session, "timeout", 10, NULL);

  /* a plain HTTP request to the HTTPS listener is not served */
  uri = g_strdup_printf ("http://localhost:%u/", TLS_PORT);
  msg = soup_message_new ("GET", uri);
  {
    g_autoptr(GBytes) bytes = soup_session_send_and_read (session, msg, NULL, &error);

    /* the connection is refused rather than answered */
    if (!error)
      g_assert_cmpuint (soup_message_get_status (msg), >=, 400);
  }
  g_clear_error (&error);

  /* the server certificate is validated, with the test certificate
   * as the only trust anchor */
  database = g_tls_file_database_new (cert, &error);
  g_assert_no_error (error);

  g_clear_object (&session);
  session = soup_session_new ();
  g_object_set (session, "timeout", 10, "tls-database", database, NULL);

  /* the digest authentication applies over TLS too; a request which
   * cannot even complete its handshake means that the TLS stack of
   * this environment is unusable (some mingw builds under Wine for
   * example), which is not a phodav problem: skip the tests */
  {
    g_autoptr(GError) tls_error = NULL;
    guint status = https_get_test_file (session, TLS_PORT, FALSE, NULL, &tls_error);

    if (tls_error)
      {
        stop_chezdav_process (proc);
        g_test_skip_printf ("the TLS handshake failed: %s", tls_error->message);
        return;
      }

    g_assert_cmpuint (status, ==, SOUP_STATUS_UNAUTHORIZED);
  }

  /* with credentials, the file is served over TLS */
  {
    g_autoptr(GError) tls_error = NULL;

    g_assert_cmpuint (https_get_test_file (session, TLS_PORT, TRUE, &body, &tls_error), ==, SOUP_STATUS_OK);
    g_assert_no_error (tls_error);
  }
  g_assert_cmpstr (body, ==, "test data");

  stop_chezdav_process (proc);
}

/* the certificate and its private key may also be given in a single
 * PEM file */
static void
test_tls_combined (void)
{
  const gchar *cert = g_getenv ("CHEZDAV_TEST_CERT");
  const gchar *key = g_getenv ("CHEZDAV_TEST_KEY");
  g_autoptr(GSubprocess) proc = NULL;
  g_autoptr(GTlsDatabase) database = NULL;
  g_autoptr(SoupSession) session = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *combined = NULL;
  g_autofree gchar *body = NULL;

  if (!(cert && key && g_file_test (cert, G_FILE_TEST_EXISTS) && g_file_test (key, G_FILE_TEST_EXISTS)))
    {
      g_test_skip ("the TLS test certificate is not available");
      return;
    }

  if (!g_tls_backend_supports_tls (g_tls_backend_get_default ()))
    {
      g_test_skip ("TLS support is not available");
      return;
    }

  /* the certificate followed by its private key */
  combined = g_build_filename (work_dir, "combined.pem", NULL);
  {
    g_autofree gchar *cert_data = NULL;
    g_autofree gchar *key_data = NULL;
    g_autofree gchar *data = NULL;

    g_assert_true (g_file_get_contents (cert, &cert_data, NULL, NULL));
    g_assert_true (g_file_get_contents (key, &key_data, NULL, NULL));
    data = g_strconcat (cert_data, key_data, NULL);
    g_assert_true (g_file_set_contents (combined, data, -1, NULL));
  }

  proc = launch_tls_server (TLS_COMBINED_PORT, combined, NULL);
  g_assert_nonnull (proc);

  database = g_tls_file_database_new (cert, &error);
  g_assert_no_error (error);

  session = soup_session_new ();
  g_object_set (session, "timeout", 10, "tls-database", database, NULL);

  /* like in test_tls, skip when the environment cannot complete a
   * TLS handshake at all */
  {
    g_autoptr(GError) tls_error = NULL;
    guint status = https_get_test_file (session, TLS_COMBINED_PORT, FALSE, NULL, &tls_error);

    if (tls_error)
      {
        stop_chezdav_process (proc);
        g_test_skip_printf ("the TLS handshake failed: %s", tls_error->message);
        return;
      }

    g_assert_cmpuint (status, ==, SOUP_STATUS_UNAUTHORIZED);
  }

  {
    g_autoptr(GError) tls_error = NULL;

    g_assert_cmpuint (https_get_test_file (session, TLS_COMBINED_PORT, TRUE, &body, &tls_error), ==, SOUP_STATUS_OK);
    g_assert_no_error (tls_error);
  }
  g_assert_cmpstr (body, ==, "test data");

  stop_chezdav_process (proc);
}

gint
main (gint argc, gchar *argv[])
{
  g_autofree gchar *contents = NULL;
  gchar *line, *tmp;
  g_autoptr(GError) error = NULL;

  g_test_init (&argc, &argv, NULL);

  share_dir = g_dir_make_tmp ("chezdav-share-XXXXXX", &error);
  g_assert_no_error (error);
  work_dir = g_dir_make_tmp ("chezdav-test-XXXXXX", &error);
  g_assert_no_error (error);

  create_test_file (share_dir, "test.txt");

  /* user1 and user2 belong to the server realm, "other" to another one;
   * the first line is invalid and skipped with a warning */
  htdigest_file = g_build_filename (work_dir, "htdigest", NULL);
  line = htdigest_line ("user1", REALM, "password1");
  contents = g_strconcat ("invalid-line-without-colons\n", line, NULL);
  g_free (line);
  line = htdigest_line ("user2", REALM, "password2");
  tmp = g_strconcat (contents, line, NULL);
  g_free (line);
  g_free (contents);
  contents = tmp;
  line = htdigest_line ("other", "otherrealm", "password3");
  tmp = g_strconcat (contents, line, NULL);
  g_free (line);
  g_free (contents);
  contents = tmp;
  g_file_set_contents (htdigest_file, contents, -1, &error);
  g_assert_no_error (error);

#ifdef G_OS_UNIX
  install_fatal_signal_handlers ();
#endif
  launch_server (htdigest_file);
  wait_for_server ();

  /* the order matters: the PUT cases check that "put.txt" was created
   * or not by the previous requests */
  const AuthCase tests[] = {
    /* without credentials, nothing is accessible */
    { NULL, NULL, SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_UNAUTHORIZED },
    { NULL, NULL, SOUP_METHOD_PUT, "/put.txt", SOUP_STATUS_UNAUTHORIZED },

    /* the digest password must be checked */
    { "user1", "wrong", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_UNAUTHORIZED },
    { "user1", "wrong", SOUP_METHOD_PUT, "/put.txt", SOUP_STATUS_UNAUTHORIZED },

    /* unknown users are rejected */
    { "nobody", "password1", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_UNAUTHORIZED },

    /* users of another realm are not accepted */
    { "other", "password3", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_UNAUTHORIZED },

    /* valid credentials are accepted */
    { "user1", "password1", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_OK },
    /* the second entry of the htdigest file works too, on first attempt */
    { "user2", "password2", SOUP_METHOD_GET, "/test.txt", SOUP_STATUS_OK },
    { "user1", "password1", SOUP_METHOD_GET, "/non-existent", SOUP_STATUS_NOT_FOUND },
    { "user2", "password2", SOUP_METHOD_PUT, "/put.txt", SOUP_STATUS_CREATED },
  };

  for (guint i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      g_autofree gchar *test_path = g_strdup_printf ("/chezdav/%s-%s/%s%s",
                                                     tests[i].username ? tests[i].username : "anonymous",
                                                     tests[i].password ? tests[i].password : "",
                                                     tests[i].method, tests[i].path);
      g_test_add_data_func (test_path, &tests[i], test_chezdav);
    }

  gint res;

  g_test_add_func ("/chezdav/cli", test_cli);
  g_test_add_func ("/chezdav/defaults", test_defaults);
  g_test_add_func ("/chezdav/tls", test_tls);
  g_test_add_func ("/chezdav/tls-combined", test_tls_combined);

  res = g_test_run ();

  stop_server ();
  g_object_unref (server);

  delete_dir (share_dir);
  delete_dir (work_dir);
  g_free (share_dir);
  g_free (work_dir);
  g_free (htdigest_file);

  return res;
}
