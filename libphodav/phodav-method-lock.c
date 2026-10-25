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
lock_resource_exists (PathHandler *handler, const gchar *path, GCancellable *cancellable)
{
  g_autoptr(GFile) file = handler_get_file_for_path (handler, path);

  return g_file_query_exists (file, cancellable);
}

/* create the empty resource of a LOCK on a URL which is not mapped
 * yet (RFC 4918 section 7.3), when the backend supports it. The
 * request is rolled back by the caller when this fails. */
static gint
lock_create_resource (PathHandler *handler, const gchar *path, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) stream = NULL;
  g_autoptr(GFile) file = NULL;

  file = handler_get_file_for_path (handler, path);
  stream = g_file_create (file, G_FILE_CREATE_NONE, cancellable, &error);
  if (stream)
    return SOUP_STATUS_CREATED;

  /* the resource appeared after it was queried: there is nothing to
   * create anymore */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    return SOUP_STATUS_OK;

  /* a backend which does not implement create() (the virtual
   * directories) cannot hold a null resource: treat the resource as
   * existing rather than failing the lock */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      g_debug ("LOCK: %s cannot be created, locking it as it is", path);
      return SOUP_STATUS_OK;
    }

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    return SOUP_STATUS_CONFLICT;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_READ_ONLY))
    return SOUP_STATUS_FORBIDDEN;

  g_propagate_error (err, g_steal_pointer (&error));
  return SOUP_STATUS_INTERNAL_SERVER_ERROR;
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
phodav_method_lock (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  const gchar *path = phodav_request_get_path (req);
  GCancellable *cancellable = phodav_request_get_cancellable (req);
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
  gboolean exists;
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  SoupMessageBody *request_body;

  /* RFC 4918 section 10.2: only the depths 0 and infinity are valid
   * for a LOCK, and an invalid depth must be rejected */
  if (!depth_from_string (soup_message_headers_get_one (request_headers, "Depth"), &depth) ||
      depth == DEPTH_ONE)
    goto end;

  timeout = timeout_from_string (soup_message_headers_get_one (request_headers, "Timeout"));

  request_body = phodav_request_get_request_body (req);
  if (!request_body->length)
    {
      /* an empty body refreshes an existing lock, which must be
       * identified with an If header (RFC 4918 section 9.10.2) */
      status = phodav_check_if_refresh (req, &submitted);
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
          /* RFC 4918 section 14.19: there is at most one owner, keep
           * the first one and ignore the extra ones */
          if (owner == NULL)
            owner = node;
          else
            g_debug ("LOCK: ignoring the extra owner element");
        }
    }

  if (scope == DAV_LOCK_SCOPE_NONE || type == DAV_LOCK_NONE)
    goto end;

  g_debug ("lock depth:%d scope:%d type:%d owner:%p, timeout: %u",
           depth, scope, type, owner, timeout);

  uuid = g_uuid_string_random ();
  token = g_strdup_printf ("urn:uuid:%s", uuid);

  /* an existing resource is locked in place; only an unmapped URL
   * needs an empty resource */
  exists = lock_resource_exists (handler, path, cancellable);

  lpath = server_get_path (handler_get_server (handler), path);
  {
    g_autoptr(DAVLock) new_lock = dav_lock_new (lpath, token, scope, type, depth, owner, timeout);

    if (!try_add_lock (handler_get_server (handler), path, new_lock))
      {
        g_debug ("lock failed");
        status = SOUP_STATUS_LOCKED;
        server_remove_path_if_empty (handler_get_server (handler), path);
        goto end;
      }

    /* owned by the path now */
    lock = g_steal_pointer (&new_lock);
  }

  if (!exists)
    {
      status = lock_create_resource (handler, path, cancellable, err);
      if (status != SOUP_STATUS_CREATED && status != SOUP_STATUS_OK)
        {
          /* the empty resource could not be created: do not leave the
           * lock behind, a second LOCK would be refused with 423 */
          g_clear_pointer (&lock, dav_lock_free);
          server_remove_path_if_empty (handler_get_server (handler), path);
          goto end;
        }
    }
  else
    status = SOUP_STATUS_OK;

  /* the lock is committed: advertise its token */
  ltoken = g_strdup_printf ("<%s>", token);
  soup_message_headers_append (phodav_request_get_response_headers (req), "Lock-Token", ltoken);

body:
  root = xmlNewNode (NULL, BAD_CAST "prop");
  ns = xmlNewNs (root, BAD_CAST "DAV:", BAD_CAST "D");
  xmlSetNs (root, ns);

  node = xmlNewChild (root, ns, BAD_CAST "lockdiscovery", NULL);
  xmlAddChild (node, dav_lock_get_activelock_node (lock, ns));

  xml_node_to_string (root, &mem, &size);
  phodav_request_set_response (req, "application/xml",
                               SOUP_MEMORY_TAKE, (gchar *) mem, size);

end:
  return status;
}
