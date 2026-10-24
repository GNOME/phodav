/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/* End-to-end tests for the WebDAV methods, using an in-process
 * PhodavServer and a SoupSession client. This complements the litmus
 * compliance suite with checks of the phodav specific behaviour. */

#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#ifdef G_OS_UNIX
#include <signal.h>
#include <sys/resource.h>
#endif

#include "libphodav/phodav.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC (xmlDoc, xmlFreeDoc)

#define HELLO_CONTENT "Hello, phodav!\n"
#define HELLO_LENGTH 15

#define PROPFIND_ALLPROP "<D:propfind xmlns:D=\"DAV:\"><D:allprop/></D:propfind>"
#define PROPFIND_PROPNAME "<D:propfind xmlns:D=\"DAV:\"><D:propname/></D:propfind>"
#define PROPFIND_LENGTH_AND_ETAG \
  "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:getcontentlength/><D:getetag/></D:prop></D:propfind>"
#define PROPFIND_UNKNOWN \
  "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:getcontentlength/><D:nonexistent/></D:prop></D:propfind>"
#define PROPPATCH_SET                                  \
  "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop>" \
  "<user.phodav-test>test value</user.phodav-test>"    \
  "</D:prop></D:set></D:propertyupdate>"
#define PROPPATCH_REMOVE                                  \
  "<D:propertyupdate xmlns:D=\"DAV:\"><D:remove><D:prop>" \
  "<user.phodav-test/>"                                   \
  "</D:prop></D:remove></D:propertyupdate>"
#define PROPFIND_DEAD_PROP \
  "<D:propfind xmlns:D=\"DAV:\"><D:prop><user.phodav-test/></D:prop></D:propfind>"
#define LOCK_EXCLUSIVE                                                     \
  "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:exclusive/></D:lockscope>" \
  "<D:locktype><D:write/></D:locktype>"                                    \
  "<D:owner>unit-test</D:owner></D:lockinfo>"

typedef struct _DavFixture
{
  gchar *dir;
  PhodavServer *phodav;
  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
  SoupSession *session;
  gchar *base_uri;
  gboolean thread_done;
} DavFixture;

static void
write_file (const gchar *dir, const gchar *name, const gchar *contents)
{
  g_autofree gchar *path = g_build_filename (dir, name, NULL);

  g_assert_true (g_file_set_contents (path, contents, -1, NULL));
}

/* the file of a PUT is committed before its response is written:
 * once the client has seen the response, the content is settled */
static void
assert_file_contents (const gchar *path, const gchar *expected)
{
  g_autofree gchar *contents = NULL;

  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, expected);
}

/* an upload stages its content in a .phodav-*.tmp file next to the
 * destination; none should survive a request */
static void
assert_no_upload_temp_files (const gchar *directory)
{
  GDir *dir = g_dir_open (directory, 0, NULL);
  const gchar *name;

  g_assert_nonnull (dir);
  while ((name = g_dir_read_name (dir)) != NULL)
    g_assert_false (g_str_has_prefix (name, ".phodav-"));
  g_dir_close (dir);
}

static void
make_directory (const gchar *dir, const gchar *name)
{
  g_autofree gchar *path = g_build_filename (dir, name, NULL);

  g_assert_cmpint (g_mkdir (path, 0700), ==, 0);
}

static void
remove_recursive (GFile *file)
{
  g_autoptr(GFileEnumerator) e = NULL;

  e = g_file_enumerate_children (file, "standard::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (e)
    {
      while (TRUE)
        {
          GFileInfo *info = g_file_enumerator_next_file (e, NULL, NULL);

          if (!info)
            break;

          {
            GFile *child = g_file_get_child (file, g_file_info_get_name (info));

            remove_recursive (child);
            g_object_unref (child);
          }
          g_object_unref (info);
        }
    }

  g_file_delete (file, NULL, NULL);
}

static gpointer
server_thread (gpointer data)
{
  DavFixture *f = data;

  g_main_context_push_thread_default (f->context);
  g_main_loop_run (f->loop);
  g_main_context_pop_thread_default (f->context);
  g_atomic_int_set (&f->thread_done, TRUE);

  return NULL;
}

/* the server options a test may set on the fixture server */
typedef struct _DavOptions
{
  gboolean readonly;
  guint64 max_upload;
} DavOptions;

static void
dav_fixture_setup_full (DavFixture *f, const DavOptions *options)
{
  g_autoptr(GError) error = NULL;
  g_autoslist(GUri) uris = NULL;

  f->dir = g_dir_make_tmp ("phodav-dav-XXXXXX", &error);
  g_assert_no_error (error);

  write_file (f->dir, "hello.txt", HELLO_CONTENT);
  make_directory (f->dir, "dir");
  write_file (f->dir, "dir/inner.txt", "inner\n");

  /* the server runs its own context and thread: the synchronous
   * client blocks on the socket while waiting for the response, so
   * it cannot share a context with the server */
  f->context = g_main_context_new ();
  g_main_context_push_thread_default (f->context);

  f->phodav = g_object_new (PHODAV_TYPE_SERVER,
                            "root", f->dir,
                            "read-only", options->readonly,
                            "max-upload", options->max_upload,
                            NULL);

  {
    SoupServer *server = phodav_server_get_soup_server (f->phodav);

    g_assert_true (soup_server_listen_local (server, 0,
                                             SOUP_SERVER_LISTEN_IPV4_ONLY,
                                             &error));
    g_assert_no_error (error);

    uris = soup_server_get_uris (server);
    g_assert_nonnull (uris);
    f->base_uri = g_uri_to_string (uris->data);
  }

  f->loop = g_main_loop_new (f->context, FALSE);
  g_main_context_pop_thread_default (f->context);
  f->thread = g_thread_new ("dav-server", server_thread, f);

  f->session = soup_session_new ();
}

static void
dav_fixture_setup (DavFixture *f, gconstpointer user_data)
{
  DavOptions options = { .readonly = user_data != NULL };

  dav_fixture_setup_full (f, &options);
}

/* a fixture server which refuses the PUT uploads over the limit
 * passed as the test user data */
static void
dav_fixture_setup_max_upload (DavFixture *f, gconstpointer user_data)
{
  DavOptions options = { .max_upload = GPOINTER_TO_SIZE (user_data) };

  dav_fixture_setup_full (f, &options);
}

/* a foreign handler of the user-provided SoupServer, at a more
 * specific path than the phodav root */
static void
foreign_handler_cb (SoupServer *server, SoupServerMessage *msg, const gchar *path, GHashTable *query, gpointer user_data)
{
  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
}

/* a fixture server sharing the SoupServer of the embedding
 * application, which carries a foreign handler of its own */
static void
dav_fixture_setup_shared (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoslist(GUri) uris = NULL;
  g_autoptr(SoupServer) server = NULL;

  f->dir = g_dir_make_tmp ("phodav-dav-XXXXXX", &error);
  g_assert_no_error (error);

  write_file (f->dir, "hello.txt", HELLO_CONTENT);

  /* the server runs its own context and thread: the synchronous
   * client blocks on the socket while waiting for the response, so
   * it cannot share a context with the server */
  f->context = g_main_context_new ();
  g_main_context_push_thread_default (f->context);

  server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, "/upload", foreign_handler_cb, NULL, NULL);

  f->phodav = g_object_new (PHODAV_TYPE_SERVER,
                            "root", f->dir,
                            "server", server,
                            NULL);

  g_assert_true (phodav_server_get_soup_server (f->phodav) == server);

  {
    g_assert_true (soup_server_listen_local (server, 0,
                                             SOUP_SERVER_LISTEN_IPV4_ONLY,
                                             &error));
    g_assert_no_error (error);

    uris = soup_server_get_uris (server);
    g_assert_nonnull (uris);
    f->base_uri = g_uri_to_string (uris->data);
  }

  f->loop = g_main_loop_new (f->context, FALSE);
  g_main_context_pop_thread_default (f->context);
  f->thread = g_thread_new ("dav-server", server_thread, f);

  f->session = soup_session_new ();
}

static void
dav_fixture_teardown (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GFile) file = NULL;
  guint i;

  g_object_unref (f->session);

  /* quitting a loop which is not running yet is a lost wakeup, keep
   * quitting until the thread has actually left the loop */
  for (i = 0; i < 3000 && !g_atomic_int_get (&f->thread_done); i++)
    {
      g_main_loop_quit (f->loop);
      g_usleep (1000);
    }
  g_assert_true (f->thread_done);
  g_thread_join (f->thread);
  g_main_loop_unref (f->loop);

  g_object_unref (f->phodav);
  g_main_context_unref (f->context);

  file = g_file_new_for_path (f->dir);
  remove_recursive (file);

  g_free (f->base_uri);
  g_free (f->dir);
}

/* Send a request with an optional body and a NULL-terminated list of
 * extra headers (name, value pairs). Returns the message; the response
 * body is returned too when @response_body is given. */
static SoupMessage *dav_request (DavFixture *f,
                                 const gchar *method,
                                 const gchar *path,
                                 const gchar *body,
                                 GBytes **response_body,
                                 ...) G_GNUC_NULL_TERMINATED;

static SoupMessage *
dav_request (DavFixture *f, const gchar *method, const gchar *path, const gchar *body, GBytes **response_body, ...)
{
  g_autofree gchar *uri = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) error = NULL;
  va_list ap;
  const gchar *name;

  uri = g_build_path ("/", f->base_uri, path, NULL);
  msg = soup_message_new (method, uri);

  va_start (ap, response_body);
  while ((name = va_arg (ap, const gchar *)))
    {
      const gchar *value = va_arg (ap, const gchar *);

      g_assert_nonnull (value);
      soup_message_headers_append (soup_message_get_request_headers (msg), name, value);
    }
  va_end (ap);

  if (body)
    {
      g_autoptr(GBytes) request = g_bytes_new (body, strlen (body));

      soup_message_set_request_body_from_bytes (msg, NULL, request);
    }

  bytes = soup_session_send_and_read (f->session, msg, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (bytes);

  if (response_body)
    *response_body = g_steal_pointer (&bytes);

  return g_steal_pointer (&msg);
}

static guint
dav_status (DavFixture *f, const gchar *method, const gchar *path, const gchar *body, ...)
{
  g_autoptr(SoupMessage) msg = NULL;
  va_list ap;
  const gchar *name;

  {
    g_autofree gchar *uri = g_build_path ("/", f->base_uri, path, NULL);

    msg = soup_message_new (method, uri);
  }

  va_start (ap, body);
  while ((name = va_arg (ap, const gchar *)))
    {
      const gchar *value = va_arg (ap, const gchar *);

      g_assert_nonnull (value);
      soup_message_headers_append (soup_message_get_request_headers (msg), name, value);
    }
  va_end (ap);

  if (body)
    {
      g_autoptr(GBytes) request = g_bytes_new (body, strlen (body));

      soup_message_set_request_body_from_bytes (msg, NULL, request);
    }

  {
    g_autoptr(GBytes) bytes = soup_session_send_and_read (f->session, msg, NULL, NULL);

    g_assert_nonnull (bytes);
  }

  return soup_message_get_status (msg);
}

static xmlNodePtr
find_child (xmlNodePtr parent, const gchar *name)
{
  for (xmlNodePtr node = parent->children; node; node = node->next)
    if (node->type == XML_ELEMENT_NODE && g_strcmp0 ((gchar *) node->name, name) == 0)
      return node;

  return NULL;
}

static gchar *
node_content (xmlNodePtr node)
{
  xmlChar *content = xmlNodeGetContent (node);
  gchar *str = g_strdup ((gchar *) content);

  xmlFree (content);
  return str;
}

static xmlDocPtr
parse_multistatus (GBytes *body)
{
  gsize size;
  gconstpointer data = g_bytes_get_data (body, &size);
  xmlDocPtr doc = xmlReadMemory (data, size, "response.xml", NULL,
                                 XML_PARSE_NONET | XML_PARSE_NOWARNING |
                                   XML_PARSE_NOBLANKS | XML_PARSE_NSCLEAN);

  g_assert_nonnull (doc);
  g_assert_cmpstr ((gchar *) xmlDocGetRootElement (doc)->name, ==, "multistatus");

  return doc;
}

/* find the <D:response> element of a multistatus for the resource at
 * @path, NULL if missing */
static xmlNodePtr
find_response (DavFixture *f, xmlDocPtr doc, const gchar *path)
{
  gsize base_len = strlen (f->base_uri);

  /* the base URI ends with "/", keep the "/" starting the path */
  if (base_len > 0 && f->base_uri[base_len - 1] == '/')
    base_len--;

  for (xmlNodePtr node = xmlDocGetRootElement (doc)->children; node; node = node->next)
    {
      xmlNodePtr href = find_child (node, "href");
      g_autofree gchar *content = NULL;
      g_autofree gchar *decoded = NULL;

      if (g_strcmp0 ((gchar *) node->name, "response") != 0 || !href)
        continue;

      content = node_content (href);

      if (!g_str_has_prefix (content, f->base_uri))
        continue;

      /* the href is the full request URI with its path escaped: with
       * the base stripped and the path unescaped, it compares equal
       * to the request path */
      decoded = g_uri_unescape_string (content + base_len, "/");
      if (decoded && g_strcmp0 (decoded, path) == 0)
        return node;
    }

  return NULL;
}

/* find the <D:propstat> of @response holding a <D:prop> child named @name */
static xmlNodePtr
find_propstat (xmlNodePtr response, const gchar *name)
{
  for (xmlNodePtr node = response->children; node; node = node->next)
    {
      xmlNodePtr prop = find_child (node, "prop");

      if (g_strcmp0 ((gchar *) node->name, "propstat") == 0 &&
          prop && find_child (prop, name))
        return node;
    }

  return NULL;
}

