/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/* A stress test for the concurrency model: several HTTP clients, an
 * external writer changing the shared directory, and a backend shim
 * which makes every filesystem operation slow enough to overlap the
 * requests. The test only asserts the observable contract; the point is
 * to exercise the namespace, the worker pools, the lock store and the
 * cancellation paths together under load. */

#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "libphodav/phodav.h"

#include "phodav-backend.h"
#include "phodav-server-priv.h"

#define HELLO_CONTENT "Hello, phodav!\n"
#define SLOW_DELAY_US (5 * 1000)
#define CLIENT_COUNT 4
#define CLIENT_ITERATIONS 6

#define PROPFIND_LOCKDISCOVERY \
  "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:lockdiscovery/></D:prop></D:propfind>"
#define LOCK_EXCLUSIVE                                                     \
  "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:exclusive/></D:lockscope>" \
  "<D:locktype><D:write/></D:locktype>"                                    \
  "<D:owner>stress-test</D:owner></D:lockinfo>"

typedef struct _SlowBackend
{
  PhodavBackend parent;
  PhodavBackend *inner;
  guint delay_us;
} SlowBackend;

static void
slow_delay (SlowBackend *self)
{
  if (self->delay_us)
    {
      g_usleep (self->delay_us);
      g_thread_yield ();
    }
}

static void
slow_adopt_path (SlowBackend *self, PhodavPath *path)
{
  if (!path)
    return;

  phodav_backend_unref (path->backend);
  path->backend = phodav_backend_ref (&self->parent);
}

static PhodavPath *
slow_resolve (PhodavBackend *backend,
              PathHandler *handler,
              const gchar *relative,
              PhodavPathMode mode,
              gint *status)
{
  SlowBackend *self = (SlowBackend *) backend;
  PhodavPath *path;

  slow_delay (self);
  path = self->inner->klass->resolve (self->inner, handler, relative,
                                      mode, status);
  slow_adopt_path (self, path);
  return path;
}

static PhodavPath *
slow_resolve_child (PhodavBackend *backend,
                    PhodavPath *parent,
                    const gchar *name,
                    PhodavPathMode mode,
                    gint *status)
{
  SlowBackend *self = (SlowBackend *) backend;
  PhodavPath *path;

  slow_delay (self);
  path = self->inner->klass->resolve_child (self->inner, parent, name,
                                            mode, status);
  slow_adopt_path (self, path);
  return path;
}

static GFileInfo *
slow_query_info (PhodavPath *path,
                 const gchar *attributes,
                 GFileQueryInfoFlags flags,
                 GCancellable *cancellable,
                 GError **error)
{
  SlowBackend *self = (SlowBackend *) path->backend;

  slow_delay (self);
  return self->inner->klass->query_info (path, attributes, flags,
                                         cancellable, error);
}

static GFileInputStream *
slow_open (PhodavPath *path,
           GCancellable *cancellable,
           GError **error)
{
  SlowBackend *self = (SlowBackend *) path->backend;

  slow_delay (self);
  return self->inner->klass->open (path, cancellable, error);
}

static GFileEnumerator *
slow_enumerate (PhodavPath *path,
                const gchar *attributes,
                GFileQueryInfoFlags flags,
                GCancellable *cancellable,
                GError **error)
{
  SlowBackend *self = (SlowBackend *) path->backend;

  slow_delay (self);
  return self->inner->klass->enumerate (path, attributes, flags,
                                        cancellable, error);
}

static gboolean
slow_mkdir (PhodavPath *path,
            GCancellable *cancellable,
            GError **error)
{
  SlowBackend *self = (SlowBackend *) path->backend;

  slow_delay (self);
  return self->inner->klass->mkdir (path, cancellable, error);
}

static gboolean
slow_unlink (PhodavPath *path,
             GCancellable *cancellable,
             GError **error)
{
  SlowBackend *self = (SlowBackend *) path->backend;

  slow_delay (self);
  return self->inner->klass->unlink (path, cancellable, error);
}

static gboolean
slow_unlink_child (PhodavPath *parent,
                   const gchar *name,
                   GCancellable *cancellable,
                   GError **error)
{
  SlowBackend *self = (SlowBackend *) parent->backend;

  slow_delay (self);
  return self->inner->klass->unlink_child (parent, name, cancellable, error);
}

