/*
 * Copyright (C) 2026 Red Hat, Inc.
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

/* the response to a PUT is sent as soon as the request is received,
 * while the body is still being streamed to the file: wait for the
 * expected contents to appear before reading the file back */
static void
wait_file_contents (const gchar *path, const gchar *expected)
{
  g_autofree gchar *contents = NULL;
  gint i;

  for (i = 0; i < 500; i++)
    {
      g_clear_pointer (&contents, g_free);

      if (g_file_get_contents (path, &contents, NULL, NULL) &&
          g_strcmp0 (contents, expected) == 0)
        return;

      g_usleep (1000);
    }

  /* the expected contents never appeared */
  g_assert_cmpstr (contents, ==, expected);
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

static void
dav_fixture_setup (DavFixture *f, gconstpointer user_data)
{
  gboolean readonly = user_data != NULL;
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
                            "read-only", readonly,
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

/* find the <D:response> element of a multistatus for @path, NULL if missing */
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

      if (g_strcmp0 ((gchar *) node->name, "response") != 0 || !href)
        continue;

      content = node_content (href);

      /* the href is the full request URI, with the base stripped it
       * compares equal to the request path */
      if (g_str_has_prefix (content, f->base_uri) &&
          g_strcmp0 (content + base_len, path) == 0)
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
  g_assert_nonnull (soup_message_headers_get_one (soup_message_get_response_headers (msg), "Allow"));
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
  /* infinite depth is not supported, it is rejected with a warning */
  g_test_expect_message ("phodav", G_LOG_LEVEL_WARNING, "*should not be reached*");
  g_assert_cmpuint (dav_status (f, "PROPFIND", "/", PROPFIND_ALLPROP, "Depth", "infinity", NULL),
                    ==, SOUP_STATUS_FORBIDDEN);
  g_test_assert_expected_messages ();
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

  /* a missing resource is reported per-property, in a multistatus */
  {
    g_autoptr(GBytes) missing_body = NULL;
    g_autoptr(SoupMessage) missing_msg = NULL;
    g_autoptr(xmlDoc) missing_doc = NULL;

    g_test_expect_message ("phodav", G_LOG_LEVEL_WARNING, "failed to set property: *");
    missing_msg = dav_request (f, "PROPPATCH", "/missing.txt", PROPPATCH_SET, &missing_body, NULL);
    g_test_assert_expected_messages ();
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
  g_test_expect_message ("phodav", G_LOG_LEVEL_WARNING, "*should not be reached*");
  g_assert_cmpuint (dav_status (f, "PROPPATCH", "/hello.txt",
                                "<D:propertyupdate xmlns:D=\"DAv:\"><D:set><D:prop>"
                                "<user.phodav-test>value</user.phodav-test>"
                                "</D:prop></D:set></D:propertyupdate>",
                                NULL),
                    ==, SOUP_STATUS_BAD_REQUEST);
  g_test_assert_expected_messages ();
}

/* take an exclusive write lock, returns the Lock-Token header value */
static gchar *
lock_resource (DavFixture *f, const gchar *path, const gchar *timeout)
{
  g_autoptr(GBytes) body = NULL;
  g_autoptr(SoupMessage) msg = dav_request (f, "LOCK", path, LOCK_EXCLUSIVE, &body,
                                            "Timeout", timeout, "Depth", "0", NULL);
  const gchar *token;

  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_CREATED);
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
  g_test_expect_message ("phodav", G_LOG_LEVEL_WARNING, "lock failed");
  g_assert_cmpuint (dav_status (f, "LOCK", "/locked.txt", LOCK_EXCLUSIVE, NULL),
                    ==, SOUP_STATUS_LOCKED);
  g_test_assert_expected_messages ();

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
test_lock_double_slash (DavFixture *f, gconstpointer user_data)
{
  /* empty path segments are ignored: a lock taken on a resource
   * addressed with a doubled slash can be released again under the
   * same (or any other equivalent) form of the path */
  g_autofree gchar *token = lock_resource (f, "//locked.txt", "Second-3600");

  g_assert_cmpuint (dav_status (f, "UNLOCK", "//locked.txt", NULL,
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

    wait_file_contents (path, "data");
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

  /* every property can be read back */
  g_object_get (f->phodav,
                "root", &root,
                "root-file", &root_file,
                "server", &server,
                "read-only", &readonly,
                NULL);

  g_assert_cmpstr (root, ==, f->dir);
  g_assert_nonnull (root_file);
  g_assert_true (SOUP_IS_SERVER (server));
  g_assert_false (readonly);

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
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(GBytes) body = NULL;

  /* lock a file inside the directory */
  msg = dav_request (f, "LOCK", "/dir/inner.txt", LOCK_EXCLUSIVE, &body, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  g_clear_object (&msg);
  g_clear_pointer (&body, g_bytes_unref);

  /* modifications of the locked file are rejected without the token */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/dir/inner.txt", "data", NULL),
                    ==, SOUP_STATUS_LOCKED);

  /* deleting the directory removes the locked member with it */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/dir", NULL, NULL),
                    ==, SOUP_STATUS_NO_CONTENT);

  {
    g_autofree gchar *path = g_build_filename (f->dir, "dir", NULL);

    g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  }

  /* deleting something which does not exist */
  g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/nonexistent", NULL, NULL),
                    ==, SOUP_STATUS_NOT_FOUND);
}

static void
test_if_malformed (DavFixture *f, gconstpointer user_data)
{
  const gchar *headers[] = {
    "(<no-close",
    "([",
    "([\"etag",
    "([\"etag\"",
    "(\"etag\")",
    "Not",
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (headers); i++)
    g_assert_cmpuint (dav_status (f, SOUP_METHOD_DELETE, "/hello.txt", NULL,
                                  "If", headers[i], NULL),
                      ==, SOUP_STATUS_BAD_REQUEST);

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
  wait_file_contents (path, "matched");

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
  wait_file_contents (path, "star");
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
  wait_file_contents (path, "written");
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
  g_test_expect_message ("phodav", G_LOG_LEVEL_WARNING, "lock failed");
  g_assert_cmpuint (dav_status (f, "LOCK", "/dir", LOCK_EXCLUSIVE,
                                "Depth", "infinity", NULL),
                    ==, SOUP_STATUS_LOCKED);
  g_test_assert_expected_messages ();

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

  /* a chunked body has no Content-Length to check: it is refused
   * when the handler parses it */
  {
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *uri = g_build_path ("/", f->base_uri, "/hello.txt", NULL);
    g_autoptr(GBytes) request = g_bytes_new (big, strlen (big));
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
    g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_BAD_REQUEST);
  }
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

  /* the write failure is reported as a warning by the server */
  g_test_expect_message ("phodav", G_LOG_LEVEL_WARNING, "PUT: write error: *");

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
  g_test_assert_expected_messages ();

  /* the partial file was discarded */
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  /* the same happens when overwriting an existing file: the upload
   * fails, and what remains is removed rather than left truncated */
  {
    g_autofree gchar *data = g_strnfill (64, 'x');
    g_autofree gchar *hello = g_build_filename (f->dir, "hello.txt", NULL);

    g_test_expect_message ("phodav", G_LOG_LEVEL_WARNING, "PUT: write error: *");
    g_assert_cmpuint (dav_status (f, SOUP_METHOD_PUT, "/hello.txt", data, NULL),
                      ==, SOUP_STATUS_INTERNAL_SERVER_ERROR);
    g_test_assert_expected_messages ();

    g_assert_false (g_file_test (hello, G_FILE_TEST_EXISTS));
  }

  signal (SIGXFSZ, SIG_DFL);
  g_assert_cmpint (setrlimit (RLIMIT_FSIZE, &old), ==, 0);
#else
  g_test_skip ("not supported on this platform");
#endif
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
  g_test_add ("/dav/put-write-error", DavFixture, NULL, dav_fixture_setup, test_put_write_error, dav_fixture_teardown);
  g_test_add ("/dav/depth-headers", DavFixture, NULL, dav_fixture_setup, test_depth_headers, dav_fixture_teardown);
  g_test_add ("/dav/request-body-too-large", DavFixture, NULL, dav_fixture_setup, test_request_body_too_large, dav_fixture_teardown);
  g_test_add ("/dav/mkcol", DavFixture, NULL, dav_fixture_setup, test_mkcol, dav_fixture_teardown);
  g_test_add ("/dav/move-copy", DavFixture, NULL, dav_fixture_setup, test_move_copy, dav_fixture_teardown);
  g_test_add ("/dav/delete", DavFixture, NULL, dav_fixture_setup, test_delete, dav_fixture_teardown);

  g_test_add ("/dav/propfind/allprop", DavFixture, NULL, dav_fixture_setup, test_propfind_allprop, dav_fixture_teardown);
  g_test_add ("/dav/propfind/prop", DavFixture, NULL, dav_fixture_setup, test_propfind_prop, dav_fixture_teardown);
  g_test_add ("/dav/propfind/propname", DavFixture, NULL, dav_fixture_setup, test_propfind_propname, dav_fixture_teardown);
  g_test_add ("/dav/propfind/empty-body", DavFixture, NULL, dav_fixture_setup, test_propfind_empty_body, dav_fixture_teardown);
  g_test_add ("/dav/propfind/errors", DavFixture, NULL, dav_fixture_setup, test_propfind_errors, dav_fixture_teardown);
  g_test_add ("/dav/proppatch", DavFixture, NULL, dav_fixture_setup, test_proppatch, dav_fixture_teardown);

  g_test_add ("/dav/lock", DavFixture, NULL, dav_fixture_setup, test_lock, dav_fixture_teardown);
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
  g_test_add ("/dav/unlock/no-token", DavFixture, NULL, dav_fixture_setup, test_unlock_no_token, dav_fixture_teardown);
  g_test_add ("/dav/locked-parent", DavFixture, NULL, dav_fixture_setup, test_locked_parent, dav_fixture_teardown);

  g_test_add ("/dav/readonly", DavFixture, GINT_TO_POINTER (1), dav_fixture_setup, test_readonly, dav_fixture_teardown);

  return g_test_run ();
}