static guint
propstat_status (xmlNodePtr propstat)
{
  g_autofree gchar *content = node_content (find_child (propstat, "status"));
  gint status = 0;

  g_assert_nonnull (content);
  g_assert_true (sscanf (content, "HTTP/%*d.%*d %d", &status) == 1);

  return status;
}

static xmlDocPtr
parse_prop (GBytes *body)
{
  gsize size;
  gconstpointer data = g_bytes_get_data (body, &size);
  xmlDocPtr doc = xmlReadMemory (data, size, "response.xml", NULL,
                                 XML_PARSE_NONET | XML_PARSE_NOWARNING |
                                   XML_PARSE_NOBLANKS | XML_PARSE_NSCLEAN);

  g_assert_nonnull (doc);
  g_assert_cmpstr ((gchar *) xmlDocGetRootElement (doc)->name, ==, "prop");

  return doc;
}

static xmlNodePtr
find_activelock (xmlDocPtr doc)
{
  xmlNodePtr discovery = find_child (xmlDocGetRootElement (doc), "lockdiscovery");

  g_assert_nonnull (discovery);
  return find_child (discovery, "activelock");
}

/* status of a property in a multistatus response, 0 when absent */
static guint
prop_status (DavFixture *f, xmlDocPtr doc, const gchar *path, const gchar *name)
{
  xmlNodePtr response = find_response (f, doc, path);
  xmlNodePtr propstat;

  if (!response)
    return 0;

  propstat = find_propstat (response, name);

  return propstat ? propstat_status (propstat) : 0;
}

static gchar *
prop_value (DavFixture *f, xmlDocPtr doc, const gchar *path, const gchar *name)
{
  xmlNodePtr response = find_response (f, doc, path);
  xmlNodePtr propstat;

  g_assert_nonnull (response);
  propstat = find_propstat (response, name);
  g_assert_nonnull (propstat);
  g_assert_cmpuint (propstat_status (propstat), ==, SOUP_STATUS_OK);

  return node_content (find_child (find_child (propstat, "prop"), name));
}

static void
test_options (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(SoupMessage) msg = dav_request (f, SOUP_METHOD_OPTIONS, "/", NULL, NULL, NULL);

  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "DAV"), ==, "1,2");
  g_assert_nonnull (soup_message_headers_get_one (soup_message_get_response_headers (msg), "MS-Author-Via"));

  /* the Allow header is generated from the method table: it must
   * list every method of the table, comma separated, and nothing
   * else */
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Allow"),
                   ==, "GET, HEAD, PUT, PROPFIND, PROPPATCH, MKCOL, DELETE, MOVE, COPY, LOCK, UNLOCK");
}

static void
test_get (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;

  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  g_assert_cmpuint (g_bytes_get_size (body), ==, HELLO_LENGTH);
  g_assert_cmpmem (g_bytes_get_data (body, NULL), HELLO_LENGTH, HELLO_CONTENT, HELLO_LENGTH);

  /* directories are listed as HTML */
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, SOUP_METHOD_GET, "/dir", NULL, &body,
                     "Accept", "text/html", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  {
    const gchar *type;
    GHashTable *params = NULL;
    SoupMessageHeaders *response_headers = soup_message_get_response_headers (msg);

    type = soup_message_headers_get_content_type (response_headers, &params);
    g_assert_cmpstr (type, ==, "text/html");
    g_clear_pointer (&params, g_hash_table_unref);

    /* the listing is not cacheable, must not be mistaken for another
     * content type, and nothing is ever loaded from it */
    g_assert_cmpstr (soup_message_headers_get_one (response_headers, "Cache-Control"), ==, "no-cache");
    g_assert_cmpstr (soup_message_headers_get_one (response_headers, "X-Content-Type-Options"), ==, "nosniff");
    g_assert_cmpstr (soup_message_headers_get_one (response_headers, "Content-Security-Policy"), ==, "default-src 'none'");
  }

  g_assert_cmpuint (dav_status (f, SOUP_METHOD_HEAD, "/hello.txt", NULL, NULL), ==, SOUP_STATUS_OK);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/missing.txt", NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
}

static void
test_get_conditional (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autofree gchar *etag = NULL;
  g_autofree gchar *weak = NULL;
  g_autofree gchar *list = NULL;
  g_autofree gchar *last_modified = NULL;

  /* learn the validators the way a client would */
  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  {
    SoupMessageHeaders *response_headers = soup_message_get_response_headers (msg);

    g_set_str (&etag, soup_message_headers_get_one (response_headers, "ETag"));
    g_set_str (&last_modified, soup_message_headers_get_one (response_headers, "Last-Modified"));
    g_assert_nonnull (etag);
    g_assert_nonnull (last_modified);
    g_assert_cmpstr (soup_message_headers_get_one (response_headers, "Cache-Control"), ==, "no-cache");
  }

  /* an unchanged validator is answered with 304 Not Modified, with
   * the validators still advertised and no body, RFC 9110 13.1.2 */
  g_clear_object (&msg);
  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, &body,
                     "If-None-Match", etag, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_NOT_MODIFIED);
  g_assert_cmpuint (g_bytes_get_size (body), ==, 0);
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "ETag"), ==, etag);
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Last-Modified"), ==, last_modified);

  /* the comparison is weak, and a list matches when any member does */
  weak = g_strdup_printf ("W/%s", etag);
  list = g_strdup_printf ("\"not-the-etag\", %s", etag);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "If-None-Match", weak, NULL),
                    ==, SOUP_STATUS_NOT_MODIFIED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "If-None-Match", list, NULL),
                    ==, SOUP_STATUS_NOT_MODIFIED);

  /* the star matches any existing resource */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "If-None-Match", "*", NULL),
                    ==, SOUP_STATUS_NOT_MODIFIED);

  /* a non-matching validator falls back to a full response */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "If-None-Match", "\"not-the-etag\"", NULL),
                    ==, SOUP_STATUS_OK);

  /* an unmodified resource is answered with 304 too, RFC 9110 13.1.3 */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "If-Modified-Since", last_modified, NULL),
                    ==, SOUP_STATUS_NOT_MODIFIED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "If-Modified-Since", "Thu, 01 Jan 1970 00:00:00 GMT", NULL),
                    ==, SOUP_STATUS_OK);

  /* If-None-Match takes precedence over If-Modified-Since */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "If-None-Match", "\"not-the-etag\"",
                                "If-Modified-Since", last_modified, NULL),
                    ==, SOUP_STATUS_OK);

  /* HEAD is conditional in the same way */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_HEAD, "/hello.txt", NULL,
                                "If-None-Match", etag, NULL),
                    ==, SOUP_STATUS_NOT_MODIFIED);

  /* a missing resource is not compared with the star */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/missing.txt", NULL,
                                "If-None-Match", "*", NULL),
                    ==, SOUP_STATUS_NOT_FOUND);
}

static void
test_get_range (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autofree gchar *etag = NULL;
  g_autofree gchar *weak = NULL;

  /* hello.txt holds "Hello, phodav!\n" (15 bytes) */
  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL, NULL);
  g_set_str (&etag, soup_message_headers_get_one (soup_message_get_response_headers (msg), "ETag"));
  g_assert_nonnull (etag);
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Accept-Ranges"), ==, "bytes");
  weak = g_strdup_printf ("W/%s", etag);

  /* a satisfiable range is answered with the slice and a
   * Content-Range of the whole representation, RFC 9110 14.2 */
  g_clear_object (&msg);
  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, &body,
                     "Range", "bytes=0-4", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_PARTIAL_CONTENT);
  g_assert_cmpuint (g_bytes_get_size (body), ==, 5);
  g_assert_cmpmem (g_bytes_get_data (body, NULL), 5, "Hello", 5);
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Content-Range"), ==, "bytes 0-4/15");

  /* open-ended and suffix ranges */
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, &body,
                     "Range", "bytes=7-", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_PARTIAL_CONTENT);
  g_assert_cmpuint (g_bytes_get_size (body), ==, 8);
  g_assert_cmpmem (g_bytes_get_data (body, NULL), 8, "phodav!\n", 8);
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Content-Range"), ==, "bytes 7-14/15");

  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, &body,
                     "Range", "bytes=-7", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_PARTIAL_CONTENT);
  g_assert_cmpuint (g_bytes_get_size (body), ==, 7);
  g_assert_cmpmem (g_bytes_get_data (body, NULL), 7, "hodav!\n", 7);
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Content-Range"), ==, "bytes 8-14/15");

  /* a last byte beyond the end is clamped to it */
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, &body,
                     "Range", "bytes=0-100", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_PARTIAL_CONTENT);
  g_assert_cmpuint (g_bytes_get_size (body), ==, HELLO_LENGTH);
  g_assert_cmpmem (g_bytes_get_data (body, NULL), HELLO_LENGTH, HELLO_CONTENT, HELLO_LENGTH);
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Content-Range"), ==, "bytes 0-14/15");

  /* a first byte at or beyond the end is unsatisfiable, and the
   * actual length is reported, RFC 9110 section 14.2 */
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, &body,
                     "Range", "bytes=15-", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE);
  g_assert_cmpuint (g_bytes_get_size (body), ==, 0);
  g_assert_cmpstr (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Content-Range"), ==, "bytes */15");
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "Range", "bytes=-0", NULL),
                    ==, SOUP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE);

  /* invalid, multi-range or unknown-unit requests fall back to the
   * whole representation */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "Range", "bytes=5-2", NULL),
                    ==, SOUP_STATUS_OK);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "Range", "bytes=0-4,10-14", NULL),
                    ==, SOUP_STATUS_OK);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "Range", "items=0-4", NULL),
                    ==, SOUP_STATUS_OK);

  /* If-Range applies the range only while the client holds the
   * current representation, RFC 9110 section 13.1.5 */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "Range", "bytes=0-4",
                                "If-Range", etag, NULL),
                    ==, SOUP_STATUS_PARTIAL_CONTENT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "Range", "bytes=0-4",
                                "If-Range", "\"not-the-etag\"", NULL),
                    ==, SOUP_STATUS_OK);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL,
                                "Range", "bytes=0-4",
                                "If-Range", weak, NULL),
                    ==, SOUP_STATUS_OK);

  /* Range is ignored for HEAD, which describes the whole file */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_HEAD, "/hello.txt", NULL,
                                "Range", "bytes=0-4", NULL),
                    ==, SOUP_STATUS_OK);

  /* an empty file has no satisfiable range, its empty body is
   * served unchanged */
  write_file (f->dir, "empty.txt", "");
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, SOUP_METHOD_GET, "/empty.txt", NULL, &body,
                     "Range", "bytes=0-", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  g_assert_cmpuint (g_bytes_get_size (body), ==, 0);
}

static void
test_put (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *path = g_build_filename (f->dir, "new.txt", NULL);
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;

  msg = dav_request (f, SOUP_METHOD_PUT, "/new.txt", "new content", NULL, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_CREATED);
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

  /* overwriting an existing resource is not a creation */
  g_clear_object (&msg);
  msg = dav_request (f, SOUP_METHOD_PUT, "/new.txt", "overwritten", NULL, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);

  g_clear_object (&msg);
  msg = dav_request (f, SOUP_METHOD_GET, "/new.txt", NULL, &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  {
    g_autofree gchar *text = g_strndup (g_bytes_get_data (body, NULL), g_bytes_get_size (body));

    g_assert_cmpstr (text, ==, "overwritten");
  }

  /* writing below a missing collection fails, RFC 4918 section 9.7.1 */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/missing-dir/new.txt", "data", NULL),
                    ==, SOUP_STATUS_CONFLICT);
  {
    g_autofree gchar *missing = g_build_filename (f->dir, "missing-dir", NULL);

    g_assert_false (g_file_test (missing, G_FILE_TEST_EXISTS));
  }

  /* a collection cannot be replaced with a PUT, RFC 4918 section 9.7.2 */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/dir", "data", NULL),
                    ==, SOUP_STATUS_METHOD_NOT_ALLOWED);
  {
    g_autofree gchar *inner = g_build_filename (f->dir, "dir/inner.txt", NULL);

    assert_file_contents (inner, "inner\n");
  }

  assert_no_upload_temp_files (f->dir);
}

static void
test_mkcol (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *path = g_build_filename (f->dir, "col", NULL);

  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MKCOL, "/col", NULL, NULL), ==, SOUP_STATUS_CREATED);
  g_assert_true (g_file_test (path, G_FILE_TEST_IS_DIR));

  /* it already exists */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MKCOL, "/col", NULL, NULL), ==, SOUP_STATUS_METHOD_NOT_ALLOWED);

  /* the parent collection is missing */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MKCOL, "/col/sub/deep", NULL, NULL), ==, SOUP_STATUS_CONFLICT);

  /* a body is not allowed */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MKCOL, "/col2", "body", NULL),
                    ==, SOUP_STATUS_UNSUPPORTED_MEDIA_TYPE);
}

static void
test_move_copy (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *dest = g_build_path ("/", f->base_uri, "moved.txt", NULL);

  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/file.txt", "data", NULL), ==, SOUP_STATUS_CREATED);

  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/file.txt", NULL,
                                "Destination", dest, NULL),
                    ==, SOUP_STATUS_CREATED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/file.txt", NULL, NULL), ==, SOUP_STATUS_OK);

  /* copying over an existing resource overwrites it */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/file.txt", NULL,
                                "Destination", dest, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);
  /* ... unless Overwrite is disabled */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/file.txt", NULL,
                                "Destination", dest, "Overwrite", "F", NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);

  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/file.txt", NULL,
                                "Destination", dest, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/file.txt", NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/moved.txt", NULL, NULL), ==, SOUP_STATUS_OK);

  /* the source does not exist */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/file.txt", NULL,
                                "Destination", dest, NULL),
                    ==, SOUP_STATUS_CONFLICT);
}

