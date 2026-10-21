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
#include "config.h"

#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n.h>

#include "libphodav/phodav.h"

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#ifdef WITH_AVAHI
#include "avahi-common.h"
#endif

G_GNUC_PRINTF (1, 2)
static void G_GNUC_NORETURN
my_error (const gchar *format, ...)
{
  va_list args;

  fprintf (stderr, "%s: ", PACKAGE_NAME);
  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);
  fputc ('\n', stderr);

  exit (1);
}

#ifdef G_OS_UNIX
static gboolean
sighup_received (gpointer user_data)
{
  GMainLoop *mainloop = user_data;

  g_message ("Signal received, leaving");
  g_main_loop_quit (mainloop);

  return G_SOURCE_CONTINUE;
}
#endif

static gchar *
get_realm (void)
{
  return g_strdup_printf ("%s\'s public share", g_get_user_name ());
}

/* returns a table of user -> digest for the given realm */
static GHashTable *
load_htdigest (const gchar *filename, const gchar *realm, GError **error)
{
  GHashTable *users;
  g_autofree gchar *contents = NULL;
  g_auto(GStrv) lines = NULL;
  guint i;

  if (!g_file_get_contents (filename, &contents, NULL, error))
    return NULL;

  users = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  lines = g_strsplit (contents, "\n", -1);
  for (i = 0; lines[i]; i++)
    {
      gchar *line = g_strchomp (lines[i]);
      g_auto(GStrv) strv = NULL;

      if (!*line)
        continue;

      /* user:realm:md5(user:realm:password) */
      strv = g_strsplit (line, ":", 3);
      if (!(strv[0] && strv[1] && strv[2]))
        g_warning ("Invalid htdigest line %u", i + 1);
      else if (g_strcmp0 (strv[1], realm) == 0)
        g_hash_table_insert (users, g_strdup (strv[0]), g_strdup (strv[2]));
    }

  return users;
}

static gchar *
digest_auth_callback (SoupAuthDomain *auth_domain, SoupServerMessage *msg, const gchar *username, gpointer data)
{
  GHashTable *users = data;

  /* libsoup checks the client response against it */
  return g_strdup (g_hash_table_lookup (users, username));
}

/* Some TLS backends (GnuTLS) accept a certificate file without a
 * private key at load time, while the server could then never
 * complete a handshake; report the problem up front instead. */
static void
check_certificate_has_key (const gchar *filename)
{
  g_autofree gchar *contents = NULL;

  if (!g_file_get_contents (filename, &contents, NULL, NULL))
    return; /* the loading reports the reason */

  /* matches every private key PEM header, encrypted or not */
  if (strstr (contents, "PRIVATE KEY-----") == NULL)
    my_error (_ ("--tls-cert contains no private key, pass it with --tls-key\n"));
}

gint
main (gint argc, gchar *argv[])
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GOptionContext) context = NULL;
  /* the parsed string and filename options are owned copies */
  g_autofree gchar *path = NULL;
  g_autofree gchar *realm = NULL;
  g_autofree gchar *htdigest = NULL;
  /* the TLS options: tls_cert is NULL when the server is plain HTTP;
   * tls_key is only set when the private key is in a separate file */
  g_autofree gchar *tls_cert = NULL;
  g_autofree gchar *tls_key = NULL;
  g_autoptr(GMainLoop) mainloop = NULL;

  gint version = 0;

  /* the command line options, parsed in place by GOption: they are
   * plain locals of main(), which outlives the option context */
  gboolean verbose = FALSE;
  gboolean readonly = FALSE;
  gboolean local = FALSE;
  gboolean public = FALSE;
#ifdef WITH_AVAHI
  gboolean nomdns = FALSE;
#endif
  gint port = 8080;
  gint64 max_upload = 0;

  g_autoptr(PhodavServer) dav = NULL;

  GOptionEntry entries[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &version, N_ ("Print program version"), NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, N_ ("Be verbose"), NULL },
    { "port", 'p', 0, G_OPTION_ARG_INT, &port, N_ ("Port to listen to"), NULL },
    { "local", 0, 0, G_OPTION_ARG_NONE, &local, N_ ("Listen on loopback only"), NULL },
    { "public", 0, 0, G_OPTION_ARG_NONE, &public, N_ ("Listen on all interfaces"), NULL },
    { "path", 'P', 0, G_OPTION_ARG_FILENAME, &path, N_ ("Path to export"), NULL },
    { "htdigest", 'd', 0, G_OPTION_ARG_FILENAME, &htdigest, N_ ("Path to htdigest file"), NULL },
    { "realm", 0, 0, G_OPTION_ARG_STRING, &realm, N_ ("DIGEST realm"), NULL },
    { "readonly", 'r', 0, G_OPTION_ARG_NONE, &readonly, N_ ("Read-only access"), NULL },
    { "max-upload", 0, 0, G_OPTION_ARG_INT64, &max_upload, N_ ("Refuse uploads over SIZE bytes (0 for no limit)"), N_ ("SIZE") },
    { "tls-cert", 0, 0, G_OPTION_ARG_FILENAME, &tls_cert, N_ ("Serve HTTPS, with the TLS certificate in CERT (which must include the private key, unless --tls-key is given)"), N_ ("CERT") },
    { "tls-key", 0, 0, G_OPTION_ARG_FILENAME, &tls_key, N_ ("Path to the TLS private key, when it is not in the certificate file"), N_ ("KEY") },
