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

#include "guuid.h"
#include "phodav-multistatus.h"
#include "phodav-path.h"
#include "phodav-lock.h"
#include "phodav-utils.h"

static gboolean G_GNUC_PURE
check_lock (const gchar *key, Path *path, gpointer data)
{
  DAVLock *lock = data;
  DAVLock *other = NULL;
  GList *l;

  for (l = path->locks; l; l = l->next)
    {
      other = l->data;
      if (other->scope == DAV_LOCK_SCOPE_EXCLUSIVE)
        return FALSE;
    }

  if (other && lock->scope == DAV_LOCK_SCOPE_EXCLUSIVE)
    return FALSE;

  return TRUE;
}

static gboolean
try_add_lock (PhodavServer *server, const gchar *path, DAVLock *lock)
{
  Path *p;

  if (!server_foreach_parent_path (server, path, check_lock, lock))
    return FALSE;

  p = server_get_path (server, path);
  path_add_lock (p, lock);

  return TRUE;
}

static gboolean
lock_ensure_file (PathHandler *handler, const char *path,
                  GCancellable *cancellable, GError **err)
{
  GError *e = NULL;
  GFileOutputStream *stream = NULL;
  GFile *file = NULL;
  gboolean created = FALSE;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  stream = g_file_create (file, G_FILE_CREATE_NONE, cancellable, &e);
  created = !!stream;

  if (e)
    {
      if (g_error_matches (e, G_IO_ERROR, G_IO_ERROR_EXISTS))
        g_clear_error (&e);
      else
        g_propagate_error (err, e);
    }

  g_clear_object (&stream);
  g_clear_object (&file);

  return created;
}

static DAVLockScopeType
parse_lockscope (xmlNodePtr rt)
{
  xmlNodePtr node;

  for (node = rt->children; node; node = node->next)
    if (xml_node_is_element (node))
      break;

  if (node == NULL)
    return DAV_LOCK_SCOPE_NONE;

  if (!g_strcmp0 ((char *) node->name, "exclusive"))
    return DAV_LOCK_SCOPE_EXCLUSIVE;
  else if (!g_strcmp0 ((char *) node->name, "shared"))
    return DAV_LOCK_SCOPE_SHARED;
  else
    return DAV_LOCK_SCOPE_NONE;
}

static DAVLockType
parse_locktype (xmlNodePtr rt)
{
  xmlNodePtr node;

  for (node = rt->children; node; node = node->next)
    if (xml_node_is_element (node))
      break;

  if (node == NULL)
    return DAV_LOCK_NONE;

  if (!g_strcmp0 ((char *) node->name, "write"))
    return DAV_LOCK_WRITE;
  else
    return DAV_LOCK_NONE;
}

gint
phodav_method_lock (PathHandler *handler, SoupServerMessage *msg,
                    const char *path, GError **err)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  Path *lpath = NULL;
  xmlChar *mem = NULL;
  int size = 0;
  DavDoc doc = {0, };
  xmlNodePtr node = NULL, owner = NULL, root = NULL;
  xmlNsPtr ns = NULL;
  DAVLockScopeType scope = DAV_LOCK_SCOPE_SHARED;
  DAVLockType type;
  DepthType depth;
  guint timeout;
  gchar *ltoken = NULL, *uuid = NULL, *token = NULL;
  DAVLock *lock = NULL;
  gint status = SOUP_STATUS_BAD_REQUEST;
  gboolean created;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  SoupMessageBody *request_body;

  depth = depth_from_string (soup_message_headers_get_one (request_headers, "Depth"));
  timeout = timeout_from_string (soup_message_headers_get_one (request_headers, "Timeout"));

  if (depth != DEPTH_ZERO && depth != DEPTH_INFINITY)
    goto end;

  request_body = soup_server_message_get_request_body (msg);
  if (!request_body->length)
    {
      const gchar *hif = soup_message_headers_get_one (request_headers, "If");
      gint len = strlen (hif);

      if (len <= 4 || hif[0] != '(' || hif[1] != '<' || hif[len - 2] != '>' || hif[len - 1] != ')')
        goto end;

      token = g_strndup (hif + 2, len - 4);

      g_debug ("refresh token %s", token);
      lock = server_path_get_lock (handler_get_server (handler), path, token);

      if (!lock)
        goto end;

      dav_lock_refresh_timeout (lock, timeout);
      status = SOUP_STATUS_OK;
      goto body;
    }

  if (!davdoc_parse (&doc, msg, request_body, "lockinfo"))
    goto end;

  node = doc.root;
  for (node = node->children; node; node = node->next)
    {
      if (!xml_node_is_element (node))
        continue;

      if (xml_node_has_name (node, "lockscope"))
        {
          scope = parse_lockscope (node);
          if (scope == DAV_LOCK_SCOPE_NONE)
            break;
        }
      else if (xml_node_has_name (node, "locktype"))
        {
          type = parse_locktype (node);
          if (type == DAV_LOCK_NONE)
            break;
        }
      else if (xml_node_has_name (node, "owner"))
        {
          if (owner == NULL)
            owner = node;
          else
            g_warn_if_reached ();
        }
    }

  g_debug ("lock depth:%d scope:%d type:%d owner:%p, timeout: %u",
           depth, scope, type, owner, timeout);

  uuid = g_uuid_string_random ();
  token = g_strdup_printf ("urn:uuid:%s", uuid);
  ltoken = g_strdup_printf ("<%s>", token);
  soup_message_headers_append (soup_server_message_get_response_headers (msg), "Lock-Token", ltoken);

  lpath = server_get_path (handler_get_server (handler), path);
  lock = dav_lock_new (lpath, token, scope, type, depth, owner, timeout);
  if (!try_add_lock (handler_get_server (handler), path, lock))
    {
      g_warning ("lock failed");
      dav_lock_free (lock);
      status = SOUP_STATUS_LOCKED;
      goto end;
    }

  created = lock_ensure_file (handler, path, cancellable, err);
  if (*err)
    goto end;

  status = created ? SOUP_STATUS_CREATED : SOUP_STATUS_OK;

body:
  root = xmlNewNode (NULL, BAD_CAST "prop");
  ns = xmlNewNs (root, BAD_CAST "DAV:", BAD_CAST "D");
  xmlSetNs (root, ns);

  node = xmlNewChild (root, ns, BAD_CAST "lockdiscovery", NULL);
  xmlAddChild (node, dav_lock_get_activelock_node (lock, ns));

  xml_node_to_string (root, &mem, &size);
  soup_server_message_set_response (msg, "application/xml",
                                    SOUP_MEMORY_TAKE, (gchar *) mem, size);

end:
  g_free (ltoken);
  g_free (token);
  g_free (uuid);
  davdoc_free (&doc);

  return status;
}
