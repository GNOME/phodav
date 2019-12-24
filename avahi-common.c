/*
 * Copyright (C) 2019 Red Hat, Inc.
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
#include "avahi-common.h"

#include <avahi-gobject/ga-client.h>
#include <avahi-gobject/ga-entry-group.h>

static GaClient *mdns_client;
static GaEntryGroup *mdns_group;

static const gchar *s_name;
static guint s_port;

static void
mdns_register_service (void)
{
  GaEntryGroupService *mdns_service;
  GError *error = NULL;

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

  mdns_service = ga_entry_group_add_service (mdns_group,
                                             s_name, "_webdav._tcp",
                                             s_port, &error,
                                             NULL);
  if (!mdns_service)
    {
      g_warning ("Could not create service: %s", error->message);
      g_clear_error (&error);
      return;
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
}

static void
mdns_unregister_service (void)
{
  GError *error = NULL;

  if (mdns_group)
    {
      if (!ga_entry_group_reset (mdns_group, &error))
        {
          g_warning ("Could not disconnect MDNS service: %s", error->message);
          g_clear_error (&error);
        }
      g_debug ("MDNS client disconected");
    }
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
      mdns_unregister_service ();
      break;

    default:
      // Do nothing
      break;
    }
}

gboolean
avahi_client_start (const gchar *name, guint port, GError **error)
{
  g_return_val_if_fail (mdns_client == NULL, FALSE);

  mdns_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);
  s_name = name;
  s_port = port;

  g_signal_connect (mdns_client, "state-changed", G_CALLBACK (mdns_state_changed), NULL);
  return ga_client_start (mdns_client, error);
}

void
avahi_client_stop ()
{
  mdns_unregister_service ();
  g_clear_object (&mdns_group);
  g_clear_object (&mdns_client);
}
