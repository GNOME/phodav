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

/* create the empty resource of a LOCK on a URL which is not mapped
 * yet (RFC 4918 section 7.3), when the backend supports it. The
 * request is rolled back by the caller when this fails. */
static gint
lock_create_resource (GFile *file, GCancellable *cancellable, GError **err)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) stream = NULL;

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
      g_debug ("LOCK: %s cannot be created, locking it as it is",
               g_file_peek_path (file));
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
  g_autofree gchar *refreshed = NULL;
  g_autolist(LockSubmitted) submitted = NULL;
  PhodavLockGuard *guard = NULL;
  gint status = SOUP_STATUS_BAD_REQUEST;
  gboolean exists;
  g_autoptr(GFile) file = NULL;
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

      GList *paths = g_list_prepend (NULL, (gpointer) path);
      PhodavServer *server = handler_get_server (handler);

      guard = phodav_lock_store_guard_acquire (server, paths);
      g_list_free (paths);

      /* the lock to refresh is the one covering the path among the
       * tokens submitted with the If header */
      status = phodav_lock_store_refresh_lock (server, path, submitted,
                                               timeout, &refreshed);
      if (status != SOUP_STATUS_OK)
        goto end;

      token = g_steal_pointer (&refreshed);
      g_debug ("refresh token %s", token);
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
  file = phodav_path_resolve (handler, path, PHODAV_PATH_CREATE, &status);
  if (!file)
    goto end;

  exists = g_file_query_exists (file, cancellable);

  {
    PhodavServer *server = handler_get_server (handler);
    GList *paths = g_list_prepend (NULL, (gpointer) path);

    guard = phodav_lock_store_guard_acquire (server, paths);
    g_list_free (paths);

    status = phodav_lock_store_add_lock (server, path, token, scope, type,
                                         depth, owner, timeout);
    if (status != SOUP_STATUS_OK)
      {
        g_debug ("lock failed");
        goto end;
      }

    if (!exists)
      {
        status = lock_create_resource (file, cancellable, err);
        if (status != SOUP_STATUS_CREATED && status != SOUP_STATUS_OK)
          {
            /* the empty resource could not be created: do not leave the
             * lock behind, a second LOCK would be refused with 423 */
            phodav_lock_store_remove_lock (server, path, token);
            goto end;
          }
      }
    else
      status = SOUP_STATUS_OK;
  }

  /* the lock is committed: advertise its token */
  ltoken = g_strdup_printf ("<%s>", token);
  soup_message_headers_append (phodav_request_get_response_headers (req), "Lock-Token", ltoken);

body:
  root = xmlNewNode (NULL, BAD_CAST "prop");
  ns = xmlNewNs (root, BAD_CAST "DAV:", BAD_CAST "D");
  xmlSetNs (root, ns);

  node = xmlNewChild (root, ns, BAD_CAST "lockdiscovery", NULL);
  {
    xmlNodePtr active = phodav_lock_store_lock_node (handler_get_server (handler),
                                                     path, token, ns);

    if (active)
      xmlAddChild (node, active);
  }

  xml_node_to_string (root, &mem, &size);
  phodav_request_set_response (req, "application/xml",
                               SOUP_MEMORY_TAKE, (gchar *) mem, size);

end:
  phodav_lock_store_guard_release (guard);
  return status;
}
