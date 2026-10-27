/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "avahi-common.h"

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>

#include <avahi-gobject/ga-client.h>
#include <avahi-gobject/ga-entry-group.h>
#include <ifaddrs.h>

static GaClient *mdns_client;
static GaEntryGroup *mdns_group;

static const gchar *s_name;
static guint s_port;
static gboolean s_local;
static gboolean s_tls;

static gboolean
ifaddr_is_loopback (struct ifaddrs *ifa)
{
  union
  {
    struct sockaddr_in *in;
    struct sockaddr_in6 *in6;
  } sa;

  if (!(ifa->ifa_flags & IFF_LOOPBACK))
    return FALSE;

  if (!ifa->ifa_addr)
    return FALSE;

  switch (ifa->ifa_addr->sa_family)
    {
    case AF_INET:
      sa.in = (struct sockaddr_in *) (ifa->ifa_addr);
      return sa.in->sin_addr.s_addr == g_htonl (0x7f000001); // 127.0.0.1
    case AF_INET6:
      sa.in6 = (struct sockaddr_in6 *) (ifa->ifa_addr);
      return IN6_IS_ADDR_LOOPBACK (&sa.in6->sin6_addr);
    }
  return FALSE;
}

static gboolean
get_loopback_if_id (gint *if_id)
{
  struct ifaddrs *ifaddr, *ifa;
  gboolean found = FALSE;

  if (getifaddrs (&ifaddr) == -1)
    {
      g_warning ("getifaddrs failed: %s", g_strerror (errno));
      return FALSE;
    }

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
      if (!ifaddr_is_loopback (ifa))
        continue;

      *if_id = if_nametoindex (ifa->ifa_name);
      if (*if_id == 0)
        {
          g_warning ("Could not find the index of %s: %s",
                     ifa->ifa_name, g_strerror (errno));
          continue;
        }

      found = TRUE;
      break;
    }

  freeifaddrs (ifaddr);

  if (!found)
    g_warning ("Could not find a loopback interface");

  return found;
}

static void
mdns_register_service (void)
{
  GaEntryGroupService *mdns_service;
  GError *error = NULL;
  gint if_id = AVAHI_IF_UNSPEC;

  if (s_local && !get_loopback_if_id (&if_id))
    {
      g_warning ("Could not announce MDNS service: the loopback interface is unknown");
      return;
    }

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

  mdns_service =
    ga_entry_group_add_service_full (mdns_group,
                                     if_id,
                                     AVAHI_PROTO_UNSPEC,
                                     0,
                                     s_name,
                                     /* WebDAV over TLS is announced with its own service type */
                                     s_tls ? "_webdavs._tcp" : "_webdav._tcp",
                                     NULL, NULL,
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
avahi_client_start (const gchar *name, guint port, gboolean local, gboolean tls, GError **error)
{
  g_return_val_if_fail (mdns_client == NULL, FALSE);

  mdns_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);
  s_name = name;
  s_port = port;
  s_local = local;
  s_tls = tls;

  g_signal_connect (mdns_client, "state-changed", G_CALLBACK (mdns_state_changed), NULL);
  return ga_client_start (mdns_client, error);
}

void
avahi_client_stop (void)
{
  mdns_unregister_service ();
  g_clear_object (&mdns_group);
  g_clear_object (&mdns_client);
}
