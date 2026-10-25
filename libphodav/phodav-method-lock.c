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
 * yet (RFC 4918 section 7.3), when the backend supports it. */
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

typedef struct _LockJob
{
  PhodavRequest *req;
  PathHandler *handler;
  gchar *path;
  GCancellable *cancellable;
  GList *submitted;
  gchar *token;
  gchar *refreshed;
  DAVLockScopeType scope;
  DAVLockType type;
  DepthType depth;
  guint timeout;
  xmlNodePtr owner;
  gboolean refresh;
  gint status;
  GError *error;
} LockJob;

static void
lock_job_free (LockJob *job)
{
  g_clear_pointer (&job->req, phodav_request_unref);
  g_clear_pointer (&job->handler, path_handler_unref);
  g_free (job->path);
  g_clear_object (&job->cancellable);
  g_list_free_full (job->submitted, (GDestroyNotify) lock_submitted_free);
  g_free (job->token);
  g_free (job->refreshed);
  if (job->owner)
    xmlFreeNode (job->owner);
  g_clear_error (&job->error);
  g_free (job);
}

static void
lock_run (gpointer data)
{
  LockJob *job = data;
  g_autoptr(PhodavPath) handle = NULL;
  g_autoptr(GFileInfo) info = NULL;
  GFile *file;
  gboolean exists;

  handle = phodav_path_resolve (job->handler, job->path,
                                PHODAV_PATH_CREATE, &job->status);
  if (!handle)
    return;

  file = phodav_path_get_file (handle);
  info = phodav_path_query_info (handle, "standard::type",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 job->cancellable, NULL);
  exists = info != NULL;

  if (exists)
    job->status = SOUP_STATUS_OK;
  else
    job->status = lock_create_resource (file, job->cancellable, &job->error);
}

static void
lock_build_response (LockJob *job, const gchar *token)
{
  xmlChar *mem = NULL;
  gint size = 0;
  xmlNodePtr root, node;
  xmlNsPtr ns;

  root = xmlNewNode (NULL, BAD_CAST "prop");
  ns = xmlNewNs (root, BAD_CAST "DAV:", BAD_CAST "D");
  xmlSetNs (root, ns);

  node = xmlNewChild (root, ns, BAD_CAST "lockdiscovery", NULL);
  {
    xmlNodePtr active = phodav_lock_store_lock_node (
      handler_get_server (job->handler), job->path, token, ns);

    if (active)
      xmlAddChild (node, active);
  }

  xml_node_to_string (root, &mem, &size);
  phodav_request_set_response (job->req, "application/xml",
                               SOUP_MEMORY_TAKE, (gchar *) mem, size);
}

static void
lock_done (gpointer data)
{
  LockJob *job = data;
  PhodavServer *server = handler_get_server (job->handler);

  if (job->status != SOUP_STATUS_OK &&
      job->status != SOUP_STATUS_CREATED)
    {
      /* the resource could not be created: do not leave the lock
       * behind, a second LOCK would be refused with 423 */
      phodav_lock_store_remove_lock (server, job->path, job->token);
    }
  else
    {
      g_autofree gchar *ltoken = g_strdup_printf ("<%s>", job->token);

      /* the lock is committed: advertise its token */
      soup_message_headers_append (phodav_request_get_response_headers (job->req),
                                   "Lock-Token", ltoken);
      lock_build_response (job, job->token);
    }

  phodav_server_namespace_release (server);
  phodav_request_complete (job->req, job->status,
                           g_steal_pointer (&job->error));
  lock_job_free (job);
}

