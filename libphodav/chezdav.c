/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2; -*- */
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

#include <stdlib.h>
#include <locale.h>
#include <glib/gi18n.h>
#include <glib.h>
#include <glib/gprintf.h>
#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#ifdef WITH_AVAHI
#include <avahi-gobject/ga-client.h>
#include <avahi-gobject/ga-entry-group.h>

static GaClient *mdns_client;
static GaEntryGroup *mdns_group;
static GaEntryGroupService *mdns_service;
#endif

#include "libphodav/phodav.h"

static PhodavServer *dav;
static gint verbose;
static gint readonly;
static gint port = 8080;
static gint local = 0;
static gint public = 0;

G_GNUC_PRINTF (1, 2) static void
my_error (const gchar *format, ...)
{
  va_list args;

  g_fprintf (stderr, PACKAGE_NAME ": ");
  va_start (args, format);
  g_vfprintf (stderr, format, args);
  va_end (args);
  g_fprintf (stderr, "\n");

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

#ifdef WITH_AVAHI
static void
mdns_register_service (void)
{
  GError *error = NULL;
  gchar *name = NULL;

  if (!mdns_group)
    {
      mdns_group = ga_entry_group_new ();

      if (!ga_entry_group_attach (mdns_group, mdns_client, &error))
        {
          g_warning ("Could not attach MDNS group to client: %s", error->message);
          g_clear_error (&error);
          return;
        }
    }

  name = get_realm ();
  mdns_service = ga_entry_group_add_service (mdns_group,
                                             name, "_webdav._tcp",
                                             port, &error,
                                             NULL);
  if (!mdns_service)
    {
      g_warning ("Could not create service: %s", error->message);
      g_clear_error (&error);
      goto end;
    }

  ga_entry_group_service_freeze (mdns_service);
  if (!ga_entry_group_service_set (mdns_service, "u", "", &error) ||
      !ga_entry_group_service_set (mdns_service, "p", "", &error) ||
      !ga_entry_group_service_set (mdns_service, "path", "/", &error) ||
      !ga_entry_group_service_thaw (mdns_service, &error))
    {
      g_warning ("Could not update TXT: %s", error->message);
      g_clear_error (&error);
    }

  if (!ga_entry_group_commit (mdns_group, &error))
    {
      g_warning ("Could not announce MDNS service: %s", error->message);
      g_clear_error (&error);
    }

end:
  g_free (name);
}

static void
mdns_state_changed (GaClient *client, GaClientState state, gpointer user_data)
{
  switch (state)
    {
    case GA_CLIENT_STATE_FAILURE:
      g_warning ("MDNS client state failure");
      break;

    case GA_CLIENT_STATE_S_RUNNING:
      g_debug ("MDNS client found server running");
      mdns_register_service ();
      break;

    case GA_CLIENT_STATE_S_COLLISION:
    case GA_CLIENT_STATE_S_REGISTERING:
      g_message ("MDNS collision");
      if (mdns_group)
        {
          ga_entry_group_reset (mdns_group, NULL);
          mdns_service = 0;
        }
      break;

    default:
      // Do nothing
      break;
    }
}
#endif // WITH_AVAHI

gchar *htdigest = NULL;

static gchar *
digest_auth_callback (SoupAuthDomain *auth_domain, SoupMessage *msg,
                      const char *username, gpointer data)
{
  gchar *digest = NULL;;
  gchar *line = NULL;
  gchar *eol = NULL;

  for (line = htdigest; line && *line; line = eol ? eol + 1 : NULL)
    {
      gchar **strv = g_strsplit (line, ":", -1);
      eol = strchr (line, '\n');
      if (eol)
        *eol = '\0';

      if (!(strv[0] && strv[1] && strv[2])) {
        g_warn_if_reached ();
      } else if (g_strcmp0 (strv[0], username) == 0)
        digest = g_strdup (strv[2]);

      g_strfreev (strv);

      if (digest)
        break;
    }

  return digest;
}

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context;
  const gchar *path = NULL;
  GMainLoop *mainloop = NULL;

  int version = 0;
  GOptionEntry entries[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &version, N_ ("Print program version"), NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, N_ ("Be verbose"), NULL },
    { "port", 'p', 0, G_OPTION_ARG_INT, &port, N_ ("Port to listen to"), NULL },
    { "local", 0, 0, G_OPTION_ARG_NONE, &local, N_ ("Listen on loopback only"), NULL },
    { "public", 0, 0, G_OPTION_ARG_NONE, &public, N_ ("Listen on all interfaces"), NULL },
    { "path", 'P', 0, G_OPTION_ARG_FILENAME, &path, N_ ("Path to export"), NULL },
    { "htdigest", 'd', 0, G_OPTION_ARG_FILENAME, &htdigest, N_ ("Path to htdigest file"), NULL },
    { "readonly", 'r', 0, G_OPTION_ARG_NONE, &readonly, N_ ("Read-only access"), NULL },
    { NULL }
  };

  setlocale (LC_ALL, "");
  textdomain (GETTEXT_PACKAGE);
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  g_set_prgname ("chezdav");

  context = g_option_context_new (_ ("- simple WebDAV server"));
  gchar *s = g_strdup_printf (_ ("Report bugs to <%s>"), PACKAGE_BUGREPORT);
  g_option_context_set_description (context, s);
  g_free (s);

  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    my_error (_ ("Option parsing failed: %s\n"), error->message);
  g_option_context_free (context);

  if (argc != 1)
    my_error (_ ("Unsupported extra arguments: %s ...\n"), argv[1]);

  if (version)
    {
      g_printf (PACKAGE_STRING "\n");
      return 0;
    }

  if (local && public)
    my_error (_ ("--local and --public are mutually exclusive\n"));

  if (!local && !public)
    public = 1; // default

  if (!path)
      path = g_get_home_dir ();

  mainloop = g_main_loop_new (NULL, FALSE);