/* COPY and MOVE must not recurse into themselves: a destination which
 * is the source or one of its members would create an unbounded tree
 * of nested copies. */
static void
test_movecopy_self_containment (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *inside = g_build_path ("/", f->base_uri, "dir/sub", NULL);
  g_autofree gchar *inside_slash = g_strconcat (f->base_uri, "dir//sub", NULL);
  g_autofree gchar *same = g_build_path ("/", f->base_uri, "dir", NULL);
  g_autofree gchar *same_slash = g_strconcat (f->base_uri, "dir/", NULL);
  g_autofree gchar *partial = g_build_filename (f->dir, "dir", "sub", NULL);

  /* copying a collection into one of its members would recurse
   * endlessly, it must be refused before anything is written */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/dir", NULL,
                                "Destination", inside, "Depth", "infinity", NULL),
                    ==, SOUP_STATUS_CONFLICT);
  g_assert_false (g_file_test (partial, G_FILE_TEST_EXISTS));

  /* the same destination under another spelling of the path */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/dir", NULL,
                                "Destination", inside_slash, NULL),
                    ==, SOUP_STATUS_CONFLICT);

  /* the destination cannot be the source either */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/dir", NULL,
                                "Destination", same, NULL),
                    ==, SOUP_STATUS_CONFLICT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/dir/", NULL,
                                "Destination", same, NULL),
                    ==, SOUP_STATUS_CONFLICT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/dir", NULL,
                                "Destination", same_slash, NULL),
                    ==, SOUP_STATUS_CONFLICT);

  /* MOVE is recursive too */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/dir", NULL,
                                "Destination", inside, NULL),
                    ==, SOUP_STATUS_CONFLICT);

  /* replacing one of the source ancestors would delete the source
   * before it is copied or moved */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/dir/inner.txt", NULL,
                                "Destination", same, NULL),
                    ==, SOUP_STATUS_CONFLICT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/dir/inner.txt", NULL,
                                "Destination", same, NULL),
                    ==, SOUP_STATUS_CONFLICT);

  /* the source is untouched */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/dir/inner.txt", NULL, NULL),
                    ==, SOUP_STATUS_OK);
  g_assert_false (g_file_test (partial, G_FILE_TEST_EXISTS));
}

#ifdef G_OS_UNIX
/* a symbolic link in the destination path could point outside of the
 * shared directory: refuse to follow one */
static void
test_movecopy_symlink_destination (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *outside = g_dir_make_tmp ("phodav-outside-XXXXXX", NULL);
  g_autofree gchar *link_path = g_build_filename (f->dir, "link", NULL);
  g_autoptr(GFile) link = g_file_new_for_path (link_path);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dest = NULL;
  g_autofree gchar *dest_link = NULL;
  g_autofree gchar *copied = NULL;

  g_assert_nonnull (outside);
  g_assert_true (g_file_make_symbolic_link (link, outside, NULL, &error));
  g_assert_no_error (error);

  dest = g_strconcat (f->base_uri, "link/copied.txt", NULL);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/hello.txt", NULL,
                                "Destination", dest, NULL),
                    ==, SOUP_STATUS_FORBIDDEN);

  copied = g_build_filename (outside, "copied.txt", NULL);
  g_assert_false (g_file_test (copied, G_FILE_TEST_EXISTS));

  /* a symbolic link as the destination itself is refused too */
  dest_link = g_strconcat (f->base_uri, "link", NULL);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/hello.txt", NULL,
                                "Destination", dest_link, NULL),
                    ==, SOUP_STATUS_FORBIDDEN);

  g_assert_true (g_file_test (link_path, G_FILE_TEST_IS_SYMLINK));
  g_assert_cmpint (g_rmdir (outside), ==, 0);
}
#endif

/* Regression tests for the path traversal fix: encoded slashes and
 * dot-dot segments in the Destination header must be rejected so that
 * COPY/MOVE cannot write outside the shared directory. */
static void
test_movecopy_path_traversal (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *parent_dir = g_path_get_dirname (f->dir);

  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/source.txt", "data", NULL),
                    ==, SOUP_STATUS_CREATED);

  /* encoded slashes survive GLib dot-segment removal and would
   * resolve outside the root once decoded: they must be rejected */
  {
    g_autofree gchar *bad = g_strconcat (f->base_uri, "x%2f..%2f..%2fpwned.txt", NULL);

    g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/source.txt", NULL,
                                  "Destination", bad, NULL),
                      ==, SOUP_STATUS_BAD_REQUEST);
    g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/source.txt", NULL,
                                  "Destination", bad, NULL),
                      ==, SOUP_STATUS_BAD_REQUEST);
  }

  /* upper-case %2F variant */
  {
    g_autofree gchar *bad = g_strconcat (f->base_uri, "x%2F..%2Fpwned.txt", NULL);

    g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/source.txt", NULL,
                                  "Destination", bad, NULL),
                      ==, SOUP_STATUS_BAD_REQUEST);
  }

  /* a destination with an empty first segment ("//") would be
   * resolved against the filesystem root by g_file_get_child():
   * reject it like an absolute request path (the base URI ends
   * with a slash, the destination path is thus "//pwned.txt") */
  {
    g_autofree gchar *bad = g_strconcat (f->base_uri, "/pwned.txt", NULL);

    g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/source.txt", NULL,
                                  "Destination", bad, NULL),
                      ==, SOUP_STATUS_BAD_REQUEST);
    g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/source.txt", NULL,
                                  "Destination", bad, NULL),
                      ==, SOUP_STATUS_BAD_REQUEST);
  }

  /* dot-dot segments in a raw Destination path: GLib resolves them
   * during URI parsing before phodav sees the path, but phodav has
   * a defense-in-depth check with path_has_dot_dot_segment(); the
   * encoded-slash cases above are the real attack vector because
   * %2F is not a segment separator for the URI parser */

  /* an unparseable Destination URI */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/source.txt", NULL,
                                "Destination", "not-a-valid-uri", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* no file was created outside the shared directory */
  {
    g_autofree gchar *escaped = g_build_filename (parent_dir, "pwned.txt", NULL);

    g_assert_false (g_file_test (escaped, G_FILE_TEST_EXISTS));
  }
  {
    g_autofree gchar *escaped = g_build_filename (parent_dir, "outside.txt", NULL);

    g_assert_false (g_file_test (escaped, G_FILE_TEST_EXISTS));
  }

  /* the source file is still there */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/source.txt", NULL, NULL),
                    ==, SOUP_STATUS_OK);
}

static void
test_delete (DavFixture *f, gconstpointer user_data)
{
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/del.txt", "data", NULL), ==, SOUP_STATUS_CREATED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/del.txt", NULL, NULL), ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/del.txt", NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/del.txt", NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);

  /* collections are removed recursively */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MKCOL, "/dd", NULL, NULL), ==, SOUP_STATUS_CREATED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/dd/x.txt", "data", NULL), ==, SOUP_STATUS_CREATED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/dd", NULL, NULL), ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/dd/x.txt", NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
}

static void
test_propfind_allprop (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = dav_request (f, "PROPFIND", "/", PROPFIND_ALLPROP, &body,
                                            "Depth", "1", NULL);
  g_autoptr(xmlDoc) doc = NULL;
  g_autofree gchar *content = NULL;

  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);

  /* the root and its direct children are listed */
  g_assert_nonnull (find_response (f, doc, "/"));
  g_assert_nonnull (find_response (f, doc, "/hello.txt"));
  g_assert_nonnull (find_response (f, doc, "/dir"));
  g_assert_null (find_response (f, doc, "/dir/inner.txt"));
  g_assert_null (find_response (f, doc, "/missing.txt"));

  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "getcontentlength"), ==, SOUP_STATUS_OK);
  content = prop_value (f, doc, "/hello.txt", "getcontentlength");
  g_assert_cmpstr (content, ==, G_STRINGIFY (HELLO_LENGTH));

  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "getcontenttype"), ==, SOUP_STATUS_OK);
  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "resourcetype"), ==, SOUP_STATUS_OK);
  g_assert_cmpuint (prop_status (f, doc, "/dir", "resourcetype"), ==, SOUP_STATUS_OK);
  g_assert_cmpuint (prop_status (f, doc, "/", "getlastmodified"), ==, SOUP_STATUS_OK);

  /* collections are marked in the resourcetype property */
  {
    xmlNodePtr response = find_response (f, doc, "/dir");
    xmlNodePtr propstat = find_propstat (response, "resourcetype");

    g_assert_nonnull (find_child (find_child (propstat, "prop"), "resourcetype")->children);
    g_assert_cmpstr ((gchar *) find_child (find_child (propstat, "prop"), "resourcetype")->children->name,
                     ==, "collection");
  }

  /* depth 1 on a collection lists its children */
  {
    g_autoptr(GBytes) dir_body = NULL;
    g_autoptr(xmlDoc) dir_doc = NULL;
    g_autoptr(SoupMessage) dir_msg = dav_request (f, "PROPFIND", "/dir", PROPFIND_ALLPROP, &dir_body,
                                                  "Depth", "1", NULL);

    g_assert_cmpuint (soup_message_get_status (dir_msg), ==, SOUP_STATUS_MULTI_STATUS);
    dir_doc = parse_multistatus (dir_body);
    g_assert_nonnull (find_response (f, dir_doc, "/dir"));
    g_assert_nonnull (find_response (f, dir_doc, "/dir/inner.txt"));
    g_assert_null (find_response (f, dir_doc, "/hello.txt"));
  }
}

static void
test_propfind_prop (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = dav_request (f, "PROPFIND", "/hello.txt", PROPFIND_LENGTH_AND_ETAG,
                                            &body, "Depth", "0", NULL);
  g_autoptr(xmlDoc) doc = NULL;
  g_autofree gchar *content = NULL;
  g_autofree gchar *etag = NULL;

  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);

  content = prop_value (f, doc, "/hello.txt", "getcontentlength");
  g_assert_cmpstr (content, ==, G_STRINGIFY (HELLO_LENGTH));

  /* the reported etag is the GIO etag, in quotes */
  {
    g_autofree gchar *path = g_build_filename (f->dir, "hello.txt", NULL);
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GFileInfo) info = g_file_query_info (file, "etag::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

    etag = g_strdup_printf ("\"%s\"", g_file_info_get_etag (info));
  }
  g_clear_pointer (&content, g_free);
  content = prop_value (f, doc, "/hello.txt", "getetag");
  g_assert_cmpstr (content, ==, etag);

  /* only the requested properties are returned */
  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "getcontenttype"), ==, 0);

  /* unknown properties are reported in a separate, failing propstat */
  g_clear_object (&msg);
  g_clear_pointer (&doc, xmlFreeDoc);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, "PROPFIND", "/hello.txt", PROPFIND_UNKNOWN, &body, "Depth", "0", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "getcontentlength"), ==, SOUP_STATUS_OK);
  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "nonexistent"), ==, SOUP_STATUS_NOT_FOUND);
}

static void
test_propfind_propname (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = dav_request (f, "PROPFIND", "/hello.txt", PROPFIND_PROPNAME,
                                            &body, "Depth", "0", NULL);
  g_autoptr(xmlDoc) doc = NULL;
  xmlNodePtr propstat;

  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);

  propstat = find_propstat (find_response (f, doc, "/hello.txt"), "getcontentlength");
  g_assert_nonnull (propstat);
  /* propname responses have no values */
  g_assert_null (find_child (find_child (propstat, "prop"), "getcontentlength")->children);
}

/* count how many times @name appears as a <prop> child across all
 * propstat elements of the response for @path */
static guint
count_prop (DavFixture *f, xmlDocPtr doc, const gchar *path, const gchar *name)
{
  xmlNodePtr response = find_response (f, doc, path);
  guint count = 0;

  g_assert_nonnull (response);

  for (xmlNodePtr ps = response->children; ps; ps = ps->next)
    {
      xmlNodePtr prop;

      if (g_strcmp0 ((gchar *) ps->name, "propstat") != 0)
        continue;

      prop = find_child (ps, "prop");
      if (!prop)
        continue;

      for (xmlNodePtr n = prop->children; n; n = n->next)
        if (n->type == XML_ELEMENT_NODE && g_strcmp0 ((gchar *) n->name, name) == 0)
          count++;
    }

  return count;
}

/* Duplicate properties in a PROPFIND <prop> element should be
 * answered only once (the hash table deduplicates them with
 * prop_node_hash / prop_node_equal). */
