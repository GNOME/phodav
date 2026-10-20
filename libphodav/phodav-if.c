/*
 * Copyright (C) 2014 Red Hat, Inc.
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

#include "phodav-lock.h"
#include "phodav-priv.h"
#include "phodav-utils.h"

typedef struct _IfState
{
  gchar *cur;
  GUri *base;
  gchar *path;
  GList *locks;
  gboolean error;
} IfState;

static gboolean
eat_whitespaces (IfState *state)
{
  while (*state->cur && strchr (" \f\n\r\t\v", *state->cur))
    state->cur++;

  return !*state->cur;
}

static gboolean
next_token (IfState *state, const gchar *token)
{
  eat_whitespaces (state);

  return g_str_has_prefix (state->cur, token);
}

static gboolean
accept_token (IfState *state, const gchar *token)
{
  gboolean success = next_token (state, token);

  if (success)
    state->cur += strlen (token);

  return success;
}

static const gchar *
accept_ref (IfState *state)
{
  gchar *url, *end;

  if (!accept_token (state, "<"))
    return NULL;

  url = state->cur;
  end = strchr (state->cur, '>');
  if (end)
    {
      *end = '\0';
      state->cur = end + 1;
      return url;
    }

  return NULL;
}

static gchar *
accept_etag (IfState *state)
{
  g_autoptr(GString) str = NULL;
  gboolean success = FALSE;

  str = g_string_sized_new (strlen (state->cur));

  if (!accept_token (state, "["))
    goto end;

  if (!accept_token (state, "\""))
    goto end;

  while (*state->cur)
    {
      if (*state->cur == '"')
        break;
      else if (*state->cur == '\\')
        state->cur++;

      g_string_append_c (str, *state->cur);
      state->cur++;
    }

  if (!accept_token (state, "\""))
    goto end;

  if (!accept_token (state, "]"))
    goto end;

  success = TRUE;

end:
  if (!success)
    return NULL;

  return g_string_free_and_steal (g_steal_pointer (&str));
}

static gboolean
check_token (PathHandler *handler, const gchar *path, const gchar *token)
{
  PhodavServer *server = handler_get_server (handler);

  g_debug ("check %s for %s", token, path);

  if (g_strcmp0 (token, "DAV:no-lock") == 0)
    return FALSE;

  return !!server_path_get_lock (server, path, token);
}

static gboolean
check_etag (PathHandler *handler, const gchar *path, const gchar *etag)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GError) error = NULL;
  const gchar *fetag;

  g_debug ("check etag %s for %s", etag, path);

  file = g_file_get_child (handler_get_file (handler), path + 1);
  info = g_file_query_info (file, "etag::*",
                            G_FILE_QUERY_INFO_NONE, cancellable, &error);
  if (!info)
    {
      if (error)
        g_warning ("check_etag error: %s", error->message);
      return FALSE;
    }

  fetag = g_file_info_get_etag (info);
  g_warn_if_fail (fetag != NULL);

  return !g_strcmp0 (etag, fetag);
}

static gboolean
eval_if_condition (PathHandler *handler, IfState *state)
{
  gboolean success = FALSE;

  if (next_token (state, "<"))
    {
      const gchar *token = accept_ref (state);
      LockSubmitted *l;

      if (!token)
        {
          state->error = TRUE;
          return FALSE;
        }

      l = lock_submitted_new (state->path, token);
      state->locks = g_list_append (state->locks, l);

      success = check_token (handler, state->path, token);
    }
  else if (next_token (state, "["))
    {
      gchar *etag = accept_etag (state);

      if (!etag)
        {
          state->error = TRUE;
          return FALSE;
        }

      success = check_etag (handler, state->path, etag);
      g_free (etag);
    }
  else
    state->error = TRUE;

  return success;
}

static gboolean
eval_if_not_condition (PathHandler *handler, IfState *state)
{
  gboolean not = FALSE;
  gboolean res;

  if (accept_token (state, "Not"))
    not = TRUE;

  res = eval_if_condition (handler, state);

  return not ? !res : res;
}

static gboolean
eval_if_list (PathHandler *handler, IfState *state)
{
  gboolean success;

  if (!accept_token (state, "("))
    {
      state->error = TRUE;
      return FALSE;
    }

  success = eval_if_not_condition (handler, state);

  while (!state->error && !accept_token (state, ")"))
    {
      if (eat_whitespaces (state))
        state->error = TRUE;
      else
        success &= eval_if_not_condition (handler, state);
    }

  return success;
}

static gboolean
eval_if_lists (PathHandler *handler, IfState *state)
{
  gboolean success = FALSE;

  if (!next_token (state, "("))
    {
      state->error = TRUE;
      return FALSE;
    }

  while (!state->error && next_token (state, "("))
    success |= eval_if_list (handler, state);

  return success;
}

static gboolean
eval_if_tag (PathHandler *handler, IfState *state)
{
  GUri *uri;
  g_autofree gchar *path = NULL;
  const gchar *ref = accept_ref (state);

  /* the resource may be an absolute URI or a path */
  uri = ref ? g_uri_parse_relative (state->base, ref, G_URI_FLAGS_ENCODED_PATH, NULL) : NULL;
  if (!uri)
    {
      state->error = TRUE;
      return FALSE;
    }

  /* like the request path, it may not contain encoded slashes */
  path = g_uri_unescape_string (g_uri_get_path (uri), "/");
  g_uri_unref (uri);
  if (!path || *path != '/' || path_has_dot_dot_segment (path))
    {
      state->error = TRUE;
      return FALSE;
    }

  g_free (state->path);
  state->path = g_steal_pointer (&path);

  return eval_if_lists (handler, state);
}