#ifdef G_OS_UNIX
  g_unix_signal_add (SIGINT, sighup_received, mainloop);
#endif

  dav = phodav_server_new (path);
  g_object_set (dav, "read-only", readonly, NULL);

  if (htdigest)
    {
      SoupAuthDomain *auth;
      gchar *realm;
      SoupServer *server;

      if (!g_file_get_contents (htdigest, &htdigest, NULL, &error))
        my_error (_ ("Failed to open htdigest: %s\n"), error->message);

      realm = get_realm ();
      auth = soup_auth_domain_digest_new (SOUP_AUTH_DOMAIN_REALM, realm,
                                          SOUP_AUTH_DOMAIN_ADD_PATH, "/",
                                          SOUP_AUTH_DOMAIN_DIGEST_AUTH_CALLBACK, digest_auth_callback,
                                          SOUP_AUTH_DOMAIN_DIGEST_AUTH_DATA, NULL,
                                          NULL);
      server = phodav_server_get_soup_server (dav);
      g_free (realm);
      soup_server_add_auth_domain (server, auth);
      g_object_unref (auth);
  }


#ifdef WITH_AVAHI
  mdns_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);
  g_signal_connect (mdns_client, "state-changed", G_CALLBACK (mdns_state_changed), NULL);
  if (!ga_client_start (mdns_client, &error))
    my_error (_ ("mDNS failed: %s\n"), error->message);
#endif

  SoupServer *server = phodav_server_get_soup_server (dav);

  int res;
  if (local)
    res = soup_server_listen_local (server, port, 0, &error);
  else if (public)
    res = soup_server_listen_all (server, port, 0, &error);
  else
    my_error (_ ("Internal error, should not happen\n"));

  if (!res) {
    my_error (_ ("Listen failed: %s\n"), error->message);
  }

  g_main_loop_run (mainloop);

  g_main_loop_unref (mainloop);
#ifdef WITH_AVAHI
  g_object_unref (mdns_client);
#endif
  g_object_unref (dav);

  g_message ("Bye");

  return 0;
}