static void
test_propfind_dedup (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(xmlDoc) doc = NULL;

  /* the same property listed twice in the same <prop> element */
  msg = dav_request (f, "PROPFIND", "/hello.txt",
                     "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
                     "<D:getcontentlength/>"
                     "<D:getcontentlength/>"
                     "</D:prop></D:propfind>",
                     &body, "Depth", "0", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  /* the property must appear exactly once in the response */
  g_assert_cmpuint (count_prop (f, doc, "/hello.txt", "getcontentlength"), ==, 1);

  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  g_clear_pointer (&doc, xmlFreeDoc);

  /* properties with the same local name but different namespaces are
   * distinct and must both appear */
  msg = dav_request (f, "PROPFIND", "/hello.txt",
                     "<D:propfind xmlns:D=\"DAV:\" xmlns:X=\"urn:test\">"
                     "<D:prop><D:displayname/><X:displayname/></D:prop>"
                     "</D:propfind>",
                     &body, "Depth", "0", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  g_assert_cmpuint (count_prop (f, doc, "/hello.txt", "displayname"), ==, 2);
}

/* Regression test for the multistatus href escaping: the names are
 * URI-escaped in the hrefs, and a client can fetch the resources
 * they report. The markup escaping which used to be applied to the
 * names reported "amp&amp;and.txt" for a file named "amp&and.txt",
 * which cannot be fetched, and left a "?" or a "#" raw in the
 * href, where a client parses them as the query or the fragment
 * separator. */
static void
test_propfind_escaping (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(xmlDoc) doc = NULL;
  gsize base_len = strlen (f->base_uri);
  static const struct
  {
    const gchar *name;    /* the resource path */
    const gchar *escaped; /* its form in an href */
  } cases[] = {
    { "/hello.txt", "/hello.txt" },
    { "/dir", "/dir" },
    { "/sp ace.txt", "/sp%20ace.txt" },
    { "/amp&and.txt", "/amp&and.txt" },
    { "/100%.txt", "/100%25.txt" },
    { "/fold er", "/fold%20er" },
#ifndef G_OS_WIN32
    /* the names with ? or angle brackets cannot exist on Windows */
    { "/que?ry#hash.txt", "/que%3Fry%23hash.txt" },
    { "/tag<le>.txt", "/tag%3Cle%3E.txt" },
#endif
  };

  write_file (f->dir, "sp ace.txt", "space\n");
  write_file (f->dir, "amp&and.txt", "amp\n");
  write_file (f->dir, "100%.txt", "percent\n");
  make_directory (f->dir, "fold er");
#ifndef G_OS_WIN32
  write_file (f->dir, "que?ry#hash.txt", "query\n");
  write_file (f->dir, "tag<le>.txt", "tag\n");
#endif

  msg = dav_request (f, "PROPFIND", "/", PROPFIND_ALLPROP, &body, "Depth", "1", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);

  /* the base URI ends with "/", keep the "/" starting the path */
  if (base_len > 0 && f->base_uri[base_len - 1] == '/')
    base_len--;

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      xmlNodePtr response = find_response (f, doc, cases[i].name);
      g_autofree gchar *content = NULL;
      g_autoptr(SoupMessage) get = NULL;
      g_autoptr(GBytes) get_body = NULL;
      g_autoptr(GError) error = NULL;

      /* the resource is reported, with its name URI-escaped in
       * the href */
      g_assert_nonnull (response);
      content = node_content (find_child (response, "href"));
      g_assert_cmpstr (content + base_len, ==, cases[i].escaped);

      /* the reported href fetches the resource it names */
      get = soup_message_new (SOUP_METHOD_GET, content);
      g_assert_nonnull (get);
      get_body = soup_session_send_and_read (f->session, get, NULL, &error);
      g_assert_no_error (error);
      g_assert_cmpuint (soup_message_get_status (get), ==, SOUP_STATUS_OK);
    }
}

static void
test_propfind_empty_body (DavFixture *f, gconstpointer user_data)
{
  /* an empty body is accepted, as a Windows kludge, and behaves like allprop */
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = dav_request (f, "PROPFIND", "/hello.txt", NULL,
                                            &body, "Depth", "0", NULL);
  g_autoptr(xmlDoc) doc = NULL;

  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "getcontentlength"), ==, SOUP_STATUS_OK);
}

static void
test_propfind_errors (DavFixture *f, gconstpointer user_data)
{
  /* malformed body */
  g_assert_cmpuint (dav_status (f, "PROPFIND", "/hello.txt", "<propfind", "Depth", "0", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
  /* unexpected root element */
  g_assert_cmpuint (dav_status (f, "PROPFIND", "/hello.txt", "<unexpected/>", "Depth", "0", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
  /* an empty prop element names no property, it is malformed too */
  g_assert_cmpuint (dav_status (f, "PROPFIND", "/hello.txt",
                                "<D:propfind xmlns:D=\"DAV:\"><D:prop/></D:propfind>",
                                "Depth", "0", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
  /* missing resource */
  g_assert_cmpuint (dav_status (f, "PROPFIND", "/missing.txt", PROPFIND_ALLPROP, "Depth", "0", NULL),
                    ==, SOUP_STATUS_NOT_FOUND);
  /* infinite depth is not supported: RFC 4918 section 9.1 allows a
   * server to refuse to walk the whole tree, with 403 */
  g_assert_cmpuint (dav_status (f, "PROPFIND", "/", PROPFIND_ALLPROP, "Depth", "infinity", NULL),
                    ==, SOUP_STATUS_FORBIDDEN);
}

static void
test_proppatch (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(xmlDoc) doc = NULL;
  g_autofree gchar *content = NULL;
  g_autoptr(GError) error = NULL;
  gboolean supported;

  /* dead properties are stored as user xattrs, which is not supported
   * by every filesystem */
  {
    g_autofree gchar *path = g_build_filename (f->dir, "hello.txt", NULL);
    g_autoptr(GFile) file = g_file_new_for_path (path);

    supported = g_file_set_attribute (file, "xattr::user.phodav-probe",
                                      G_FILE_ATTRIBUTE_TYPE_STRING, "probe",
                                      G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (!supported)
      {
        g_test_skip ("user xattrs are not supported by the filesystem");
        return;
      }
  }

  msg = dav_request (f, "PROPPATCH", "/hello.txt", PROPPATCH_SET, &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "user.phodav-test"), ==, SOUP_STATUS_OK);

  g_clear_object (&msg);
  g_clear_pointer (&doc, xmlFreeDoc);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, "PROPFIND", "/hello.txt", PROPFIND_DEAD_PROP, &body, "Depth", "0", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  content = prop_value (f, doc, "/hello.txt", "user.phodav-test");
  g_assert_cmpstr (content, ==, "test value");

  /* non-ASCII values must round-trip unmodified */
  g_clear_object (&msg);
  g_clear_pointer (&doc, xmlFreeDoc);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, "PROPPATCH", "/hello.txt",
                     "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop>"
                     "<user.phodav-test>ünïcødé 𐀀</user.phodav-test>"
                     "</D:prop></D:set></D:propertyupdate>",
                     &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, "PROPFIND", "/hello.txt", PROPFIND_DEAD_PROP, &body, "Depth", "0", NULL);
  doc = parse_multistatus (body);
  g_clear_pointer (&content, g_free);
  content = prop_value (f, doc, "/hello.txt", "user.phodav-test");
  g_assert_cmpstr (content, ==, "ünïcødé 𐀀");

  g_clear_object (&msg);
  g_clear_pointer (&doc, xmlFreeDoc);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, "PROPPATCH", "/hello.txt", PROPPATCH_REMOVE, &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "user.phodav-test"), ==, SOUP_STATUS_OK);

  g_clear_object (&msg);
  g_clear_pointer (&doc, xmlFreeDoc);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, "PROPFIND", "/hello.txt", PROPFIND_DEAD_PROP, &body, "Depth", "0", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "user.phodav-test"), ==, SOUP_STATUS_NOT_FOUND);

  /* removing an already absent property is idempotent */
  g_clear_object (&msg);
  g_clear_pointer (&doc, xmlFreeDoc);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, "PROPPATCH", "/hello.txt", PROPPATCH_REMOVE, &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  g_assert_cmpuint (prop_status (f, doc, "/hello.txt", "user.phodav-test"), ==, SOUP_STATUS_OK);

  /* a missing resource is reported per-property, in a multistatus */
  {
    g_autoptr(GBytes) missing_body = NULL;
    g_autoptr(SoupMessage) missing_msg = NULL;
    g_autoptr(xmlDoc) missing_doc = NULL;

    missing_msg = dav_request (f, "PROPPATCH", "/missing.txt", PROPPATCH_SET, &missing_body, NULL);
    g_assert_cmpuint (soup_message_get_status (missing_msg), ==, SOUP_STATUS_MULTI_STATUS);
    missing_doc = parse_multistatus (missing_body);
    g_assert_cmpuint (prop_status (f, missing_doc, "/missing.txt", "user.phodav-test"), ==, SOUP_STATUS_NOT_FOUND);

    /* the same holds for a removal, which cannot reach the file */
    g_clear_object (&missing_msg);
    g_clear_pointer (&missing_doc, xmlFreeDoc);
    g_clear_pointer (&missing_body, g_bytes_unref);
    missing_msg = dav_request (f, "PROPPATCH", "/missing.txt", PROPPATCH_REMOVE, &missing_body, NULL);
    g_assert_cmpuint (soup_message_get_status (missing_msg), ==, SOUP_STATUS_MULTI_STATUS);
    missing_doc = parse_multistatus (missing_body);
    g_assert_cmpuint (prop_status (f, missing_doc, "/missing.txt", "user.phodav-test"), ==, SOUP_STATUS_NOT_FOUND);
  }

  /* a body without any set or remove instruction is malformed, it
   * would produce an empty multistatus response otherwise */
  g_assert_cmpuint (dav_status (f, "PROPPATCH", "/hello.txt",
                                "<D:propertyupdate xmlns:D=\"DAV:\"/>", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
  /* instructions outside of the DAV namespace are not recognized,
   * the body names no property either */
  g_assert_cmpuint (dav_status (f, "PROPPATCH", "/hello.txt",
                                "<D:propertyupdate xmlns:D=\"DAv:\"><D:set><D:prop>"
                                "<user.phodav-test>value</user.phodav-test>"
                                "</D:prop></D:set></D:propertyupdate>",
                                NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
}

static void
test_proppatch_empty_prop (DavFixture *f, gconstpointer user_data)
{
  /* a set with an empty prop names no property, it is malformed */
  g_assert_cmpuint (dav_status (f, "PROPPATCH", "/hello.txt",
                                "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop/>"
                                "</D:set></D:propertyupdate>",
                                NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
}

/* take an exclusive write lock, returns the Lock-Token header value;
 * @expected_status is CREATED when the lock creates the resource, OK
 * when it already exists */
static gchar *
lock_resource_full (DavFixture *f, const gchar *path, const gchar *timeout, guint expected_status)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = dav_request (f, "LOCK", path, LOCK_EXCLUSIVE, &body,
                                            "Timeout", timeout, "Depth", "0", NULL);
  const gchar *token;

  g_assert_cmpuint (soup_message_get_status (msg), ==, expected_status);
  token = soup_message_headers_get_one (soup_message_get_response_headers (msg), "Lock-Token");
  g_assert_nonnull (token);
  g_assert_true (g_str_has_prefix (token, "<urn:uuid:"));
  g_assert_true (g_str_has_suffix (token, ">"));

  /* the lockdiscovery body reports the lock */
  {
    g_autoptr(xmlDoc) doc = parse_prop (body);

    g_assert_nonnull (find_activelock (doc));
  }

  return g_strdup (token);
}

static gchar *
lock_resource (DavFixture *f, const gchar *path, const gchar *timeout)
{
  return lock_resource_full (f, path, timeout, SOUP_STATUS_CREATED);
}

static void
test_lock (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *token = lock_resource (f, "/locked.txt", "Second-3600");
  g_autofree gchar *if_header = g_strdup_printf ("(%s)", token);
  g_autofree gchar *path = g_build_filename (f->dir, "locked.txt", NULL);
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(xmlDoc) doc = NULL;

  /* the resource was created by the lock */
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

  /* it cannot be modified without submitting the lock */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/locked.txt", "data", NULL),
                    ==, SOUP_STATUS_LOCKED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/locked.txt", NULL, NULL),
                    ==, SOUP_STATUS_LOCKED);

  /* a second exclusive lock is refused */
  g_assert_cmpuint (dav_status (f, "LOCK", "/locked.txt", LOCK_EXCLUSIVE, NULL),
                    ==, SOUP_STATUS_LOCKED);

  /* the lock can be discovered with PROPFIND */
  msg = dav_request (f, "PROPFIND", "/locked.txt",
                     "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:lockdiscovery/></D:prop></D:propfind>",
                     &body, "Depth", "0", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  doc = parse_multistatus (body);
  g_assert_cmpuint (prop_status (f, doc, "/locked.txt", "lockdiscovery"), ==, SOUP_STATUS_OK);

  /* submitting the lock token in the If header allows writes */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/locked.txt", "locked content",
                                "If", if_header, NULL),
                    ==, SOUP_STATUS_OK);

  /* an invalid If header syntax is a bad request */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/locked.txt", "data", "If", "(x)", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* unlocking requires the token */
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/locked.txt", NULL,
                                "Lock-Token", "<urn:uuid:11111111-2222-3333-4444-555555555555>", NULL),
                    ==, SOUP_STATUS_CONFLICT);
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/locked.txt", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);

  /* the resource is writable again */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/locked.txt", "unlocked", NULL),
                    ==, SOUP_STATUS_OK);

  /* the token is gone */
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/locked.txt", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_CONFLICT);
}

static void
test_delete_locked (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *token = lock_resource (f, "/delete-locked.txt", "Second-3600");
  g_autofree gchar *if_header = g_strdup_printf ("(%s)", token);

  /* DELETE requires the token of the lock it removes */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/delete-locked.txt", NULL, NULL),
                    ==, SOUP_STATUS_LOCKED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/delete-locked.txt", NULL,
                                "If", if_header, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);

  /* RFC 4918 section 9.6.1: the lock is destroyed with the resource,
   * the path can be written again without submitting the old token */
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/delete-locked.txt", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_CONFLICT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/delete-locked.txt", "data", NULL),
                    ==, SOUP_STATUS_CREATED);
}

static void
test_lock_escaping (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(xmlDoc) doc = NULL;
  g_autofree gchar *content = NULL;

  msg = dav_request (f, "LOCK", "/sp ace.txt", LOCK_EXCLUSIVE, &body,
                     "Depth", "0", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_CREATED);

  doc = parse_prop (body);
  content = node_content (find_child (find_child (find_activelock (doc), "lockroot"), "href"));
  g_assert_cmpstr (content, ==, "/sp%20ace.txt");
}

