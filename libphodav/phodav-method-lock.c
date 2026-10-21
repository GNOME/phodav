/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-lock.h"
#include "phodav-multistatus.h"
#include "phodav-path.h"
#include "phodav-priv.h"
#include "phodav-utils.h"

static gboolean
check_lock (const gchar *key, Path *path, gpointer data)
{
  DAVLock *lock = data;
  DAVLock *other = NULL;
  GList *l;

  path_expire_locks (path);

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

  /* RFC 4918 section 7.4: a depth-infinity lock on a collection must
   * fail when a member is locked in a conflicting manner */
  if (lock->depth == DEPTH_INFINITY &&
      server_tree_has_lock_conflict (server, path, lock))
    return FALSE;

  p = server_get_path (server, path);
  path_add_lock (p, lock);

  return TRUE;
}

static gboolean
lock_ensure_file (PathHandler *handler, const gchar *path, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) e = NULL;
  g_autoptr(GFileOutputStream) stream = NULL;
  g_autoptr(GFile) file = NULL;
  gboolean created = FALSE;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  stream = g_file_create (file, G_FILE_CREATE_NONE, cancellable, &e);
  created = !!stream;

  if (e && !g_error_matches (e, G_IO_ERROR, G_IO_ERROR_EXISTS))
    g_propagate_error (err, g_steal_pointer (&e));

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

  if (g_strcmp0 ((char *) node->name, "exclusive") == 0)
    return DAV_LOCK_SCOPE_EXCLUSIVE;
  else if (g_strcmp0 ((char *) node->name, "shared") == 0)
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

  if (g_strcmp0 ((char *) node->name, "write") == 0)
    return DAV_LOCK_WRITE;
  else
    return DAV_LOCK_NONE;
}

gint
phodav_method_lock (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  Path *lpath = NULL;
  xmlChar *mem = NULL;
  gint size = 0;
  g_auto(DavDoc) doc = {
    0,
  };
  xmlNodePtr node = NULL, owner = NULL, root = NULL;
  xmlNsPtr ns = NULL;
  DAVLockScopeType scope = DAV_LOCK_SCOPE_NONE;
  DAVLockType type = DAV_LOCK_NONE;
  DepthType depth;
  guint timeout;
  g_autofree gchar *ltoken = NULL;
  g_autofree gchar *uuid = NULL;
  g_autofree gchar *token = NULL;
  g_autolist(LockSubmitted) submitted = NULL;
  DAVLock *lock = NULL;
  gint status = SOUP_STATUS_BAD_REQUEST;
  gboolean created;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  SoupMessageBody *request_body;

  /* RFC 4918 section 10.2: only the depths 0 and infinity are valid
   * for a LOCK, and an invalid depth must be rejected */
  if (!depth_from_string (soup_message_headers_get_one (request_headers, "Depth"), &depth) ||
      depth == DEPTH_ONE)
    goto end;

  timeout = timeout_from_string (soup_message_headers_get_one (request_headers, "Timeout"));

  request_body = soup_server_message_get_request_body (msg);
  if (!request_body->length)
    {
      /* an empty body refreshes an existing lock, which must be
       * identified with an If header (RFC 4918 section 9.10.2) */
      status = phodav_check_if_refresh (handler, msg, path, &submitted);
      if (status != SOUP_STATUS_OK)
        goto end;

      /* the lock to refresh is the one covering the path among the
       * tokens submitted with the If header */
      for (GList *l = submitted; l != NULL; l = l->next)
        {
          LockSubmitted *sub = l->data;

          lock = server_path_lookup_lock (handler_get_server (handler), path, sub->token);
          if (lock)
            break;
        }

      if (!lock)
        {
          status = SOUP_STATUS_BAD_REQUEST;
          goto end;
        }

      g_debug ("refresh token %s", lock->token);
      dav_lock_refresh_timeout (lock, timeout);
      status = SOUP_STATUS_OK;
      goto body;
    }

  if (!davdoc_parse (&doc, request_body, "lockinfo"))
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

  if (scope == DAV_LOCK_SCOPE_NONE || type == DAV_LOCK_NONE)
    goto end;

  g_debug ("lock depth:%d scope:%d type:%d owner:%p, timeout: %u",
           depth, scope, type, owner, timeout);

  uuid = g_uuid_string_random ();
  token = g_strdup_printf ("urn:uuid:%s", uuid);
  ltoken = g_strdup_printf ("<%s>", token);
  soup_message_headers_append (soup_server_message_get_response_headers (msg), "Lock-Token", ltoken);

  lpath = server_get_path (handler_get_server (handler), path);
  {
    g_autoptr(DAVLock) new_lock = dav_lock_new (lpath, token, scope, type, depth, owner, timeout);

    if (!try_add_lock (handler_get_server (handler), path, new_lock))
      {
        g_debug ("lock failed");
        status = SOUP_STATUS_LOCKED;
        goto end;
      }

    /* owned by the path now */
    lock = g_steal_pointer (&new_lock);
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
  return status;
}