static void
lock_start (gpointer data)
{
  LockJob *job = data;
  PhodavServer *server = handler_get_server (job->handler);

  if (phodav_request_is_finished (job->req))
    {
      phodav_server_namespace_release (server);
      phodav_request_complete (job->req, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
      lock_job_free (job);
      return;
    }

  if (job->refresh)
    {
      job->status = phodav_check_if_refresh (job->req, &job->submitted);
      if (job->status == SOUP_STATUS_OK)
        job->status = phodav_lock_store_refresh_lock (server, job->path,
                                                      job->submitted,
                                                      job->timeout,
                                                      &job->refreshed);

      if (job->status != SOUP_STATUS_OK)
        {
          phodav_server_namespace_release (server);
          phodav_request_complete (job->req, job->status, NULL);
          lock_job_free (job);
          return;
        }

      g_free (job->token);
      job->token = g_steal_pointer (&job->refreshed);
      lock_build_response (job, job->token);
      phodav_server_namespace_release (server);
      phodav_request_complete (job->req, job->status, NULL);
      lock_job_free (job);
      return;
    }

  job->status = phodav_lock_store_add_lock (server, job->path, job->token,
                                            job->scope, job->type, job->depth,
                                            job->owner, job->timeout);
  if (job->status != SOUP_STATUS_OK)
    {
      g_debug ("lock failed");
      phodav_server_namespace_release (server);
      phodav_request_complete (job->req, job->status, NULL);
      lock_job_free (job);
      return;
    }

  phodav_server_dispatch (server, lock_run, lock_done, job);
}

gint
phodav_method_lock (PhodavRequest *req, GError **err)
{
  PathHandler *handler = phodav_request_get_handler (req);
  const gchar *path = phodav_request_get_path (req);
  g_auto(DavDoc) doc = {
    0,
  };
  xmlNodePtr node, owner = NULL;
  DAVLockScopeType scope = DAV_LOCK_SCOPE_NONE;
  DAVLockType type = DAV_LOCK_NONE;
  DepthType depth;
  guint timeout;
  g_autofree gchar *uuid = NULL;
  g_autofree gchar *token = NULL;
  gint status = SOUP_STATUS_BAD_REQUEST;
  SoupMessageHeaders *request_headers = phodav_request_get_request_headers (req);
  SoupMessageBody *request_body = phodav_request_get_request_body (req);
  LockJob *job;

  (void) err;

  /* RFC 4918 section 10.2: only the depths 0 and infinity are valid
   * for a LOCK, and an invalid depth must be rejected */
  if (!depth_from_string (soup_message_headers_get_one (request_headers, "Depth"), &depth) ||
      depth == DEPTH_ONE)
    return SOUP_STATUS_BAD_REQUEST;

  timeout = timeout_from_string (soup_message_headers_get_one (request_headers, "Timeout"));

  job = g_new0 (LockJob, 1);
  job->req = phodav_request_ref (req);
  job->handler = path_handler_ref (handler);
  job->path = g_strdup (path);
  job->cancellable = g_object_ref (phodav_request_get_cancellable (req));
  job->depth = depth;
  job->timeout = timeout;

  if (!request_body->length)
    {
      /* an empty body refreshes an existing lock, which must be
       * identified with an If header (RFC 4918 section 9.10.2) */
      job->refresh = TRUE;
      phodav_server_namespace_acquire (handler_get_server (handler),
                                       lock_start, job);
      return 0;
    }

  if (!davdoc_parse (&doc, request_body, "lockinfo"))
    {
      lock_job_free (job);
      return status;
    }

  node = doc.root;
  for (node = node->children; node; node = node->next)
    {
      if (!xml_node_is_element (node))
        continue;

      if (xml_node_has_name (node, "lockscope"))
        {
          scope = parse_lockscope (node);
          if (scope == DAV_LOCK_SCOPE_NONE)
            {
              lock_job_free (job);
              return status;
            }
        }
      else if (xml_node_has_name (node, "locktype"))
        {
          type = parse_locktype (node);
          if (type == DAV_LOCK_NONE)
            {
              lock_job_free (job);
              return status;
            }
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
    {
      lock_job_free (job);
      return status;
    }

  g_debug ("lock depth:%d scope:%d type:%d owner:%p, timeout: %u",
           depth, scope, type, owner, timeout);

  uuid = g_uuid_string_random ();
  token = g_strdup_printf ("urn:uuid:%s", uuid);

  job->scope = scope;
  job->type = type;
  job->token = g_strdup (token);
  if (owner)
    job->owner = xmlCopyNode (owner, 1);

  phodav_server_namespace_acquire (handler_get_server (handler),
                                   lock_start, job);

  return 0;
}