static gboolean
slow_copy_file (PhodavPath *from,
                PhodavPath *to,
                GFileCopyFlags flags,
                GCancellable *cancellable,
                GError **error)
{
  SlowBackend *self = (SlowBackend *) from->backend;

  slow_delay (self);
  return self->inner->klass->copy_file (from, to, flags,
                                        cancellable, error);
}

static gboolean
slow_move (PhodavPath *from,
           PhodavPath *to,
           GFileCopyFlags flags,
           GCancellable *cancellable,
           GError **error)
{
  SlowBackend *self = (SlowBackend *) from->backend;

  slow_delay (self);
  return self->inner->klass->move (from, to, flags, cancellable, error);
}

static void
slow_destroy (PhodavBackend *backend)
{
  SlowBackend *self = (SlowBackend *) backend;

  phodav_backend_unref (self->inner);
}

static const PhodavBackendClass slow_backend_class = {
  .name = "slow-test",
  .resolve = slow_resolve,
  .resolve_child = slow_resolve_child,
  .query_info = slow_query_info,
  .open = slow_open,
  .enumerate = slow_enumerate,
  .mkdir = slow_mkdir,
  .unlink = slow_unlink,
  .unlink_child = slow_unlink_child,
  .copy_file = slow_copy_file,
  .move = slow_move,
  .destroy = slow_destroy,
};

/* @inner is consumed */
static PhodavBackend *
slow_backend_new (PhodavBackend *inner, guint delay_us)
{
  SlowBackend *self = g_new0 (SlowBackend, 1);

  self->parent.klass = &slow_backend_class;
  self->parent.ref_count = 1;
  self->parent.root = g_object_ref (inner->root);
  self->inner = inner;
  self->delay_us = delay_us;

  return &self->parent;
}

static void
slow_backend_install (PhodavServer *server)
{
  PathHandler *handler = server->root_handler;
  PhodavBackend *inner;

  g_assert_nonnull (handler);
  inner = phodav_backend_ref (handler->backend);
  phodav_backend_unref (handler->backend);
  handler->backend = slow_backend_new (inner, SLOW_DELAY_US);
}

typedef struct _StressFixture
{
  gchar *dir;
  gchar *base_uri;
  PhodavServer *server;
  GMainContext *context;
  GMainLoop *loop;
  GThread *server_thread;
  gboolean thread_done;
} StressFixture;

static gpointer
stress_server_thread (gpointer data)
{
  StressFixture *f = data;

  g_main_context_push_thread_default (f->context);
  g_main_loop_run (f->loop);
  g_main_context_pop_thread_default (f->context);
  g_atomic_int_set (&f->thread_done, TRUE);

  return NULL;
}

static SoupMessage *
stress_request (SoupSession *session,
                const gchar *base_uri,
                const gchar *method,
                const gchar *path,
                const gchar *body,
                GBytes **response_body,
                const gchar *header,
                const gchar *value)
{
  g_autofree gchar *uri = g_build_path ("/", base_uri, path, NULL);
  g_autoptr(SoupMessage) msg = soup_message_new (method, uri);
  g_autoptr(GBytes) request = NULL;
  g_autoptr(GError) error = NULL;
  GBytes *response;

  g_assert_nonnull (msg);

  if (header)
    soup_message_headers_append (soup_message_get_request_headers (msg),
                                 header, value);

  if (body)
    {
      request = g_bytes_new (body, strlen (body));
      soup_message_set_request_body_from_bytes (msg, NULL, request);
    }

  response = soup_session_send_and_read (session, msg, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response);

  if (response_body)
    *response_body = response;
  else
    g_bytes_unref (response);

  return g_steal_pointer (&msg);
}

static guint
stress_status (SoupSession *session,
               const gchar *base_uri,
               const gchar *method,
               const gchar *path,
               const gchar *body,
               const gchar *header,
               const gchar *value)
{
  g_autoptr(SoupMessage) msg =
    stress_request (session, base_uri, method, path, body, NULL, header, value);

  return soup_message_get_status (msg);
}

typedef struct _ClientData
{
  StressFixture *fixture;
  guint id;
} ClientData;

