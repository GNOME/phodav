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

#include "phodav-priv.h"
#include "phodav-lock.h"
#include "phodav-utils.h"

typedef struct _IfState
{
  gchar   *cur;
  GUri    *base;
  gchar   *path;
  GList   *locks;
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

static const gchar*
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

static gchar*
accept_etag (IfState *state)
{
  GString *str = NULL;
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
  return g_string_free (str, !success);
}

static gboolean
check_token (PathHandler *handler, const gchar *path, const gchar *token)
{
  PhodavServer *server = handler_get_server (handler);

  g_debug ("check %s for %s", token, path);

  if (!g_strcmp0 (token, "DAV:no-lock"))
    return FALSE;

  return !!server_path_get_lock (server, path, token);
}

static gboolean
check_etag (PathHandler *handler, const gchar *path, const gchar *etag)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  GFile *file = NULL;
  GFileInfo *info = NULL;
  GError *error = NULL;
  const gchar *fetag;
  gboolean success = FALSE;

  g_debug ("check etag %s for %s", etag, path);

  file = g_file_get_child (handler_get_file (handler), path + 1);
  info = g_file_query_info (file, "etag::*",
                            G_FILE_QUERY_INFO_NONE, cancellable, &error);
  if (!info)
    goto end;

  fetag = g_file_info_get_etag (info);
  g_warn_if_fail (fetag != NULL);

  success = !g_strcmp0 (etag, fetag);

end:
  if (error)
    {
      g_warning ("check_etag error: %s", error->message);
      g_clear_error (&error);
    }

  g_clear_object (&info);
  g_clear_object (&file);

  return success;
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
  gchar *path;
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
      g_free (path);
      state->error = TRUE;
      return FALSE;
    }

  g_free (state->path);
  state->path = path;

  return eval_if_lists (handler, state);
}


static gboolean
eval_if (PathHandler *handler, IfState *state)
{
  gboolean success = FALSE;

  if (next_token (state, "<")) {
    while (!state->error && !eat_whitespaces (state))
      success |= eval_if_tag (handler, state);
  } else {
    while (!state->error && !eat_whitespaces (state))
      success |= eval_if_lists (handler, state);
  }

  return success;
}

gint
phodav_check_if (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GList **locks)
{
  PhodavServer *server = handler_get_server (handler);
  gboolean success = TRUE;
  gint status;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (msg);
  gchar *str = g_strdup (soup_message_headers_get_one (request_headers, "If"));
  IfState state = { .cur = str, .base = soup_server_message_get_uri (msg), .path = g_strdup (path) };
  gboolean copy = soup_server_message_get_method (msg) == SOUP_METHOD_COPY;

  if (!str)
    goto end;

  if (eval_if (handler, &state) && !state.error)
    {
      *locks = state.locks;
    }
  else
    {
      g_list_free_full (state.locks, (GDestroyNotify) lock_submitted_free);
      success = FALSE;
    }

  if (state.error)
    {
      g_debug ("invalid If header: %s", soup_message_headers_get_one (request_headers, "If"));
      status = SOUP_STATUS_BAD_REQUEST;
      goto out;
    }

end:
  status = success ? SOUP_STATUS_OK
           : str ? SOUP_STATUS_PRECONDITION_FAILED : SOUP_STATUS_LOCKED;

  if (success && !copy && server_path_has_other_locks (server, path, *locks))
    status = SOUP_STATUS_LOCKED;

out:
  g_free (str);
  g_free (state.path);
  return status;
}