static void
test_lock_double_slash (DavFixture *f, gconstpointer user_data)
{
  /* empty path segments are skipped when the path is used as a key
   * in the lock table: a lock can be released again under an
   * equivalent spelling of its path.  The empty segment cannot be
   * leading though: such a request path is rejected outright, see
   * test_request_path_validation().  (Note that the test client
   * builds its request URIs with g_build_path(), which collapses a
   * leading empty segment, but preserves a trailing one.) */
  g_autofree gchar *token = lock_resource (f, "/locked.txt", "Second-3600");

  g_assert_cmpuint (dav_status (f, "UNLOCK", "/locked.txt/", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);

  /* and the token is gone for any spelling of the path */
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/locked.txt", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_CONFLICT);
}

static void
test_lock_depth_infinity (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *token = NULL;
  g_autofree gchar *if_header = NULL;

  /* locking a collection with infinite depth locks its tree,
   * the collection itself is not created by the lock */
  {
    g_autoptr(GBytes) body = NULL;
    g_autoptr(SoupMessage) msg = dav_request (f, "LOCK", "/dir", LOCK_EXCLUSIVE, &body,
                                              "Depth", "infinity", NULL);
    const gchar *t;

    g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
    t = soup_message_headers_get_one (soup_message_get_response_headers (msg), "Lock-Token");
    g_assert_nonnull (t);
    token = g_strdup (t);
  }

  if_header = g_strdup_printf ("(%s)", token);

  /* children are covered by the lock */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/dir/inner.txt", "data", NULL),
                    ==, SOUP_STATUS_LOCKED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/dir/other.txt", "data", NULL),
                    ==, SOUP_STATUS_LOCKED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/dir/other.txt", "data",
                                "If", if_header, NULL),
                    ==, SOUP_STATUS_CREATED);

  g_assert_cmpuint (dav_status (f, "UNLOCK", "/dir", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/dir/other.txt", "data", NULL),
                    ==, SOUP_STATUS_OK);
}

static void
test_lock_refresh (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *token = lock_resource (f, "/refresh.txt", "Second-60");
  g_autofree gchar *if_header = g_strdup_printf ("(%s)", token);
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autofree gchar *content = NULL;

  /* an empty body with an If header refreshes the lock timeout */
  msg = dav_request (f, "LOCK", "/refresh.txt", NULL, &body,
                     "If", if_header, "Timeout", "Second-120", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  {
    g_autoptr(xmlDoc) ldoc = parse_prop (body);
    xmlNodePtr timeout = find_child (find_activelock (ldoc), "timeout");

    g_assert_nonnull (timeout);
    content = node_content (timeout);
    g_assert_true (g_str_has_prefix (content, "Second-"));
    {
      gint64 remaining = g_ascii_strtoll (content + strlen ("Second-"), NULL, 10);

      g_assert_cmpint (remaining, >, 60);
      g_assert_cmpint (remaining, <=, 120);
    }
  }

  /* refreshing an unknown token fails the If condition, which must
   * be rejected with 412 (RFC 4918 section 10.4.3) */
  g_assert_cmpuint (dav_status (f, "LOCK", "/refresh.txt", NULL,
                                "If", "(<urn:uuid:11111111-2222-3333-4444-555555555555>)", NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);
  /* so is a malformed If header */
  g_assert_cmpuint (dav_status (f, "LOCK", "/refresh.txt", NULL, "If", "(x)", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  g_assert_cmpuint (dav_status (f, "UNLOCK", "/refresh.txt", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);
}

static void
test_if_etag (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *etag = NULL;
  g_autofree gchar *if_header = NULL;

  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/etag.txt", "data", NULL),
                    ==, SOUP_STATUS_CREATED);

  {
    g_autofree gchar *path = g_build_filename (f->dir, "etag.txt", NULL);
    g_autoptr(GFile) file = NULL;
    g_autoptr(GFileInfo) info = NULL;

    assert_file_contents (path, "data");
    file = g_file_new_for_path (path);
    info = g_file_query_info (file, "etag::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

    etag = g_strdup (g_file_info_get_etag (info));
    g_assert_nonnull (etag);
  }

  /* a failing etag condition */
  if_header = g_strdup_printf ("([\"not-the-etag\"])");
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/etag.txt", NULL,
                                "If", if_header, NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);

  /* the matching etag satisfies the condition */
  g_clear_pointer (&if_header, g_free);
  if_header = g_strdup_printf ("([\"%s\"])", etag);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/etag.txt", NULL,
                                "If", if_header, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/etag.txt", NULL, NULL),
                    ==, SOUP_STATUS_NOT_FOUND);
}

static void
test_readonly (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autofree gchar *dest = g_build_path ("/", f->base_uri, "moved.txt", NULL);

  /* read-only servers still serve content */
  msg = dav_request (f, SOUP_METHOD_GET, "/hello.txt", NULL, &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, "PROPFIND", "/", PROPFIND_ALLPROP, &body, "Depth", "1", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);

  /* but reject every modification */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/x.txt", "data", NULL), ==, SOUP_STATUS_FORBIDDEN);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MKCOL, "/x", NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/hello.txt", NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/hello.txt", NULL,
                                "Destination", dest, NULL),
                    ==, SOUP_STATUS_FORBIDDEN);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/hello.txt", NULL,
                                "Destination", dest, NULL),
                    ==, SOUP_STATUS_FORBIDDEN);
  g_assert_cmpuint (dav_status (f, "PROPPATCH", "/hello.txt", PROPPATCH_SET, NULL),
                    ==, SOUP_STATUS_FORBIDDEN);
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt", LOCK_EXCLUSIVE, NULL),
                    ==, SOUP_STATUS_FORBIDDEN);

  {
    g_autofree gchar *path = g_build_filename (f->dir, "x.txt", NULL);

    g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  }
}

static void
test_server_properties (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *root = NULL;
  g_autoptr(GFile) root_file = NULL;
  g_autoptr(SoupServer) server = NULL;
  gboolean readonly;
  guint64 max_upload;

  /* every property can be read back */
  g_object_get (f->phodav,
                "root", &root,
                "root-file", &root_file,
                "server", &server,
                "read-only", &readonly,
                "max-upload", &max_upload,
                NULL);

  g_assert_cmpstr (root, ==, f->dir);
  g_assert_nonnull (root_file);
  g_assert_true (SOUP_IS_SERVER (server));
  g_assert_false (readonly);
  g_assert_cmpuint (max_upload, ==, 0);

  /* the soup server is the one the requests are served on */
  g_assert_true (server == phodav_server_get_soup_server (f->phodav));
}

static void
test_propfind_quota (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(GBytes) body = NULL;
  g_autofree gchar *text = NULL;
  gsize size;

  /* the quota properties report the disk usage of the file */
  msg = dav_request (f, "PROPFIND", "/hello.txt",
                     "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
                     "<D:quota-used-bytes/><D:quota-available-bytes/>"
                     "</D:prop></D:propfind>",
                     &body, "Depth", "0", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);

  {
    const gchar *data = g_bytes_get_data (body, &size);

    text = g_strndup (data, size);
  }

  /* the usage of hello.txt is reported as the allocated blocks, and
   * some space is available */
  {
    g_autofree gchar *used = NULL;
    const gchar *pos = strstr (text, "<D:quota-used-bytes>");

    g_assert_nonnull (pos);
    pos += strlen ("<D:quota-used-bytes>");
    used = g_strndup (pos, strcspn (pos, "<"));
    g_assert_cmpuint (g_ascii_strtoull (used, NULL, 10), >, 0);
  }
  {
    g_autofree gchar *available = NULL;
    const gchar *pos = strstr (text, "<D:quota-available-bytes>");

    g_assert_nonnull (pos);
    pos += strlen ("<D:quota-available-bytes>");
    available = g_strndup (pos, strcspn (pos, "<"));
    g_assert_cmpuint (g_ascii_strtoull (available, NULL, 10), >, 0);
  }

  /* on a directory, the usage includes its contents */
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  msg = dav_request (f, "PROPFIND", "/dir",
                     "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
                     "<D:quota-used-bytes/>"
                     "</D:prop></D:propfind>",
                     &body, "Depth", "0", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);

  g_clear_pointer (&text, g_free);
  {
    const gchar *data = g_bytes_get_data (body, &size);

    text = g_strndup (data, size);
  }
  g_assert_nonnull (strstr (text, "<D:quota-used-bytes>"));
}

static void
test_lock_bad_request (DavFixture *f, gconstpointer user_data)
{
  /* an unknown lock scope */
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt",
                                "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:bogus/></D:lockscope>"
                                "<D:locktype><D:write/></D:locktype></D:lockinfo>",
                                NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* an unknown lock type */
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt",
                                "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:exclusive/></D:lockscope>"
                                "<D:locktype><D:bogus/></D:locktype></D:lockinfo>",
                                NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* an empty body is only valid for refreshing a held lock */
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt", NULL,
                                "If", "(malformed", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
  /* an If condition on a token which is not a held lock fails, which
   * must be rejected with 412 (RFC 4918 section 10.4.3) */
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt", NULL,
                                "If", "(<opaquelocktoken:not-a-held-lock>)", NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);

  /* a lock body which is not lockinfo at all */
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt",
                                "<D:wrong xmlns:D=\"DAV:\"/>", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* empty lockscope element (no child element) */
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt",
                                "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope/>"
                                "<D:locktype><D:write/></D:locktype></D:lockinfo>",
                                NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* empty locktype element (no child element) */
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt",
                                "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:exclusive/></D:lockscope>"
                                "<D:locktype/></D:lockinfo>",
                                NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* lockscope with only text content, no child element */
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt",
                                "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope>text</D:lockscope>"
                                "<D:locktype><D:write/></D:locktype></D:lockinfo>",
                                NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
}

static void
test_lock_failed_does_not_leak (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(GBytes) body = NULL;

  /* the parent collection is missing: the empty resource of the lock
   * cannot be created, and the request is rolled back */
  msg = dav_request (f, "LOCK", "/missing/locked.txt", LOCK_EXCLUSIVE, &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_CONFLICT);

  /* the token is only advertised by a successful LOCK */
  g_assert_null (soup_message_headers_get_one (
    soup_message_get_response_headers (msg), "Lock-Token"));

  /* a leaked lock would turn the next attempt into 423 */
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);
  g_assert_cmpuint (dav_status (f, "LOCK", "/missing/locked.txt", LOCK_EXCLUSIVE, NULL),
                    ==, SOUP_STATUS_CONFLICT);
}

static void
test_unlock_no_token (DavFixture *f, gconstpointer user_data)
{
  /* the lock token is mandatory */
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* ... and it must be bracketed */
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/hello.txt", NULL,
                                "Lock-Token", "opaquelocktoken:not-bracketed", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
}

static void
test_locked_parent (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *token = lock_resource_full (f, "/dir/inner.txt", "Second-3600",
                                                SOUP_STATUS_OK);
  g_autofree gchar *if_header = g_strdup_printf ("(%s)", token);
  g_autofree gchar *tagged = g_strdup_printf ("</dir/inner.txt> (%s)", token);

  /* modifications of the locked file are rejected without the token */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/dir/inner.txt", "data", NULL),
                    ==, SOUP_STATUS_LOCKED);

  /* deleting the directory also deletes the locked member: the token
   * of the member must be submitted, not only the ones of the
   * collection */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/dir", NULL, NULL),
                    ==, SOUP_STATUS_LOCKED);

  /* an untagged If list applies to the request URI (/dir): the token
   * of the locked member does not satisfy it, and the condition fails
   * before the lock check */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/dir", NULL,
                                "If", if_header, NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);

  /* the tagged list names the locked member, and lets the delete
   * proceed */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/dir", NULL,
                                "If", tagged, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);

  {
    g_autofree gchar *path = g_build_filename (f->dir, "dir", NULL);

    g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  }

  /* the lock of the deleted member is gone too */
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/dir/inner.txt", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_CONFLICT);

  /* deleting something which does not exist */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/nonexistent", NULL, NULL),
                    ==, SOUP_STATUS_NOT_FOUND);
}

static void
test_locked_destination (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *token = lock_resource_full (f, "/dir/inner.txt", "Second-3600",
                                                SOUP_STATUS_OK);
  g_autofree gchar *if_header = g_strdup_printf ("(%s)", token);
  g_autofree gchar *tagged = g_strdup_printf ("</dir/inner.txt> (%s)", token);
  g_autofree gchar *dest = g_build_path ("/", f->base_uri, "dir", NULL);

  /* copying over the collection would delete its locked member: the
   * token of that member must be submitted */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/hello.txt", NULL,
                                "Destination", dest, NULL),
                    ==, SOUP_STATUS_LOCKED);

  /* the untagged token applies to the source, not to the destination
   * member, so the condition fails */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/hello.txt", NULL,
                                "Destination", dest, "If", if_header, NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);

  /* the tagged token names the locked member of the destination */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/hello.txt", NULL,
                                "Destination", dest, "If", tagged, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);

  /* moving a collection also removes its members from the source */
  make_directory (f->dir, "moving");
  write_file (f->dir, "moving/member.txt", "member\n");
  {
    g_autofree gchar *member_token =
      lock_resource_full (f, "/moving/member.txt", "Second-3600", SOUP_STATUS_OK);
    g_autofree gchar *member_if = g_strdup_printf ("(%s)", member_token);
    g_autofree gchar *member_tagged = g_strdup_printf ("</moving/member.txt> (%s)",
                                                       member_token);
    g_autofree gchar *move_dest = g_build_path ("/", f->base_uri, "moved", NULL);
    g_autofree gchar *copy_dest = g_build_path ("/", f->base_uri, "copied", NULL);

    /* a COPY leaves the source untouched, its member locks do not
     * block it */
    g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/moving", NULL,
                                  "Destination", copy_dest, NULL),
                      ==, SOUP_STATUS_CREATED);

    g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/moving", NULL,
                                  "Destination", move_dest, NULL),
                      ==, SOUP_STATUS_LOCKED);
    g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/moving", NULL,
                                  "Destination", move_dest, "If", member_if, NULL),
                      ==, SOUP_STATUS_PRECONDITION_FAILED);
    g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/moving", NULL,
                                  "Destination", move_dest, "If", member_tagged, NULL),
                      ==, SOUP_STATUS_CREATED);

    {
      g_autofree gchar *source = g_build_filename (f->dir, "moving", NULL);
      g_autofree gchar *moved = g_build_filename (f->dir, "moved/member.txt", NULL);

      g_assert_false (g_file_test (source, G_FILE_TEST_EXISTS));
      assert_file_contents (moved, "member\n");
    }
  }
}