static gpointer
stress_client_thread (gpointer data)
{
  ClientData *client = data;
  StressFixture *f = client->fixture;
  g_autoptr(SoupSession) session = soup_session_new_with_options ("timeout", 20, NULL);
  guint i;

  for (i = 0; i < CLIENT_ITERATIONS; i++)
    {
      g_autofree gchar *file = g_strdup_printf ("/client-%u-%u.txt", client->id, i);
      g_autofree gchar *dir = g_strdup_printf ("/client-%u-%u", client->id, i);
      g_autofree gchar *payload = g_strdup_printf ("client %u iteration %u\n",
                                                   client->id, i);
      g_autoptr(GBytes) body = NULL;
      g_autoptr(SoupMessage) msg = NULL;
      g_autofree gchar *token = NULL;

      g_assert_cmpuint (stress_status (session, f->base_uri, SOUP_METHOD_GET,
                                       "/hello.txt", NULL, NULL, NULL),
                        ==, SOUP_STATUS_OK);

      g_assert_cmpuint (stress_status (session, f->base_uri, "PROPFIND", "/",
                                       PROPFIND_LOCKDISCOVERY, "Depth", "1"),
                        ==, SOUP_STATUS_MULTI_STATUS);

      g_assert_cmpuint (stress_status (session, f->base_uri, SOUP_METHOD_PUT,
                                       file, payload, NULL, NULL),
                        ==, SOUP_STATUS_CREATED);

      msg = stress_request (session, f->base_uri, SOUP_METHOD_GET, file,
                            NULL, &body, NULL, NULL);
      g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
      g_assert_cmpuint (g_bytes_get_size (body), ==, strlen (payload));
      g_assert_cmpmem (g_bytes_get_data (body, NULL), g_bytes_get_size (body),
                       payload, strlen (payload));

      g_clear_object (&msg);
      msg = stress_request (session, f->base_uri, "LOCK", file, LOCK_EXCLUSIVE,
                            NULL, "Depth", "0");
      g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
      token = g_strdup (soup_message_headers_get_one (
        soup_message_get_response_headers (msg), "Lock-Token"));
      g_assert_nonnull (token);
      g_assert_true (g_str_has_prefix (token, "<urn:uuid:"));

      g_assert_cmpuint (stress_status (session, f->base_uri, "PROPFIND", "/",
                                       PROPFIND_LOCKDISCOVERY, "Depth", "1"),
                        ==, SOUP_STATUS_MULTI_STATUS);

      g_assert_cmpuint (stress_status (session, f->base_uri, "UNLOCK", file,
                                       NULL, "Lock-Token", token),
                        ==, SOUP_STATUS_NO_CONTENT);

      g_assert_cmpuint (stress_status (session, f->base_uri, "MKCOL", dir,
                                       NULL, NULL, NULL),
                        ==, SOUP_STATUS_CREATED);
      g_assert_cmpuint (stress_status (session, f->base_uri, "PROPFIND", dir,
                                       PROPFIND_LOCKDISCOVERY, "Depth", "1"),
                        ==, SOUP_STATUS_MULTI_STATUS);
      g_assert_cmpuint (stress_status (session, f->base_uri, SOUP_METHOD_DELETE,
                                       dir, NULL, "Depth", "infinity"),
                        ==, SOUP_STATUS_NO_CONTENT);

      g_assert_cmpuint (stress_status (session, f->base_uri, SOUP_METHOD_DELETE,
                                       file, NULL, "Depth", "infinity"),
                        ==, SOUP_STATUS_NO_CONTENT);
    }

  return NULL;
}

typedef struct _WriterData
{
  gchar *path;
  gint stop;
} WriterData;

static gpointer
stress_writer_thread (gpointer data)
{
  WriterData *writer = data;
  g_autoptr(GFile) file = g_file_new_for_path (writer->path);
  guint i = 0;

  while (!g_atomic_int_get (&writer->stop))
    {
      g_autofree gchar *contents = g_strdup_printf ("external write %u\n", i++);
      g_autoptr(GError) error = NULL;

      if (!g_file_replace_contents (file, contents, strlen (contents), NULL,
                                    FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error))
        g_debug ("external writer: %s", error->message);

      g_usleep (1000);
    }

  return NULL;
}