static gboolean
eval_if (PathHandler *handler, IfState *state)
{
  gboolean success = FALSE;

  if (next_token (state, "<"))
    {
      while (!state->error && !eat_whitespaces (state))
        success |= eval_if_tag (handler, state);
    }
  else
    {
      while (!state->error && !eat_whitespaces (state))
        success |= eval_if_lists (handler, state);
    }

  return success;
}

/* Parse and evaluate the request If header (RFC 4918 section 10.4),
 * collecting the submitted lock tokens in @locks.
 *
 * Returns SOUP_STATUS_OK when the request has no If header or its
 * conditions are met, SOUP_STATUS_BAD_REQUEST for a malformed header
 * and SOUP_STATUS_PRECONDITION_FAILED when the conditions are not
 * met. @has_header is set when the request carried an If header. */
static gint
eval_if_header (PathHandler *handler,
                SoupServerMessage *msg,
                const gchar *path,
                GList **locks,
                gboolean *has_header)
{
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  g_autofree gchar *str = g_strdup (soup_message_headers_get_one (request_headers, "If"));
  IfState state = { .cur = str, .base = soup_server_message_get_uri (msg), .path = g_strdup (path) };
  gint status = SOUP_STATUS_OK;

  *has_header = str != NULL;

  if (str && !(eval_if (handler, &state) && !state.error))
    {
      g_list_free_full (state.locks, (GDestroyNotify) lock_submitted_free);

      if (state.error)
        {
          g_debug ("invalid If header: %s", str);
          status = SOUP_STATUS_BAD_REQUEST;
        }
      else
        status = SOUP_STATUS_PRECONDITION_FAILED;
    }
  else
    *locks = state.locks;

  g_free (state.path);

  return status;
}

gint
phodav_check_if (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GList **locks)
{
  PhodavServer *server = handler_get_server (handler);
  gboolean has_header;
  gint status;

  status = eval_if_header (handler, msg, path, locks, &has_header);
  if (status != SOUP_STATUS_OK)
    return status;

  /* COPY does not modify the source, its locks do not apply */
  if (soup_server_message_get_method (msg) != SOUP_METHOD_COPY &&
      server_path_has_other_locks (server, path, *locks))
    return SOUP_STATUS_LOCKED;

  return SOUP_STATUS_OK;
}

/* Like phodav_check_if(), for a lock refresh (a LOCK request with an
 * empty body, RFC 4918 section 9.10.2): the If header identifying the
 * lock is mandatory, and the other locks held on the path are not an
 * obstacle to refreshing a lock the client owns. */
gint
phodav_check_if_refresh (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GList **locks)
{
  gboolean has_header;
  gint status;

  status = eval_if_header (handler, msg, path, locks, &has_header);
  if (status != SOUP_STATUS_OK)
    return status;

  if (!has_header)
    return SOUP_STATUS_BAD_REQUEST;

  return SOUP_STATUS_OK;
}