static void
test_if_malformed (DavFixture *f, gconstpointer user_data)
{
  const gchar *headers[] = {
    "(<no-close",
    "([",
    "([\"etag",
    "([\"etag\"",
    /* a trailing backslash must not escape the string terminator */
    "([\"etag\\",
    "(\"etag\")",
    "Not",
    /* unclosed token ref: '<' without '>' (accept_ref NULL path) */
    "(<urn:uuid:no-closing-angle",
    /* etag bracket without opening quote */
    "([no-quote])",
    /* tagged-list form with no condition list after the URI */
    "<http://localhost/path>",
    /* a resource with an empty first path segment ("//") would be
     * resolved against the filesystem root: reject it like an
     * absolute request path */
    "<http://localhost//tmp/pwned.txt> ([\"etag\"])",
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (headers); i++)
    g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/hello.txt", NULL,
                                  "If", headers[i], NULL),
                      ==, SOUP_STATUS_BAD_REQUEST);

  /* tagged-list with a valid URI but a malformed condition */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/hello.txt", NULL,
                                "If", "<http://localhost/hello.txt> ([",
                                NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* a backslash-escaped quote inside an etag condition: this is valid
   * syntax (exercises the escape path in accept_etag), but the etag
   * does not match any file, so the condition fails with 412 */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/hello.txt", NULL,
                                "If", "([\"escaped\\\"quote\"])",
                                NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);

  /* the file is still there */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_OK);
}

static void
test_get_listing_escaping (DavFixture *f, gconstpointer user_data)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autofree gchar *text = NULL;

#ifdef G_OS_WIN32
  /* the names with ? or angle brackets cannot exist on Windows */
  g_test_skip ("the file names are not valid on Windows");
  return;
#endif

  /* names which need escaping in the href and in the label */
  write_file (f->dir, "sp ace.txt", "space\n");
  write_file (f->dir, "que?ry#hash.txt", "query\n");
  write_file (f->dir, "amp&and.txt", "amp\n");
  write_file (f->dir, "tag<le>.txt", "tag\n");
  make_directory (f->dir, "fold er");

  msg = dav_request (f, SOUP_METHOD_GET, "/", NULL, &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  text = g_strndup (g_bytes_get_data (body, NULL), g_bytes_get_size (body));

  /* the hrefs are URI escaped... */
  g_assert_nonnull (strstr (text, "href=\"sp%20ace.txt\""));
  g_assert_nonnull (strstr (text, "href=\"que%3Fry%23hash.txt\""));
  g_assert_nonnull (strstr (text, "href=\"amp&amp;and.txt\""));
  g_assert_nonnull (strstr (text, "href=\"fold%20er/\""));

  /* ... and the labels are XML escaped */
  g_assert_nonnull (strstr (text, ">sp ace.txt</a>"));
  g_assert_nonnull (strstr (text, ">que?ry#hash.txt</a>"));
  g_assert_nonnull (strstr (text, ">amp&amp;and.txt</a>"));
  g_assert_nonnull (strstr (text, ">tag&lt;le&gt;.txt</a>"));
}

static void
test_put_conditional (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *etag = NULL;
  g_autofree gchar *if_match = NULL;
  g_autofree gchar *path = g_build_filename (f->dir, "hello.txt", NULL);

  /* the etag of the fixture file, as a client would have got it from
   * a GET response or a PROPFIND */
  {
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GFileInfo) info = g_file_query_info (file, "etag::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

    etag = g_strdup (g_file_info_get_etag (info));
    g_assert_nonnull (etag);
  }
  if_match = g_strdup_printf ("\"%s\"", etag);

  /* If-Match with the current etag allows the write */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", "matched",
                                "If-Match", if_match, NULL),
                    ==, SOUP_STATUS_OK);
  assert_file_contents (path, "matched");

  /* the write happened, the etag changed with it */
  {
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GFileInfo) info = g_file_query_info (file, "etag::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

    g_set_str (&etag, g_file_info_get_etag (info));
    g_assert_nonnull (etag);
    g_clear_pointer (&if_match, g_free);
    if_match = g_strdup_printf ("\"%s\"", etag);
  }

  /* a failing If-Match must not overwrite the resource,
   * RFC 9110 section 13.1.1 */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", "data",
                                "If-Match", "\"not-the-etag\"", NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", "data",
                                "If-Match", "\"not-the-etag\", W/\"weak-etag\"", NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);
  {
    g_autofree gchar *contents = NULL;

    g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
    g_assert_cmpstr (contents, ==, "matched");
  }

  /* If-Match: * requires the resource to exist */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", "star",
                                "If-Match", "*", NULL),
                    ==, SOUP_STATUS_OK);
  assert_file_contents (path, "star");
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/missing.txt", "data",
                                "If-Match", "*", NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);

  /* If-None-Match: * forbids overwriting an existing resource,
   * RFC 9110 section 13.1.2 */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", "data",
                                "If-None-Match", "*", NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/created.txt", "data",
                                "If-None-Match", "*", NULL),
                    ==, SOUP_STATUS_CREATED);

  /* the current etag fails an If-None-Match too; the writes above
   * changed it, fetch it again */
  {
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GFileInfo) info = g_file_query_info (file, "etag::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

    g_set_str (&etag, g_file_info_get_etag (info));
    g_assert_nonnull (etag);
    g_clear_pointer (&if_match, g_free);
    if_match = g_strdup_printf ("\"%s\"", etag);
  }
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", "data",
                                "If-None-Match", if_match, NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);

  /* another etag does not */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", "written",
                                "If-None-Match", "\"not-the-etag\"", NULL),
                    ==, SOUP_STATUS_OK);
  assert_file_contents (path, "written");
}

/* read up to the end of an HTTP header block, so that the caller can
 * tell the interim 100 Continue response from the final one */
static void
read_http_headers (GInputStream *in, gchar *buf, gsize size, GError **error)
{
  gsize used = 0;

  buf[0] = '\0';
  while (used < size - 1)
    {
      gssize n = g_input_stream_read (in, buf + used, size - 1 - used, NULL, error);

      if (n <= 0)
        return;

      used += n;
      buf[used] = '\0';

      if (strstr (buf, "\r\n\r\n"))
        return;
    }
}

/* a condition which held when the upload started must still hold when
 * the staged file is committed: a concurrent writer wins with 412,
 * and the upload does not overwrite its content */
static void
test_put_conditional_race (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *etag = NULL;
  g_autofree gchar *if_match = NULL;
  g_autofree gchar *path = g_build_filename (f->dir, "race.txt", NULL);
  g_autofree gchar *request = NULL;
  g_autoptr(GSocketClient) client = g_socket_client_new ();
  g_autoptr(GUri) uri = g_uri_parse (f->base_uri, SOUP_HTTP_URI_FLAGS, NULL);
  g_autoptr(GSocketConnection) conn = NULL;
  g_autoptr(GError) error = NULL;
  GOutputStream *out;
  GInputStream *in;
  gchar buf[1024];

  write_file (f->dir, "race.txt", "initial");
  {
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GFileInfo) info = g_file_query_info (file, "etag::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

    etag = g_strdup (g_file_info_get_etag (info));
    g_assert_nonnull (etag);
  }
  if_match = g_strdup_printf ("\"%s\"", etag);

  conn = g_socket_client_connect_to_host (client, g_uri_get_host (uri),
                                          g_uri_get_port (uri), NULL, &error);
  g_assert_no_error (error);
  g_socket_set_timeout (g_socket_connection_get_socket (conn), 2);

  out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  in = g_io_stream_get_input_stream (G_IO_STREAM (conn));

  request = g_strdup_printf ("PUT /race.txt HTTP/1.1\r\n"
                             "Host: localhost\r\n"
                             "If-Match: %s\r\n"
                             "Expect: 100-continue\r\n"
                             "Content-Length: 10\r\n"
                             "\r\n",
                             if_match);
  g_assert_true (g_output_stream_write_all (out, request, strlen (request), NULL, NULL, &error));
  g_assert_no_error (error);

  /* the server accepted the preconditions and waits for the body */
  read_http_headers (in, buf, sizeof (buf), &error);
  g_assert_no_error (error);
  g_assert_true (g_str_has_prefix (buf, "HTTP/1.1 100"));

  /* another writer changes the resource while the upload is pending */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/race.txt", "other", NULL),
                    ==, SOUP_STATUS_OK);

  /* the staged upload is committed only if the condition still holds */
  g_assert_true (g_output_stream_write_all (out, "0123456789", 10, NULL, NULL, &error));
  g_assert_no_error (error);
  read_http_headers (in, buf, sizeof (buf), &error);
  g_assert_no_error (error);
  g_assert_true (g_str_has_prefix (buf, "HTTP/1.1 412"));

  assert_file_contents (path, "other");
  assert_no_upload_temp_files (f->dir);
}

static void
test_unknown_method (DavFixture *f, gconstpointer user_data)
{
  /* methods which are not implemented are answered with 501 */
  g_assert_cmpuint (dav_status (f, "PATCH", "/hello.txt", "data", NULL),
                    ==, SOUP_STATUS_NOT_IMPLEMENTED);
  g_assert_cmpuint (dav_status (f, "BREW", "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_NOT_IMPLEMENTED);

  /* the file was not modified */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_OK);
}

static void
test_lock_expiry (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *token = lock_resource (f, "/expiring.txt", "Second-1");
  g_autofree gchar *if_header = g_strdup_printf ("(%s)", token);

  /* the lock is enforced while it has not expired */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/expiring.txt", "data", NULL),
                    ==, SOUP_STATUS_LOCKED);

  /* wait for the deadline to pass; expiry is enforced lazily, when
   * the locks are looked at again */
  g_usleep (1500000);

  /* once expired, the lock is not honored anymore */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/expiring.txt", "expired", NULL),
                    ==, SOUP_STATUS_OK);

  /* and the token cannot be used to refresh it anymore */
  g_assert_cmpuint (dav_status (f, "LOCK", "/expiring.txt", NULL,
                                "If", if_header, NULL),
                    ==, SOUP_STATUS_PRECONDITION_FAILED);
}