static void
remove_recursive (GFile *file)
{
  g_autoptr(GFileEnumerator) e = NULL;

  e = g_file_enumerate_children (file, "standard::*",
                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (e)
    {
      while (TRUE)
        {
          g_autoptr(GFileInfo) info = g_file_enumerator_next_file (e, NULL, NULL);
          g_autoptr(GFile) child = NULL;

          if (!info)
            break;

          child = g_file_get_child (file, g_file_info_get_name (info));
          remove_recursive (child);
        }
    }

  g_file_delete (file, NULL, NULL);
}

static void
test_concurrency_stress (void)
{
  StressFixture f = {
    0,
  };
  g_autoptr(GError) error = NULL;
  g_autoslist(GUri) uris = NULL;
  g_autoptr(GFile) root = NULL;
  GThread **clients = NULL;
  g_autoptr(GThread) writer_thread = NULL;
  ClientData client_data[CLIENT_COUNT];
  WriterData writer = {
    0,
  };
  g_autofree gchar *hello = NULL;
  guint i;

  f.dir = g_dir_make_tmp ("phodav-stress-XXXXXX", &error);
  g_assert_no_error (error);

  hello = g_build_filename (f.dir, "hello.txt", NULL);
  g_assert_true (g_file_set_contents (hello, HELLO_CONTENT, -1, &error));
  g_assert_no_error (error);

  f.context = g_main_context_new ();
  g_main_context_push_thread_default (f.context);

  f.server = g_object_new (PHODAV_TYPE_SERVER, "root", f.dir, NULL);
  slow_backend_install (f.server);

  {
    SoupServer *soup = phodav_server_get_soup_server (f.server);

    g_assert_true (soup_server_listen_local (soup, 0,
                                             SOUP_SERVER_LISTEN_IPV4_ONLY,
                                             &error));
    g_assert_no_error (error);

    uris = soup_server_get_uris (soup);
    g_assert_nonnull (uris);
    f.base_uri = g_uri_to_string (uris->data);
  }

  f.loop = g_main_loop_new (f.context, FALSE);
  g_main_context_pop_thread_default (f.context);
  f.server_thread = g_thread_new ("stress-server", stress_server_thread, &f);

  writer.path = g_build_filename (f.dir, "external.txt", NULL);
  writer_thread = g_thread_new ("stress-writer", stress_writer_thread, &writer);

  clients = g_new0 (GThread *, CLIENT_COUNT);
  for (i = 0; i < CLIENT_COUNT; i++)
    {
      client_data[i].fixture = &f;
      client_data[i].id = i;
      clients[i] = g_thread_new ("stress-client", stress_client_thread,
                                 &client_data[i]);
    }

  for (i = 0; i < CLIENT_COUNT; i++)
    g_thread_join (clients[i]);

  g_atomic_int_set (&writer.stop, TRUE);
  g_thread_join (writer_thread);

  /* the server must still be usable after the concurrent load */
  {
    g_autoptr(SoupSession) session = soup_session_new_with_options ("timeout", 20, NULL);
    g_autoptr(GBytes) body = NULL;
    g_autoptr(SoupMessage) msg =
      stress_request (session, f.base_uri, SOUP_METHOD_GET, "/hello.txt",
                      NULL, &body, NULL, NULL);

    g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
    g_assert_cmpuint (g_bytes_get_size (body), ==, strlen (HELLO_CONTENT));
    g_assert_cmpmem (g_bytes_get_data (body, NULL), g_bytes_get_size (body),
                     HELLO_CONTENT, strlen (HELLO_CONTENT));
  }

  g_main_loop_quit (f.loop);
  g_thread_join (f.server_thread);
  g_assert_true (f.thread_done);

  g_clear_pointer (&clients, g_free);
  g_clear_pointer (&writer.path, g_free);
  g_clear_pointer (&writer_thread, g_thread_unref);
  g_clear_pointer (&f.base_uri, g_free);
  g_clear_pointer (&f.loop, g_main_loop_unref);
  g_clear_object (&f.server);
  g_clear_pointer (&f.context, g_main_context_unref);

  root = g_file_new_for_path (f.dir);
  remove_recursive (root);
  g_clear_pointer (&f.dir, g_free);
}

gint
main (gint argc, gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/server/concurrency-stress", test_concurrency_stress);

  return g_test_run ();
}