#ifdef WITH_AVAHI
    { "no-mdns", 0, 0, G_OPTION_ARG_NONE, &nomdns, N_ ("Skip mDNS service announcement"), NULL },
#endif
    { NULL }
  };

  setlocale (LC_ALL, "");
  textdomain (GETTEXT_PACKAGE);
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  g_set_prgname ("chezdav");

  context = g_option_context_new (_ ("- simple WebDAV server"));
  g_autofree gchar *s = g_strdup_printf (_ ("Report bugs to <%s>"), PACKAGE_BUGREPORT);
  g_option_context_set_description (context, s);

  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    my_error (_ ("Option parsing failed: %s\n"), error->message);

  /* be verbose: show the debug messages of the server */
  if (verbose)
    g_setenv ("G_MESSAGES_DEBUG", "all", FALSE);

  if (argc != 1)
    my_error (_ ("Unsupported extra arguments: %s ...\n"), argv[1]);

  if (version)
    {
      printf (PACKAGE_STRING "\n");
      return 0;
    }

  if (local && public)
    my_error (_ ("--local and --public are mutually exclusive\n"));

  if (max_upload < 0)
    my_error (_ ("--max-upload must not be negative\n"));

  if (tls_key && !tls_cert)
    my_error (_ ("--tls-key requires --tls-cert\n"));

  if (!local && !public)
    public = TRUE; /* the default */

  if (!path)
    path = g_strdup (g_get_home_dir ());

  if (!realm)
    realm = get_realm ();

  mainloop = g_main_loop_new (NULL, FALSE);

#ifdef G_OS_UNIX
  g_unix_signal_add (SIGINT, sighup_received, mainloop);
#endif

  if (tls_cert)
    {
      g_autoptr(GTlsCertificate) cert = NULL;
      g_autoptr(SoupServer) soup = NULL;

      /* the certificate may contain the private key, or it may be
       * given separately */
      if (tls_key)
        cert = g_tls_certificate_new_from_files (tls_cert, tls_key, &error);
      else
        {
          check_certificate_has_key (tls_cert);
          cert = g_tls_certificate_new_from_file (tls_cert, &error);
        }

      if (!cert)
        my_error (_ ("Failed to load the certificate: %s\n"), error->message);

      /* a SoupServer with the certificate is injected, so that the
       * shared folder is served over HTTPS */
      soup = soup_server_new ("server-header", "PhodavServer",
                              "tls-certificate", cert,
                              NULL);
      dav = g_object_new (PHODAV_TYPE_SERVER,
                          "server", soup,
                          "root", path,
                          NULL);
    }
  else
    dav = phodav_server_new (path);

  g_object_set (dav,
                "read-only", readonly,
                "max-upload", (guint64) max_upload,
                NULL);

  if (htdigest)
    {
      g_autoptr(SoupAuthDomain) auth = NULL;
      SoupServer *server;
      GHashTable *users;

      users = load_htdigest (htdigest, realm, &error);
      if (!users)
        my_error (_ ("Failed to open htdigest: %s\n"), error->message);

      auth = soup_auth_domain_digest_new ("realm", realm, NULL);
      soup_auth_domain_add_path (auth, "/");
      soup_auth_domain_digest_set_auth_callback (auth, digest_auth_callback,
                                                 users, (GDestroyNotify) g_hash_table_unref);

      server = phodav_server_get_soup_server (dav);
      soup_server_add_auth_domain (server, auth);
    }

#ifdef WITH_AVAHI
  g_autofree gchar *name = get_realm ();
  if (!nomdns && !avahi_client_start (name, port, local, tls_cert != NULL, &error))
    my_error (_ ("mDNS failed: %s\n"), error->message);
#endif

  SoupServer *server = phodav_server_get_soup_server (dav);
  SoupServerListenOptions listen_options = 0;
  gboolean listening;

  /* with a certificate, the share is served over HTTPS */
  if (tls_cert)
    listen_options |= SOUP_SERVER_LISTEN_HTTPS;

  /* public is set by default when local is not given */
  if (local)
    listening = soup_server_listen_local (server, port, listen_options, &error);
  else
    listening = soup_server_listen_all (server, port, listen_options, &error);

  if (!listening)
    my_error (_ ("Listen failed: %s\n"), error->message);

  g_main_loop_run (mainloop);

#ifdef WITH_AVAHI
  avahi_client_stop ();
#endif

  g_message ("Bye");

  return 0;
}