static void
test_lock_tree_conflict (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *token = lock_resource (f, "/dir/locked-member.txt", "Second-3600");
  g_autofree gchar *parent_token = NULL;

  /* an exclusive depth-infinity lock on the collection conflicts with
   * the lock held on the member (RFC 4918 section 7.4) */
  g_assert_cmpuint (dav_status (f, "LOCK", "/dir", LOCK_EXCLUSIVE,
                                "Depth", "infinity", NULL),
                    ==, SOUP_STATUS_LOCKED);

  /* a depth-0 lock does not cover the member and is allowed */
  {
    g_autoptr(GBytes) body = NULL;
    g_autoptr(SoupMessage) msg = dav_request (f, "LOCK", "/dir", LOCK_EXCLUSIVE, &body,
                                              "Depth", "0", NULL);

    g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
    parent_token = g_strdup (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Lock-Token"));
    g_assert_nonnull (parent_token);
  }
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/dir", NULL,
                                "Lock-Token", parent_token, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);

  /* with the member lock released, the tree can be locked */
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/dir/locked-member.txt", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);
  {
    g_autoptr(GBytes) body = NULL;
    g_autoptr(SoupMessage) msg = dav_request (f, "LOCK", "/dir", LOCK_EXCLUSIVE, &body,
                                              "Depth", "infinity", NULL);

    g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
    g_set_str (&parent_token, soup_message_headers_get_one (soup_message_get_response_headers (msg), "Lock-Token"));
    g_assert_nonnull (parent_token);
  }
  g_assert_cmpuint (dav_status (f, "UNLOCK", "/dir", NULL,
                                "Lock-Token", parent_token, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);
}

static void
test_lock_refresh_forms (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *token = lock_resource (f, "/refresh-forms.txt", "Second-60");
  g_autofree gchar *tagged = g_strdup_printf ("</refresh-forms.txt> (%s)", token);
  g_autofree gchar *etag = NULL;
  g_autofree gchar *etag_if = NULL;
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = NULL;

  /* the lock can also be identified with a tagged If header
   * (RFC 4918 section 10.4.2), not only with the bare token form */
  msg = dav_request (f, "LOCK", "/refresh-forms.txt", NULL, &body,
                     "If", tagged, "Timeout", "Second-120", NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  {
    g_autoptr(xmlDoc) doc = parse_prop (body);
    xmlNodePtr timeout = find_child (find_activelock (doc), "timeout");

    g_assert_nonnull (timeout);
  }

  /* an If header matching the resource etag but carrying no lock
   * token does not identify a lock to refresh */
  {
    g_autofree gchar *path = g_build_filename (f->dir, "refresh-forms.txt", NULL);
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GFileInfo) info = g_file_query_info (file, "etag::*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

    etag = g_strdup (g_file_info_get_etag (info));
    g_assert_nonnull (etag);
  }
  etag_if = g_strdup_printf ("([\"%s\"])", etag);
  g_assert_cmpuint (dav_status (f, "LOCK", "/refresh-forms.txt", NULL,
                                "If", etag_if, NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  g_assert_cmpuint (dav_status (f, "UNLOCK", "/refresh-forms.txt", NULL,
                                "Lock-Token", token, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);
}

static void
test_depth_headers (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *dest = g_build_path ("/", f->base_uri, "depth-dest.txt", NULL);

  /* an invalid Depth is rejected with 400 by every method using it */
  g_assert_cmpuint (dav_status (f, "PROPFIND", "/hello.txt", PROPFIND_ALLPROP,
                                "Depth", "2", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt", LOCK_EXCLUSIVE,
                                "Depth", "bogus", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* only the depths 0 and infinity are valid for LOCK (RFC 4918 section 10.2) */
  g_assert_cmpuint (dav_status (f, "LOCK", "/hello.txt", LOCK_EXCLUSIVE,
                                "Depth", "1", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* ... and for COPY and MOVE (RFC 4918 section 9.8.3) */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_MOVE, "/hello.txt", NULL,
                                "Destination", dest, "Depth", "1", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/hello.txt", NULL,
                                "Destination", dest, "Depth", "1", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* DELETE acts with depth infinity only (RFC 4918 section 9.6.1) */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/hello.txt", NULL,
                                "Depth", "0", NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* nothing was modified by the rejected requests */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_OK);

  /* a missing Depth header defaults to infinity */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_COPY, "/hello.txt", NULL,
                                "Destination", dest, NULL),
                    ==, SOUP_STATUS_CREATED);
}

/* the methods other than PUT parse their whole request body in memory:
 * oversized ones must be refused, not parsed */
static void
test_request_body_too_large (DavFixture *f, gconstpointer user_data)
{
  /* one byte more than the 1 MiB limit */
  g_autofree gchar *big = g_strnfill (1024 * 1024 + 1, 'x');

  /* with a Content-Length, the body is refused with 413 as soon as
   * the headers are received, before it is read */
  g_assert_cmpuint (dav_status (f, "PROPFIND", "/hello.txt", big, "Depth", "0", NULL),
                    ==, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);

  /* a small chunked body is parsed normally */
  {
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *uri = g_build_path ("/", f->base_uri, "/hello.txt", NULL);
    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_data (PROPFIND_ALLPROP, -1, NULL);
    g_autoptr(GBytes) response = NULL;
    g_autoptr(GError) error = NULL;

    msg = soup_message_new (SOUP_METHOD_PROPFIND, uri);
    soup_message_headers_append (soup_message_get_request_headers (msg), "Depth", "0");
    soup_message_set_request_body (msg, "application/xml", stream, -1);

    response = soup_session_send_and_read (f->session, msg, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_MULTI_STATUS);
  }

  /* a chunked body has no Content-Length to check in advance: it is
   * refused with 413 as soon as it grows over the limit, while it is
   * still being received, and the rest is drained rather than
   * accumulated */
  {
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *uri = g_build_path ("/", f->base_uri, "/hello.txt", NULL);
    /* several times the limit, so that the part received after the
     * refusal is drained too */
    g_autofree gchar *huge = g_strnfill (8 * 1024 * 1024, 'x');
    g_autoptr(GBytes) request = g_bytes_new (huge, strlen (huge));
    g_autoptr(GInputStream) stream = NULL;
    g_autoptr(GBytes) response = NULL;
    g_autoptr(GError) error = NULL;

    msg = soup_message_new (SOUP_METHOD_PROPFIND, uri);
    soup_message_headers_append (soup_message_get_request_headers (msg), "Depth", "0");
    stream = g_memory_input_stream_new_from_bytes (request);
    soup_message_set_request_body (msg, NULL, stream, -1);

    response = soup_session_send_and_read (f->session, msg, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (response);
    g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);
  }

  /* a DELETE with an oversized Content-Length must be rejected
   * with 413, not silently executed with the body discarded */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/hello.txt", big, NULL),
                    ==, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);

  /* the file must still exist */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_OK);
}

/* a PUT which fails mid-upload must not leave a partial file behind */
static void
test_put_write_error (DavFixture *f, gconstpointer user_data)
{
#ifdef G_OS_UNIX
  g_autofree gchar *path = g_build_filename (f->dir, "partial.txt", NULL);
  g_autoptr(SoupMessage) msg = NULL;
  g_autofree gchar *uri = NULL;
  g_autoptr(GError) error = NULL;
  struct rlimit limit, old;
  g_autoptr(GBytes) body;

  /* limit the writes to 8 bytes: writing more fails with EFBIG
   * (SIGXFSZ must be ignored, or it would kill the test process) */
  g_assert_cmpint (getrlimit (RLIMIT_FSIZE, &old), ==, 0);
  limit = old;
  limit.rlim_cur = 8;
  signal (SIGXFSZ, SIG_IGN);
  g_assert_cmpint (setrlimit (RLIMIT_FSIZE, &limit), ==, 0);

  /* the write failure is answered with 500, and the file it was
   * writing is discarded */

  uri = g_build_path ("/", f->base_uri, "/partial.txt", NULL);
  msg = soup_message_new (SOUP_METHOD_PUT, uri);
  body = g_bytes_new_take (g_strnfill (64, 'x'), 64);
  soup_message_set_request_body_from_bytes (msg, NULL, body);

  {
    /* the server may answer 500, or reset the connection while it is
     * still receiving the body */
    g_autoptr(GBytes) response = soup_session_send_and_read (f->session, msg, NULL, &error);

    if (error == NULL)
      g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_INTERNAL_SERVER_ERROR);
  }

  /* the partial file was discarded */
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  /* the same happens when overwriting an existing file: the upload
   * fails, and the original content is preserved */
  {
    g_autofree gchar *data = g_strnfill (64, 'x');
    g_autofree gchar *hello = g_build_filename (f->dir, "hello.txt", NULL);

    g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", data, NULL),
                      ==, SOUP_STATUS_INTERNAL_SERVER_ERROR);

    assert_file_contents (hello, HELLO_CONTENT);
  }

  assert_no_upload_temp_files (f->dir);

  signal (SIGXFSZ, SIG_DFL);
  g_assert_cmpint (setrlimit (RLIMIT_FSIZE, &old), ==, 0);
#else
  g_test_skip ("not supported on this platform");
#endif
}

/* A client may ask with "Expect: 100-continue" (RFC 9110 section
 * 10.1.1) whether the server accepts its upload before sending the
 * body. The server must answer the final status only once the body
 * has been received and the file committed: answering a success
 * right away would tell such a client its upload succeeded before it
 * sent the body at all, and the server would commit an empty file. */
static void
test_put_expect_continue (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *path = g_build_filename (f->dir, "expect.txt", NULL);
  g_autoptr(GSocketClient) client = g_socket_client_new ();
  g_autoptr(GUri) uri = g_uri_parse (f->base_uri, SOUP_HTTP_URI_FLAGS, NULL);
  g_autoptr(GSocketConnection) conn = NULL;
  g_autoptr(GError) error = NULL;
  GOutputStream *out;
  GInputStream *in;
  gchar buf[1024];
  gssize n;

  conn = g_socket_client_connect_to_host (client, g_uri_get_host (uri), g_uri_get_port (uri), NULL, &error);
  g_assert_no_error (error);

  /* fail the reads which would hang, rather than wait forever */
  g_socket_set_timeout (g_socket_connection_get_socket (conn), 2);

  out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  in = g_io_stream_get_input_stream (G_IO_STREAM (conn));

  {
    const gchar *request =
      "PUT /expect.txt HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Expect: 100-continue\r\n"
      "Content-Length: 6\r\n"
      "\r\n";

    g_assert_true (g_output_stream_write_all (out, request, strlen (request), NULL, NULL, &error));
    g_assert_no_error (error);
  }

  /* the server accepts the upload and waits for its body, it must
   * not answer the final status before receiving it */
  n = g_input_stream_read (in, buf, sizeof (buf) - 1, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (n, >, 0);
  buf[n > 0 ? n : 0] = '\0';
  g_assert_true (g_str_has_prefix (buf, "HTTP/1.1 100"));

  /* the client, allowed to continue, sends its body */
  g_assert_true (g_output_stream_write_all (out, "expect", 6, NULL, NULL, &error));
  g_assert_no_error (error);

  /* the file is committed before the final response is written */
  n = g_input_stream_read (in, buf, sizeof (buf) - 1, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (n, >, 0);
  buf[n > 0 ? n : 0] = '\0';
  g_assert_true (g_str_has_prefix (buf, "HTTP/1.1 201"));

  assert_file_contents (path, "expect");
}

/* an upload which is interrupted (the client hangs up before its
 * whole body was sent) is discarded: the partial content must not
 * be committed by the finalization of the stream */
static void
test_put_interrupted (DavFixture *f, gconstpointer user_data)
{
  g_autofree gchar *path = g_build_filename (f->dir, "interrupted.txt", NULL);
  g_autoptr(GSocketClient) client = g_socket_client_new ();
  g_autoptr(GUri) uri = g_uri_parse (f->base_uri, SOUP_HTTP_URI_FLAGS, NULL);
  g_autoptr(GSocketConnection) conn = NULL;
  g_autoptr(GError) error = NULL;
  GOutputStream *out;

  conn = g_socket_client_connect_to_host (client, g_uri_get_host (uri), g_uri_get_port (uri), NULL, &error);
  g_assert_no_error (error);

  out = g_io_stream_get_output_stream (G_IO_STREAM (conn));

  {
    /* the body promises more than it delivers */
    const gchar *request =
      "PUT /interrupted.txt HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: 100\r\n"
      "\r\n"
      "partial";

    g_assert_true (g_output_stream_write_all (out, request, strlen (request), NULL, NULL, &error));
    g_assert_no_error (error);
  }

  /* hang up mid-body */
  g_assert_true (g_io_stream_close (G_IO_STREAM (conn), NULL, &error));
  g_assert_no_error (error);

  /* the server notices the hang up while handling the next request:
   * its main context dispatches the pending connection events
   * together, and the interrupted upload is discarded before the
   * response of the request made here is written */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_OK);

  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  /* an interrupted upload which overwrites an existing resource leaves
   * the original content untouched */
  {
    g_autofree gchar *hello = g_build_filename (f->dir, "hello.txt", NULL);
    g_autoptr(GSocketConnection) overwrite_conn = NULL;
    GOutputStream *overwrite_out;

    overwrite_conn = g_socket_client_connect_to_host (client, g_uri_get_host (uri),
                                                      g_uri_get_port (uri), NULL, &error);
    g_assert_no_error (error);
    overwrite_out = g_io_stream_get_output_stream (G_IO_STREAM (overwrite_conn));

    {
      const gchar *request =
        "PUT /hello.txt HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "partial";

      g_assert_true (g_output_stream_write_all (overwrite_out, request, strlen (request),
                                                NULL, NULL, &error));
      g_assert_no_error (error);
    }

    g_assert_true (g_io_stream_close (G_IO_STREAM (overwrite_conn), NULL, &error));
    g_assert_no_error (error);

    /* dispatch the hang up, then check the destination */
    g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL),
                      ==, SOUP_STATUS_OK);
    assert_file_contents (hello, HELLO_CONTENT);
  }

  assert_no_upload_temp_files (f->dir);
}

/* the server may cap the size of the PUT uploads: the requests over
 * the cap are refused with 413, and leave no partial file behind */
static void
test_put_max_upload (DavFixture *f, gconstpointer user_data)
{
  gsize limit = GPOINTER_TO_SIZE (user_data);
  g_autofree gchar *limited = g_build_filename (f->dir, "limited.txt", NULL);
  g_autofree gchar *chunked = g_build_filename (f->dir, "chunked.txt", NULL);
  g_autofree gchar *hello = g_build_filename (f->dir, "hello.txt", NULL);
  g_autofree gchar *at_limit = g_strnfill (limit, 'x');
  g_autofree gchar *over_limit = g_strnfill (limit + 1, 'x');

  /* an upload of exactly the limit is accepted */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/limited.txt", at_limit, NULL),
                    ==, SOUP_STATUS_CREATED);
  assert_file_contents (limited, at_limit);

  /* an upload with a Content-Length over the limit is refused as
   * soon as the headers are received, before anything is written */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/over.txt", over_limit, NULL),
                    ==, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);
  {
    g_autofree gchar *over = g_build_filename (f->dir, "over.txt", NULL);

    g_assert_false (g_file_test (over, G_FILE_TEST_EXISTS));
  }

  /* an existing resource is left untouched by the refusal */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", over_limit, NULL),
                    ==, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);
  assert_file_contents (hello, HELLO_CONTENT);

  /* a chunked body has no Content-Length to check in advance: the
   * upload is cut off while it is streamed, and the partial file is
   * discarded */
  {
    g_autofree gchar *uri = g_build_path ("/", f->base_uri, "/chunked.txt", NULL);
    g_autoptr(SoupMessage) msg = soup_message_new (SOUP_METHOD_PUT, uri);
    g_autoptr(GBytes) body = g_bytes_new_take (g_strnfill (limit * 4, 'x'), limit * 4);
    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes (body);
    g_autoptr(GBytes) response = NULL;
    g_autoptr(GError) error = NULL;

    soup_message_set_request_body (msg, NULL, stream, -1);

    /* the server may answer 413, or reset the connection while it is
     * still receiving the body */
    response = soup_session_send_and_read (f->session, msg, NULL, &error);
    if (error == NULL)
      g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);

    g_assert_false (g_file_test (chunked, G_FILE_TEST_EXISTS));
  }

  /* a chunked upload which stays at the limit is accepted */
  {
    g_autofree gchar *uri = g_build_path ("/", f->base_uri, "/chunked.txt", NULL);
    g_autoptr(SoupMessage) msg = soup_message_new (SOUP_METHOD_PUT, uri);
    g_autoptr(GBytes) body = g_bytes_new (at_limit, limit);
    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes (body);
    g_autoptr(GBytes) response = NULL;
    g_autoptr(GError) error = NULL;

    soup_message_set_request_body (msg, NULL, stream, -1);

    response = soup_session_send_and_read (f->session, msg, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_CREATED);

    assert_file_contents (chunked, at_limit);
  }

  assert_no_upload_temp_files (f->dir);
}

/* Send a raw HTTP request line directly over TCP, bypassing the
 * SoupSession client which normalizes the request path.  Returns
 * the HTTP status code from the response. */
static guint
raw_http_status (DavFixture *f, const gchar *request_line)
{
  g_autoptr(GSocketClient) client = g_socket_client_new ();
  g_autoptr(GSocketConnection) conn = NULL;
  g_autoptr(GError) error = NULL;
  GOutputStream *out;
  GInputStream *in;
  g_autofree gchar *request = NULL;
  gchar buf[1024];
  gssize n;
  guint status = 0;

  {
    g_autoptr(GUri) uri = g_uri_parse (f->base_uri, SOUP_HTTP_URI_FLAGS, NULL);

    conn = g_socket_client_connect_to_host (client,
                                            g_uri_get_host (uri),
                                            g_uri_get_port (uri),
                                            NULL, &error);
  }
  g_assert_no_error (error);

  request = g_strdup_printf ("%s HTTP/1.0\r\nHost: localhost\r\n\r\n",
                             request_line);
  out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  g_assert_true (g_output_stream_write_all (out, request, strlen (request),
                                            NULL, NULL, &error));
  g_assert_no_error (error);

  in = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  n = g_input_stream_read (in, buf, sizeof (buf) - 1, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (n, >, 0);
  buf[n] = '\0';

  /* parse "HTTP/1.x NNN" */
  if (g_str_has_prefix (buf, "HTTP/") && n > 12)
    status = (guint) g_ascii_strtoull (buf + 9, NULL, 10);

  return status;
}

/* The server validates that request paths start with '/' and are not
 * absolute (e.g. //host/share on Windows).  These guards cannot be
 * triggered through a SoupSession client because libsoup normalizes
 * the request path before it reaches the server handler.  This test
 * sends raw HTTP requests to bypass the client normalization and
 * exercise the server-side guards directly. */
static void
test_request_path_validation (DavFixture *f, gconstpointer user_data)
{
  /* a request path which is absolute after the leading slash:
   * g_path_is_absolute(path+1) would be TRUE */
  g_assert_cmpuint (raw_http_status (f, "GET //etc/passwd"),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* PUT also validates its path in got_headers() */
  g_assert_cmpuint (raw_http_status (f, "PUT //etc/passwd"),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* the streamed methods are validated like the other ones: a URI
   * fragment is refused there too, not only by server_callback() */
  g_assert_cmpuint (raw_http_status (f, "PUT /hello.txt#frag"),
                    ==, SOUP_STATUS_BAD_REQUEST);

  /* the dot-dot segments of a request path cannot escape the share:
   * they are removed when the request URI is parsed, and the ones
   * surviving are refused, by libsoup and by phodav on its own.
   * Check the property with files outside the share, next to its
   * directory */
  {
    g_autofree gchar *parent = g_path_get_dirname (f->dir);
    g_autofree gchar *base = g_path_get_basename (f->dir);
    g_autofree gchar *secret_name = g_strdup_printf ("%s-secret.txt", base);
    g_autofree gchar *secret = g_build_filename (parent, secret_name, NULL);
    g_autofree gchar *get = g_strdup_printf ("GET /../%s", secret_name);
    g_autofree gchar *evil_name = g_strdup_printf ("%s-evil.txt", base);
    g_autofree gchar *evil = g_build_filename (parent, evil_name, NULL);
    g_autofree gchar *put = g_strdup_printf ("PUT /../%s", evil_name);

    g_assert_true (g_file_set_contents (secret, "secret", -1, NULL));

    /* the outside file is not served: its path is normalized to the
     * same name inside the share (not found), or refused */
    g_assert_cmpuint (raw_http_status (f, get), !=, SOUP_STATUS_OK);

    /* and a PUT does not write outside the share either: it may
     * create the file inside (normalized), never next to it */
    g_assert_cmpuint (raw_http_status (f, put), !=, SOUP_STATUS_INTERNAL_SERVER_ERROR);
    g_assert_false (g_file_test (evil, G_FILE_TEST_EXISTS));

    g_unlink (secret);
  }

  /* the normal server still works after the rejected requests */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_OK);
}

/* an embedding application may provide its own SoupServer: the
 * requests routed to its handlers, at a more specific path than the
 * phodav root, must be left to them, and phodav must not impose its
 * request guards on them either */
static void
test_shared_server (DavFixture *f, gconstpointer user_data)
{
  /* the DAV requests are served as usual on the shared server */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_GET, "/hello.txt", NULL, NULL),
                    ==, SOUP_STATUS_OK);
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/dav.txt", "data", NULL),
                    ==, SOUP_STATUS_CREATED);
  {
    g_autofree gchar *path = g_build_filename (f->dir, "dav.txt", NULL);

    assert_file_contents (path, "data");
  }

  /* a PUT to the foreign handler is not streamed into the share by
   * phodav: the foreign handler answers, and nothing is written */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/upload/foreign.txt", "data", NULL),
                    ==, SOUP_STATUS_OK);
  {
    g_autofree gchar *path = g_build_filename (f->dir, "upload", NULL);

    g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  }

  /* the phodav request guards do not apply either: an oversized
   * body for the foreign handler is not refused with 413 */
  {
    g_autofree gchar *big = g_strnfill (1024 * 1024 + 1, 'x');

    g_assert_cmpuint (dav_status (f, SOUP_METHOD_POST, "/upload", big, NULL),
                      ==, SOUP_STATUS_OK);
  }
}

gint
main (gint argc, gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_set_nonfatal_assertions ();

  g_test_add ("/dav/options", DavFixture, NULL, dav_fixture_setup, test_options, dav_fixture_teardown);
  g_test_add ("/dav/get", DavFixture, NULL, dav_fixture_setup, test_get, dav_fixture_teardown);
  g_test_add ("/dav/get-conditional", DavFixture, NULL, dav_fixture_setup, test_get_conditional, dav_fixture_teardown);
  g_test_add ("/dav/get-range", DavFixture, NULL, dav_fixture_setup, test_get_range, dav_fixture_teardown);
  g_test_add ("/dav/get-listing-escaping", DavFixture, NULL, dav_fixture_setup, test_get_listing_escaping, dav_fixture_teardown);
  g_test_add ("/dav/unknown-method", DavFixture, NULL, dav_fixture_setup, test_unknown_method, dav_fixture_teardown);
  g_test_add ("/dav/put", DavFixture, NULL, dav_fixture_setup, test_put, dav_fixture_teardown);
  g_test_add ("/dav/put-conditional", DavFixture, NULL, dav_fixture_setup, test_put_conditional, dav_fixture_teardown);
  g_test_add ("/dav/put-conditional-race", DavFixture, NULL, dav_fixture_setup, test_put_conditional_race, dav_fixture_teardown);
  g_test_add ("/dav/put-write-error", DavFixture, NULL, dav_fixture_setup, test_put_write_error, dav_fixture_teardown);
  g_test_add ("/dav/put-expect-continue", DavFixture, NULL, dav_fixture_setup, test_put_expect_continue, dav_fixture_teardown);
  g_test_add ("/dav/put-interrupted", DavFixture, NULL, dav_fixture_setup, test_put_interrupted, dav_fixture_teardown);
  g_test_add ("/dav/put-max-upload", DavFixture, GSIZE_TO_POINTER (16), dav_fixture_setup_max_upload, test_put_max_upload, dav_fixture_teardown);
  g_test_add ("/dav/depth-headers", DavFixture, NULL, dav_fixture_setup, test_depth_headers, dav_fixture_teardown);
  g_test_add ("/dav/request-body-too-large", DavFixture, NULL, dav_fixture_setup, test_request_body_too_large, dav_fixture_teardown);
  g_test_add ("/dav/mkcol", DavFixture, NULL, dav_fixture_setup, test_mkcol, dav_fixture_teardown);
  g_test_add ("/dav/move-copy", DavFixture, NULL, dav_fixture_setup, test_move_copy, dav_fixture_teardown);
  g_test_add ("/dav/movecopy-self-containment", DavFixture, NULL, dav_fixture_setup, test_movecopy_self_containment, dav_fixture_teardown);
#ifdef G_OS_UNIX
  g_test_add ("/dav/movecopy-symlink-destination", DavFixture, NULL, dav_fixture_setup, test_movecopy_symlink_destination, dav_fixture_teardown);
#endif
  g_test_add ("/dav/movecopy-path-traversal", DavFixture, NULL, dav_fixture_setup, test_movecopy_path_traversal, dav_fixture_teardown);
  g_test_add ("/dav/delete", DavFixture, NULL, dav_fixture_setup, test_delete, dav_fixture_teardown);

  g_test_add ("/dav/propfind/allprop", DavFixture, NULL, dav_fixture_setup, test_propfind_allprop, dav_fixture_teardown);
  g_test_add ("/dav/propfind/prop", DavFixture, NULL, dav_fixture_setup, test_propfind_prop, dav_fixture_teardown);
  g_test_add ("/dav/propfind/propname", DavFixture, NULL, dav_fixture_setup, test_propfind_propname, dav_fixture_teardown);
  g_test_add ("/dav/propfind/dedup", DavFixture, NULL, dav_fixture_setup, test_propfind_dedup, dav_fixture_teardown);
  g_test_add ("/dav/propfind/escaping", DavFixture, NULL, dav_fixture_setup, test_propfind_escaping, dav_fixture_teardown);
  g_test_add ("/dav/propfind/empty-body", DavFixture, NULL, dav_fixture_setup, test_propfind_empty_body, dav_fixture_teardown);
  g_test_add ("/dav/propfind/errors", DavFixture, NULL, dav_fixture_setup, test_propfind_errors, dav_fixture_teardown);
  g_test_add ("/dav/proppatch", DavFixture, NULL, dav_fixture_setup, test_proppatch, dav_fixture_teardown);
  g_test_add ("/dav/proppatch/empty-prop", DavFixture, NULL, dav_fixture_setup, test_proppatch_empty_prop, dav_fixture_teardown);

  g_test_add ("/dav/lock", DavFixture, NULL, dav_fixture_setup, test_lock, dav_fixture_teardown);
  g_test_add ("/dav/delete-locked", DavFixture, NULL, dav_fixture_setup, test_delete_locked, dav_fixture_teardown);
  g_test_add ("/dav/lock-escaping", DavFixture, NULL, dav_fixture_setup, test_lock_escaping, dav_fixture_teardown);
  g_test_add ("/dav/lock-double-slash", DavFixture, NULL, dav_fixture_setup, test_lock_double_slash, dav_fixture_teardown);
  g_test_add ("/dav/lock/depth-infinity", DavFixture, NULL, dav_fixture_setup, test_lock_depth_infinity, dav_fixture_teardown);
  g_test_add ("/dav/lock/expiry", DavFixture, NULL, dav_fixture_setup, test_lock_expiry, dav_fixture_teardown);
  g_test_add ("/dav/lock/tree-conflict", DavFixture, NULL, dav_fixture_setup, test_lock_tree_conflict, dav_fixture_teardown);
  g_test_add ("/dav/lock/refresh", DavFixture, NULL, dav_fixture_setup, test_lock_refresh, dav_fixture_teardown);
  g_test_add ("/dav/lock/refresh-forms", DavFixture, NULL, dav_fixture_setup, test_lock_refresh_forms, dav_fixture_teardown);
  g_test_add ("/dav/if-etag", DavFixture, NULL, dav_fixture_setup, test_if_etag, dav_fixture_teardown);
  g_test_add ("/dav/if-malformed", DavFixture, NULL, dav_fixture_setup, test_if_malformed, dav_fixture_teardown);

  g_test_add ("/dav/server-properties", DavFixture, NULL, dav_fixture_setup, test_server_properties, dav_fixture_teardown);
  g_test_add ("/dav/propfind/quota", DavFixture, NULL, dav_fixture_setup, test_propfind_quota, dav_fixture_teardown);
  g_test_add ("/dav/lock/bad-request", DavFixture, NULL, dav_fixture_setup, test_lock_bad_request, dav_fixture_teardown);
  g_test_add ("/dav/lock/failed-does-not-leak", DavFixture, NULL, dav_fixture_setup, test_lock_failed_does_not_leak, dav_fixture_teardown);
  g_test_add ("/dav/unlock/no-token", DavFixture, NULL, dav_fixture_setup, test_unlock_no_token, dav_fixture_teardown);
  g_test_add ("/dav/locked-parent", DavFixture, NULL, dav_fixture_setup, test_locked_parent, dav_fixture_teardown);
  g_test_add ("/dav/locked-destination", DavFixture, NULL, dav_fixture_setup, test_locked_destination, dav_fixture_teardown);

  g_test_add ("/dav/readonly", DavFixture, GINT_TO_POINTER (1), dav_fixture_setup, test_readonly, dav_fixture_teardown);
  g_test_add ("/dav/request-path-validation", DavFixture, NULL, dav_fixture_setup, test_request_path_validation, dav_fixture_teardown);
  g_test_add ("/dav/shared-server", DavFixture, NULL, dav_fixture_setup_shared, test_shared_server, dav_fixture_teardown);

  return g_test_run ();
}
