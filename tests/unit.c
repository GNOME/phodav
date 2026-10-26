/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>

#include "phodav-backend.h"
#include "phodav-lock.h"
#include "phodav-path.h"
#include "phodav-priv.h"
#include "phodav-server-priv.h"
#include "phodav-utils.h"

/* phodav-method-lock.c generates "urn:uuid:<uuid>" tokens and
 * dav_lock_new() only accepts tokens of that length */
static gchar *
new_token (void)
{
  g_autofree gchar *uuid = g_uuid_string_random ();

  return g_strdup_printf ("urn:uuid:%s", uuid);
}

static Path *
path_new_test (const gchar *path)
{
  Path *p = g_new0 (Path, 1);

  p->path = g_strdup (path);
  p->refs = 1;

  return p;
}

static xmlNodePtr
find_child (xmlNodePtr parent, const gchar *name)
{
  for (xmlNodePtr node = parent->children; node; node = node->next)
    if (xml_node_is_element (node) && g_strcmp0 ((gchar *) node->name, name) == 0)
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

static void
test_remove_trailing (void)
{
  struct
  {
    const gchar *str;
    gchar c;
    const gchar *expected;
  } cases[] = {
    { "", '/', "" },
    { "///", '/', "" },
    { "/a", '/', "/a" },
    { "/a///", '/', "/a" },
    { "ab///", '/', "ab" },
    { "ab", 'x', "ab" },
    { "abxxx", 'x', "ab" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree gchar *str = g_strdup (cases[i].str);

      remove_trailing (str, cases[i].c);
      g_assert_cmpstr (str, ==, cases[i].expected);
    }
}

static void
test_path_has_dot_dot_segment (void)
{
  const gchar *dot_dot[] = { "..", "/..", "/../a", "/a/../b", "/a/..", "a/..", "/a/../.." };
  const gchar *not_dot_dot[] = { "", "/", "/a", "a/b", "..foo", "foo..", "a..b", "..a/b", "a/..b", ".", "/a/./b" };

  for (guint i = 0; i < G_N_ELEMENTS (dot_dot); i++)
    g_assert_true (path_has_dot_dot_segment (dot_dot[i]));

  for (guint i = 0; i < G_N_ELEMENTS (not_dot_dot); i++)
    g_assert_false (path_has_dot_dot_segment (not_dot_dot[i]));

  /* the backslash separates the path components on Windows only:
   * a request path carrying dot-dot segments behind backslashes
   * would traverse there, while it names an ordinary file on Unix */
#ifdef G_OS_WIN32
  g_assert_true (path_has_dot_dot_segment ("/a\\..\\..\\b"));
#else
  g_assert_false (path_has_dot_dot_segment ("/a\\..\\..\\b"));
#endif
}

static void
test_path_is_safe (void)
{
  const gchar *safe[] = { "/", "/a", "/a/b", "/..foo", "/a.." };
  const gchar *unsafe[] = { NULL, "", "a", "a/b", "//a", "/..", "/../a", "/a/../b" };

  for (guint i = 0; i < G_N_ELEMENTS (safe); i++)
    g_assert_true (phodav_path_is_safe (safe[i]));

  for (guint i = 0; i < G_N_ELEMENTS (unsafe); i++)
    g_assert_false (phodav_path_is_safe (unsafe[i]));

  /* the backslash separates the path components on Windows only */
#ifdef G_OS_WIN32
  g_assert_false (phodav_path_is_safe ("/a\\..\\..\\b"));
#else
  g_assert_true (phodav_path_is_safe ("/a\\..\\..\\b"));
#endif
}

static void
test_depth (void)
{
  DepthType depth = DEPTH_ZERO;

  /* a missing Depth header defaults to infinity (RFC 4918 section 9.1) */
  g_assert_true (depth_from_string (NULL, &depth));
  g_assert_cmpint (depth, ==, DEPTH_INFINITY);
  g_assert_true (depth_from_string ("0", &depth));
  g_assert_cmpint (depth, ==, DEPTH_ZERO);
  g_assert_true (depth_from_string ("1", &depth));
  g_assert_cmpint (depth, ==, DEPTH_ONE);
  g_assert_true (depth_from_string ("infinity", &depth));
  g_assert_cmpint (depth, ==, DEPTH_INFINITY);

  g_assert_cmpstr (depth_to_string (DEPTH_ZERO), ==, "0");
  g_assert_cmpstr (depth_to_string (DEPTH_ONE), ==, "1");
  g_assert_cmpstr (depth_to_string (DEPTH_INFINITY), ==, "infinity");

  /* invalid values are rejected, the caller answers with 400 */
  g_assert_false (depth_from_string ("2", &depth));
  g_assert_false (depth_from_string ("Infinity", &depth));
  g_assert_false (depth_from_string ("", &depth));
}

static void
test_path_is_covered (void)
{
  g_assert_true (path_is_covered ("/a", "/a"));
  g_assert_true (path_is_covered ("/a/b", "/a"));
  g_assert_true (path_is_covered ("/a/b/c", "/a"));

  g_assert_false (path_is_covered ("/ab", "/a"));
  g_assert_false (path_is_covered ("/", "/a"));
  g_assert_false (path_is_covered ("/a", "/a/b"));

  /* the root covers everything */
  g_assert_true (path_is_covered ("/", "/"));
  g_assert_true (path_is_covered ("/a/b", "/"));
  g_assert_false (path_is_covered ("a/b", "/"));
}

static void
test_etag_header_matches (void)
{
  /* RFC 9110 section 13.1: a list of quoted entity tags compared
   * weakly, or a star for any existing representation */
  g_assert_true (etag_header_matches ("\"abc\"", "abc", TRUE));
  g_assert_true (etag_header_matches ("W/\"abc\"", "abc", TRUE));
  g_assert_true (etag_header_matches ("\"xyz\", \"abc\"", "abc", TRUE));
  g_assert_true (etag_header_matches ("*", NULL, TRUE));
  g_assert_true (etag_header_matches ("*", "abc", TRUE));

  g_assert_false (etag_header_matches ("\"abc\"", "abd", TRUE));
  g_assert_false (etag_header_matches ("\"abc\"", "ab", TRUE));
  g_assert_false (etag_header_matches ("\"abc\"", NULL, TRUE));
  g_assert_false (etag_header_matches ("\"\"", "abc", TRUE));
  g_assert_false (etag_header_matches ("", "abc", TRUE));

  /* unquoted values are malformed, but compared leniently */
  g_assert_true (etag_header_matches ("abc", "abc", TRUE));
  g_assert_false (etag_header_matches ("abc", "abd", TRUE));

  /* the star matches any existing representation only */
  g_assert_false (etag_header_matches ("*", NULL, FALSE));
  g_assert_false (etag_header_matches ("*", "abc", FALSE));
}

static void
test_http_time_to_string (void)
{
  g_autofree gchar *date = http_time_to_string (0);

  g_assert_cmpstr (date, ==, "Thu, 01 Jan 1970 00:00:00 GMT");

  /* times beyond the year 9999 cannot be represented */
  g_assert_null (http_time_to_string ((guint64) G_MAXINT64));
}

/* set a header to an HTTP date, without leaking the formatted string */
static void
set_http_date_header (SoupMessageHeaders *headers, const gchar *name, guint64 time)
{
  g_autofree gchar *date = http_time_to_string (time);

  soup_message_headers_replace (headers, name, date);
}

static void
test_http_request_not_modified (void)
{
  g_autoptr(SoupMessageHeaders) headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);

  /* without a condition, the representation is always sent */
  g_assert_false (http_request_not_modified (headers, "abc", 1000));

  soup_message_headers_replace (headers, "If-None-Match", "\"abc\"");
  g_assert_true (http_request_not_modified (headers, "abc", 1000));
  g_assert_false (http_request_not_modified (headers, "xyz", 1000));

  /* If-None-Match takes precedence over If-Modified-Since */
  set_http_date_header (headers, "If-Modified-Since", 1000);
  g_assert_false (http_request_not_modified (headers, "xyz", 1000));
  g_assert_true (http_request_not_modified (headers, "abc", 2000));

  /* the representation is current when its last modification is
   * not after the condition date */
  soup_message_headers_remove (headers, "If-None-Match");
  g_assert_true (http_request_not_modified (headers, "xyz", 1000));
  g_assert_false (http_request_not_modified (headers, "xyz", 1001));

  /* an unparseable date is ignored */
  soup_message_headers_replace (headers, "If-Modified-Since", "not a date");
  g_assert_false (http_request_not_modified (headers, "xyz", 1000));

  /* without a modification time, the date cannot be checked */
  set_http_date_header (headers, "If-Modified-Since", 1000);
  g_assert_false (http_request_not_modified (headers, "xyz", 0));
}

static void
test_http_range (void)
{
  g_autoptr(SoupMessageHeaders) headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);
  guint64 first = 0, last = 0;

  /* no Range header: the whole representation is served */
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_NONE);

  soup_message_headers_replace (headers, "Range", "bytes=0-9");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_PARTIAL);
  g_assert_cmpuint (first, ==, 0);
  g_assert_cmpuint (last, ==, 9);

  /* an open-ended range goes to the end, the unit is case-insensitive */
  soup_message_headers_replace (headers, "Range", "BYTES=10-");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_PARTIAL);
  g_assert_cmpuint (first, ==, 10);
  g_assert_cmpuint (last, ==, 99);

  /* a last byte beyond the end is clamped to it */
  soup_message_headers_replace (headers, "Range", "bytes=90-200");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_PARTIAL);
  g_assert_cmpuint (first, ==, 90);
  g_assert_cmpuint (last, ==, 99);

  /* a suffix range covers the last bytes */
  soup_message_headers_replace (headers, "Range", "bytes=-10");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_PARTIAL);
  g_assert_cmpuint (first, ==, 90);
  g_assert_cmpuint (last, ==, 99);

  /* a suffix longer than the representation covers it entirely */
  soup_message_headers_replace (headers, "Range", "bytes=-200");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_PARTIAL);
  g_assert_cmpuint (first, ==, 0);
  g_assert_cmpuint (last, ==, 99);

  /* a first byte at or beyond the end, or an empty suffix, is
   * unsatisfiable, RFC 9110 section 14.1.1 */
  soup_message_headers_replace (headers, "Range", "bytes=100-");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_UNSATISFIABLE);
  soup_message_headers_replace (headers, "Range", "bytes=100-200");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_UNSATISFIABLE);
  soup_message_headers_replace (headers, "Range", "bytes=-0");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_UNSATISFIABLE);

  /* invalid, multi-range or unknown-unit requests are ignored */
  const gchar *ignored[] = { "bytes=9-2", "bytes=a-b", "bytes=5", "bytes=", "bytes=-5x", "bytes=0-4x", "bytes=0-9,20-29", "items=0-9" };

  for (guint i = 0; i < G_N_ELEMENTS (ignored); i++)
    {
      soup_message_headers_replace (headers, "Range", ignored[i]);
      g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_NONE);
    }

  /* an empty representation has no satisfiable range */
  soup_message_headers_replace (headers, "Range", "bytes=0-");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 0, &first, &last), ==, HTTP_RANGE_NONE);

  /* If-Range with the current etag applies the range, RFC 9110 13.1.5 */
  soup_message_headers_replace (headers, "Range", "bytes=0-9");
  soup_message_headers_replace (headers, "If-Range", "\"abc\"");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_PARTIAL);

  /* a weak or other etag does not match */
  soup_message_headers_replace (headers, "If-Range", "W/\"abc\"");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_NONE);
  soup_message_headers_replace (headers, "If-Range", "\"xyz\"");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_NONE);

  /* a date applies the range when the representation was not
   * modified after it */
  set_http_date_header (headers, "If-Range", 1000);
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_PARTIAL);
  set_http_date_header (headers, "If-Range", 999);
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_NONE);

  /* an unparseable If-Range is not a date either */
  soup_message_headers_replace (headers, "If-Range", "garbage");
  g_assert_cmpint (http_request_range (headers, "abc", 1000, 100, &first, &last), ==, HTTP_RANGE_NONE);
}

static void
test_timeout (void)
{
  g_assert_cmpuint (timeout_from_string (NULL), ==, 0);
  g_assert_cmpuint (timeout_from_string ("Infinite"), ==, 0);
  g_assert_cmpuint (timeout_from_string ("Second-3600"), ==, 3600);
  g_assert_cmpuint (timeout_from_string ("second-42"), ==, 42);
  g_assert_cmpuint (timeout_from_string ("Second-"), ==, 0);

  /* an unrecognized value is treated like a missing header */
  g_assert_cmpuint (timeout_from_string ("later"), ==, 0);
}

static void
test_xml_node_helpers (void)
{
  xmlDocPtr doc;
  xmlNodePtr root, child, plain, text;
  xmlNsPtr ns;
  g_autofree gchar *name = NULL;

  doc = xmlNewDoc (NULL);
  root = xmlNewDocNode (doc, NULL, BAD_CAST "propfind", NULL);
  xmlDocSetRootElement (doc, root);
  ns = xmlNewNs (root, BAD_CAST "DAV:", BAD_CAST "D");
  xmlSetNs (root, ns);
  child = xmlNewChild (root, ns, BAD_CAST "prop", NULL);

  plain = xmlNewNode (NULL, BAD_CAST "foo");
  text = xmlNewText (BAD_CAST "some text");

  g_assert_true (xml_node_is_element (root));
  g_assert_true (xml_node_is_element (child));
  g_assert_false (xml_node_is_element (text));
  g_assert_false (xml_node_is_element (NULL));

  g_assert_true (xml_node_has_name (child, "prop"));
  g_assert_false (xml_node_has_name (root, "prop"));
  g_assert_true (xml_node_has_name_ns (child, "prop", "DAV:"));
  g_assert_false (xml_node_has_name_ns (child, "prop", "urn:other"));
  g_assert_false (xml_node_has_name_ns (plain, "foo", "DAV:"));
  /* a NULL name or namespace matches anything */
  g_assert_true (xml_node_has_name_ns (child, NULL, "DAV:"));
  g_assert_true (xml_node_has_name_ns (child, "prop", NULL));

  name = xml_node_get_xattr_name (child, "user.");
  g_assert_cmpstr (name, ==, "user.DAV:#prop");
  g_clear_pointer (&name, g_free);
  name = xml_node_get_xattr_name (plain, "user.");
  g_assert_cmpstr (name, ==, "user.foo");

  xmlFreeNode (plain);
  xmlFreeNode (text);
  xmlFreeDoc (doc);
}

static void
test_path_lifecycle (void)
{
  /* the refcount of the path is the very subject of this test: it is
   * driven to zero by the lock teardown, so it must not be g_autoptr */
  /* gobject-linter-ignore-next-line: use_auto_cleanup */
  Path *p = path_new_test ("/a");
  g_autofree gchar *token = new_token ();
  g_autoptr(DAVLock) lock = NULL;

  g_assert_cmpuint (p->refs, ==, 1);
  g_assert_true (path_ref (p) == p);
  g_assert_cmpuint (p->refs, ==, 2);
  path_unref (p);
  g_assert_cmpuint (p->refs, ==, 1);

  /* a lock holds a reference on the path */
  lock = dav_lock_new (p, token, DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                       DEPTH_ZERO, NULL, 60);
  g_assert_cmpuint (p->refs, ==, 2);
  g_assert_true (p->locks == NULL);

  path_add_lock (p, lock);
  g_assert_true (g_list_length (p->locks) == 1);
  path_remove_lock (p, lock);
  g_assert_null (p->locks);
  path_add_lock (p, lock);

  /* the trie reference goes away, the lock keeps the path alive */
  path_unref (p);
  g_assert_cmpuint (p->refs, ==, 1);

  /* freeing the lock drops the last reference, and the path
   * releases the locks it still holds */
  g_clear_pointer (&lock, dav_lock_free);
  /* p is freed here, there is nothing left to assert on */
}

static void
test_path_orphaned_locks (void)
{
  /* like test_path_lifecycle, the path references are released manually
   * to reach the last unref with the locks still registered */
  /* gobject-linter-ignore-next-line: use_auto_cleanup */
  Path *p = path_new_test ("/orphan");
  g_autofree gchar *token = new_token ();
  DAVLock *lock;

  /* simulate a path reaching its last reference while locks are still
   * registered: path_unref() must release the locks without corrupting
   * the list, as dav_lock_free() removes itself from it */
  lock = dav_lock_new (p, token, DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                       DEPTH_ZERO, NULL, 60);
  path_add_lock (p, lock);

  /* the path takes ownership of the lock */
  path_unref (p);
  path_unref (p);
}

static void
test_dav_lock_new (void)
{
  g_autoptr(Path) p = path_new_test ("/a");
  g_autofree gchar *token = new_token ();
  g_autoptr(DAVLock) lock = NULL;
  gint64 now = g_get_monotonic_time () / G_USEC_PER_SEC;

  /* invalid tokens are rejected */
  g_test_expect_message ("phodav", G_LOG_LEVEL_CRITICAL, "*strlen (token) == DAV_LOCK_TOKEN_LENGTH*");
  g_assert_null (dav_lock_new (p, "short", DAV_LOCK_SCOPE_SHARED, DAV_LOCK_WRITE,
                               DEPTH_ZERO, NULL, 60));
  g_test_assert_expected_messages ();
  g_test_expect_message ("phodav", G_LOG_LEVEL_CRITICAL, "*assertion 'token' failed*");
  g_assert_null (dav_lock_new (p, NULL, DAV_LOCK_SCOPE_SHARED, DAV_LOCK_WRITE,
                               DEPTH_ZERO, NULL, 60));
  g_test_assert_expected_messages ();

  lock = dav_lock_new (p, token, DAV_LOCK_SCOPE_SHARED, DAV_LOCK_WRITE,
                       DEPTH_INFINITY, NULL, 60);
  g_assert_nonnull (lock);
  g_assert_cmpstr (lock->token, ==, token);
  g_assert_true (lock->path == p);
  g_assert_cmpuint (lock->scope, ==, DAV_LOCK_SCOPE_SHARED);
  g_assert_cmpuint (lock->type, ==, DAV_LOCK_WRITE);
  g_assert_cmpuint (lock->depth, ==, DEPTH_INFINITY);
  g_assert_null (lock->owner);

  /* the timeout is an absolute deadline of now + timeout seconds */
  g_assert_cmpuint (lock->timeout, >, (guint64) now);
  g_assert_cmpuint (lock->timeout, <=, (guint64) now + 60);

  /* a missing or infinite timeout is capped too, so a client cannot
   * keep a lock forever */
  dav_lock_refresh_timeout (lock, 0);
  g_assert_cmpuint (lock->timeout, >, (guint64) now);
  g_assert_cmpuint (lock->timeout, <=, (guint64) now + PHODAV_LOCK_TIMEOUT_MAX);

  /* an oversized finite timeout is capped as well */
  dav_lock_refresh_timeout (lock, PHODAV_LOCK_TIMEOUT_MAX + 1000);
  g_assert_cmpuint (lock->timeout, >, (guint64) now);
  g_assert_cmpuint (lock->timeout, <=, (guint64) now + PHODAV_LOCK_TIMEOUT_MAX);

  dav_lock_refresh_timeout (lock, 10);
  g_assert_cmpuint (lock->timeout, >, (guint64) now);
  g_assert_cmpuint (lock->timeout, <=, (guint64) now + 70);
}

static void
test_dav_lock_activelock_node (void)
{
  xmlDocPtr doc = xmlNewDoc (NULL);
  xmlNodePtr owner = xmlNewDocNode (doc, NULL, BAD_CAST "owner", BAD_CAST "unit-test");
  xmlNsPtr ns = xmlNewNs (NULL, BAD_CAST "DAV:", BAD_CAST "D");
  g_autoptr(Path) p = path_new_test ("/a b/c");
  g_autofree gchar *token = new_token ();
  g_autoptr(DAVLock) lock = NULL;
  xmlNodePtr node;
  g_autofree gchar *content = NULL;

  lock = dav_lock_new (p, token, DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                       DEPTH_INFINITY, owner, 60);
  node = dav_lock_get_activelock_node (lock, ns);

  g_assert_cmpstr ((gchar *) node->name, ==, "activelock");

  g_assert_cmpstr ((gchar *) find_child (node, "locktype")->children->name, ==, "write");
  g_assert_cmpstr ((gchar *) find_child (node, "lockscope")->children->name, ==, "exclusive");

  content = node_content (find_child (node, "depth"));
  g_assert_cmpstr (content, ==, "infinity");
  g_clear_pointer (&content, g_free);

  content = node_content (find_child (node, "owner"));
  g_assert_cmpstr (content, ==, "unit-test");
  g_clear_pointer (&content, g_free);

  content = node_content (find_child (find_child (node, "locktoken"), "href"));
  g_assert_cmpstr (content, ==, token);
  g_clear_pointer (&content, g_free);

  /* the lockroot is a URI, its path must be escaped */
  content = node_content (find_child (find_child (node, "lockroot"), "href"));
  g_assert_cmpstr (content, ==, "/a%20b/c");
  g_clear_pointer (&content, g_free);

  content = node_content (find_child (node, "timeout"));
  /* the remaining timeout is relative to the current time, a second
   * may already have elapsed since the lock was taken */
  g_assert_nonnull (content);
  g_assert_true (g_str_has_prefix (content, "Second-"));
  {
    gint64 remaining = g_ascii_strtoll (content + strlen ("Second-"), NULL, 10);

    g_assert_cmpint (remaining, >, 0);
    g_assert_cmpint (remaining, <=, 60);
  }
  g_clear_pointer (&content, g_free);

  lock->timeout = g_get_monotonic_time () / G_USEC_PER_SEC + 42;

  /* the remaining timeout is serialized relative to the current time,
   * the node has to be rebuilt to pick up the new value */
  xmlFreeNode (node);
  node = dav_lock_get_activelock_node (lock, ns);
  content = node_content (find_child (node, "timeout"));
  g_assert_true (g_str_has_prefix (content, "Second-4"));
  {
    gint64 remaining = g_ascii_strtoll (content + strlen ("Second-"), NULL, 10);

    g_assert_cmpint (remaining, >=, 41);
    g_assert_cmpint (remaining, <=, 42);
  }
  g_clear_pointer (&content, g_free);

  /* infinite locks have no timeout element */
  lock->timeout = 0;
  xmlFreeNode (node);
  node = dav_lock_get_activelock_node (lock, ns);
  g_assert_null (find_child (node, "timeout"));

  xmlFreeNode (node);
  xmlFreeNs (ns);
  xmlFreeNode (owner);
  xmlFreeDoc (doc);
}

/* check whether a lock is matched by a submitted (path, token) pair */
static void
check_locks_submitted (DAVLock *lock, const gchar *path, const gchar *token, gboolean expected)
{
  g_autolist(LockSubmitted) locks = g_list_append (NULL, lock_submitted_new (path, token));

  g_assert_true (locks_submitted_has (locks, lock) == expected);
}

static void
test_lock_submitted (void)
{
  g_autoptr(Path) p = path_new_test ("/a");
  g_autofree gchar *token = new_token ();
  g_autoptr(DAVLock) lock = NULL;
  g_autoptr(LockSubmitted) submitted = NULL;

  lock = dav_lock_new (p, token, DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                       DEPTH_ZERO, NULL, 60);

  submitted = lock_submitted_new ("/a/", token);
  g_assert_cmpstr (submitted->path, ==, "/a");
  g_assert_cmpstr (submitted->token, ==, token);

  g_assert_false (locks_submitted_has (NULL, lock));

  check_locks_submitted (lock, "/a", token, TRUE);

  /* the token may also be submitted for a resource covered by the lock */
  check_locks_submitted (lock, "/a/b/c", token, TRUE);

  /* but not for a sibling path */
  check_locks_submitted (lock, "/ab", token, FALSE);

  /* a different token does not match */
  check_locks_submitted (lock, "/a", "urn:uuid:00000000-0000-0000-0000-000000000000", FALSE);

  /* a different path does not match either */
  check_locks_submitted (lock, "/other", token, FALSE);
}

static void
test_server_paths (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *token = new_token ();

  g_assert_cmpint (phodav_lock_store_add_lock (server, "/a", token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);

  /* trailing slashes are ignored, and locks on parent paths apply to
   * their children */
  g_assert_true (phodav_lock_store_has_token (server, "/a", token));
  g_assert_true (phodav_lock_store_has_token (server, "/a/", token));
  g_assert_true (phodav_lock_store_has_token (server, "/a/b/c", token));
  g_assert_false (phodav_lock_store_has_token (server, "/ab", token));
  g_assert_false (phodav_lock_store_has_token (server, "/b", token));

  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/a", token),
                   ==, SOUP_STATUS_NO_CONTENT);
  g_assert_false (phodav_lock_store_has_token (server, "/a/b/c", token));
}

static void
test_server_trie_paths (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *root_token = new_token ();
  g_autofree gchar *member_token = new_token ();
  g_autofree gchar *deep_token = new_token ();
  g_autofree gchar *sibling_token = new_token ();

  /* a lock on the trie root covers the resources below it */
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/", root_token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);
  g_assert_true (phodav_lock_store_has_token (server, "/a/b", root_token));
  g_assert_true (phodav_lock_store_path_has_other_locks (server, "/a/b", NULL));
  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/", root_token),
                   ==, SOUP_STATUS_NO_CONTENT);

  g_assert_cmpint (phodav_lock_store_add_lock (server, "/coll/member", member_token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/coll/other/deep", deep_token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/collateral", sibling_token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);

  /* removing a subtree only visits its trie branch, not a similarly
   * named sibling */
  phodav_lock_store_remove_locks (server, "/coll");
  g_assert_false (phodav_lock_store_has_token (server, "/coll/member", member_token));
  g_assert_false (phodav_lock_store_has_token (server, "/coll/other/deep", deep_token));
  g_assert_true (phodav_lock_store_has_token (server, "/collateral", sibling_token));

  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/collateral", sibling_token),
                   ==, SOUP_STATUS_NO_CONTENT);
}

static void
test_server_lock_manager (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *token = new_token ();
  g_autofree gchar *other = new_token ();
  g_autolist(LockSubmitted) submitted = NULL;

  g_assert_cmpint (phodav_lock_store_add_lock (server, "/a", token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);

  /* a resource is locked when a lock is not submitted */
  g_assert_true (phodav_lock_store_path_has_other_locks (server, "/a/b", NULL));
  g_assert_true (phodav_lock_store_path_has_other_locks (server, "/a/b/c", NULL));
  g_assert_false (phodav_lock_store_path_has_other_locks (server, "/x/y", NULL));

  /* submitting the lock makes the resource accessible */
  submitted = g_list_append (NULL, lock_submitted_new ("/a", token));
  g_assert_false (phodav_lock_store_path_has_other_locks (server, "/a/b", submitted));
  g_assert_false (phodav_lock_store_path_has_other_locks (server, "/a/b/c", submitted));

  /* submitting another token does not help */
  lock_submitted_free (submitted->data);
  submitted->data = lock_submitted_new ("/a", other);
  g_assert_true (phodav_lock_store_path_has_other_locks (server, "/a/b", submitted));

  /* the lock is released with its token */
  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/a", token),
                   ==, SOUP_STATUS_NO_CONTENT);
  g_assert_false (phodav_lock_store_has_token (server, "/a/b/c", token));
}

static void
test_server_tree_other_locks (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *token = new_token ();
  g_autofree gchar *other = new_token ();
  g_autolist(LockSubmitted) submitted = NULL;

  g_assert_cmpint (phodav_lock_store_add_lock (server, "/coll/member", token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/other", other,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);

  /* locks on the collection and on its members are in the tree */
  g_assert_true (phodav_lock_store_tree_has_other_locks (server, "/coll", NULL));
  g_assert_true (phodav_lock_store_tree_has_other_locks (server, "/coll/member", NULL));

  /* a lock outside the tree, or on a resource with a similar name,
   * does not count */
  g_assert_false (phodav_lock_store_tree_has_other_locks (server, "/elsewhere", NULL));
  g_assert_false (phodav_lock_store_tree_has_other_locks (server, "/col", NULL));

  /* an untagged submission on the collection does not submit the token
   * of a locked member */
  submitted = g_list_append (NULL, lock_submitted_new ("/coll", token));
  g_assert_true (phodav_lock_store_tree_has_other_locks (server, "/coll", submitted));

  /* the submission for the locked member itself does */
  lock_submitted_free (submitted->data);
  submitted->data = lock_submitted_new ("/coll/member", token);
  g_assert_false (phodav_lock_store_tree_has_other_locks (server, "/coll", submitted));

  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/coll/member", token),
                   ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/other", other),
                   ==, SOUP_STATUS_NO_CONTENT);
}

static void
test_dav_lock_expiry (void)
{
  /* the path references are released manually, the expiry drives the
   * last unrefs of the locks and of the path */
  /* gobject-linter-ignore-next-line: use_auto_cleanup */
  Path *p = path_new_test ("/expire");
  g_autofree gchar *active_token = new_token ();
  g_autofree gchar *expired_token = new_token ();
  DAVLock *active, *expired;

  /* an infinite lock never expires */
  active = dav_lock_new (p, active_token, DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                         DEPTH_ZERO, NULL, 0);
  g_assert_false (dav_lock_is_expired (active));
  path_add_lock (p, active);

  /* a lock whose deadline has passed does */
  expired = dav_lock_new (p, expired_token, DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                          DEPTH_ZERO, NULL, 60);
  g_assert_false (dav_lock_is_expired (expired));
  expired->timeout = g_get_monotonic_time () / G_USEC_PER_SEC - 1;
  g_assert_true (dav_lock_is_expired (expired));
  path_add_lock (p, expired);

  /* the sweep frees only the expired locks */
  path_expire_locks (p);
  g_assert_cmpuint (g_list_length (p->locks), ==, 1);
  g_assert_true (p->locks->data == active);

  /* the path releases the remaining lock */
  path_unref (p);
  path_unref (p);
}

static void
test_server_lock_expiry (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *token = new_token ();

  g_assert_cmpint (phodav_lock_store_add_lock (server, "/a", token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 1),
                   ==, SOUP_STATUS_OK);

  /* the lock is found while it has not expired */
  g_assert_true (phodav_lock_store_has_token (server, "/a/b", token));

  /* the expired lock is not honored anymore, and the sweep reclaims
   * the empty paths from the trie */
  g_usleep (G_USEC_PER_SEC + 100000);
  g_assert_false (phodav_lock_store_has_token (server, "/a/b", token));
  phodav_lock_store_expire_locks (server);
}

static void
test_server_expiry_index (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *slow_token = new_token ();
  g_autofree gchar *fast_token = new_token ();
  gint64 now = g_get_monotonic_time () / G_USEC_PER_SEC;
  g_autofree gchar *refreshed = NULL;
  gint64 next;

  g_assert_cmpint (phodav_lock_store_add_lock (server, "/slow", slow_token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 100),
                   ==, SOUP_STATUS_OK);
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/fast", fast_token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 10),
                   ==, SOUP_STATUS_OK);

  /* the expiry index returns the earliest deadline without walking
   * the paths again */
  next = phodav_lock_store_next_expiry (server);
  g_assert_cmpint (next, >, now);
  g_assert_cmpint (next, <=, now + 10);

  /* refreshing the earliest lock moves it to the front of the index */
  {
    g_autolist(LockSubmitted) submitted =
                 g_list_append (NULL, lock_submitted_new ("/fast", fast_token));

    g_assert_cmpint (phodav_lock_store_refresh_lock (server, "/fast", submitted,
                                                     5, &refreshed),
                     ==, SOUP_STATUS_OK);
    g_assert_cmpstr (refreshed, ==, fast_token);
  }
  next = phodav_lock_store_next_expiry (server);
  g_assert_cmpint (next, >, now);
  g_assert_cmpint (next, <=, now + 5);

  /* removing the earliest lock leaves the other deadline in place */
  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/fast", fast_token),
                   ==, SOUP_STATUS_NO_CONTENT);
  next = phodav_lock_store_next_expiry (server);
  g_assert_cmpint (next, >, now + 5);
  g_assert_cmpint (next, <=, now + 100);

  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/slow", slow_token),
                   ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpint (phodav_lock_store_next_expiry (server), ==, 0);
}

static void
test_server_lock_limit (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *first_token = new_token ();
  g_autofree gchar *extra_token = new_token ();
  g_autofree gchar *replacement_token = new_token ();

  for (guint i = 0; i < PHODAV_LOCK_COUNT_MAX; i++)
    {
      g_autofree gchar *path = g_strdup_printf ("/limited/%u", i);
      g_autofree gchar *token = i == 0 ? g_strdup (first_token) : new_token ();

      g_assert_cmpint (phodav_lock_store_add_lock (server, path, token,
                                                   DAV_LOCK_SCOPE_SHARED,
                                                   DAV_LOCK_WRITE, DEPTH_ZERO,
                                                   NULL, 60),
                       ==, SOUP_STATUS_OK);
    }

  /* the store refuses one more lock, but accepts another one after a
   * lock was released */
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/limited/extra", extra_token,
                                               DAV_LOCK_SCOPE_SHARED,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 60),
                   ==, SOUP_STATUS_INSUFFICIENT_STORAGE);
  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/limited/0", first_token),
                   ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/limited/extra", replacement_token,
                                               DAV_LOCK_SCOPE_SHARED,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 60),
                   ==, SOUP_STATUS_OK);
}

static void
test_server_lock_conflict (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *member_token = new_token ();
  g_autofree gchar *tree_token = new_token ();

  /* a lock on a member of the collection */
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/coll/member", member_token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);

  /* an exclusive depth-infinity lock on the collection conflicts with
   * it (RFC 4918 section 7.4)... */
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/coll", tree_token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_INFINITY,
                                               NULL, 0),
                   ==, SOUP_STATUS_LOCKED);

  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/coll/member", member_token),
                   ==, SOUP_STATUS_NO_CONTENT);

  /* ...but a shared tree lock does not conflict with a shared member lock */
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/coll/member", member_token,
                                               DAV_LOCK_SCOPE_SHARED,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/coll", tree_token,
                                               DAV_LOCK_SCOPE_SHARED,
                                               DAV_LOCK_WRITE, DEPTH_INFINITY,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);

  /* a depth-infinity lock elsewhere does not conflict either */
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/other", tree_token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_INFINITY,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);

  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/coll/member", member_token),
                   ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/coll", tree_token),
                   ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/other", tree_token),
                   ==, SOUP_STATUS_NO_CONTENT);
}

static void
test_server_stop (void)
{
  g_autoptr(GFile) root = g_file_new_for_path (g_get_tmp_dir ());
  g_autoptr(PhodavServer) server = phodav_server_new_for_root_file (root);
  PhodavServer *request_ref = g_object_ref (server);

  g_assert_nonnull (server->root_handler);
  g_assert_cmpstr (server->handler_path, ==, "/");
  g_assert_false (g_cancellable_is_cancelled (server->cancellable));

  phodav_server_stop (server);

  /* the handler is unregistered and the in-flight work cancelled, even
   * though this reference (standing for a request) keeps the object
   * alive */
  g_assert_true (server->stopped);
  g_assert_true (g_cancellable_is_cancelled (server->cancellable));
  g_assert_null (server->root_handler);
  g_assert_null (server->handler_path);
  g_assert_null (server->expire_locks_source);

  /* stopping twice is harmless, and later property changes do not
   * register the handler again */
  phodav_server_stop (server);
  g_object_set (server, "base-path", "/dav", NULL);
  g_assert_null (server->root_handler);

  g_object_unref (request_ref);
}

static void
test_server_dispose (void)
{
  GLogLevelFlags fatal;
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);

  /* dispose may run more than once (g_object_run_dispose() followed
   * by the last unref): the second pass must not touch the cleared
   * SoupServer, make any critical fatal for the check */
  g_object_run_dispose (G_OBJECT (server));

  fatal = g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  /* the mask must be restored only after the last unref */
  g_object_unref (g_steal_pointer (&server));
  g_log_set_always_fatal (fatal);
}

/* the test does a real round trip on the injected server: a
 * synchronous client would block the context the server runs on, so
 * the asynchronous client API is used with a loop */
typedef struct _InjectData
{
  GMainLoop *loop;
  GBytes *body;
} InjectData;

static void
injected_read_cb (GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
  InjectData *d = user_data;

  d->body = soup_session_send_and_read_finish (SOUP_SESSION (source), result, NULL);
  g_main_loop_quit (d->loop);
}

static void
test_server_injected_soup_server (void)
{
  g_autoptr(SoupServer) soup = soup_server_new (NULL, NULL);
  g_autoptr(PhodavServer) server = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(SoupSession) session = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoslist(GUri) uris = NULL;
  g_autoptr(GUri) file_uri = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = NULL;
  g_autofree gchar *file = NULL;
  g_autofree gchar *path = NULL;
  InjectData d = {
    0,
  };

  dir = g_dir_make_tmp ("phodav-unit-XXXXXX", &error);
  g_assert_no_error (error);

  file = g_build_filename (dir, "hello.txt", NULL);
  g_assert_true (g_file_set_contents (file, "hello", -1, &error));
  g_assert_no_error (error);

  root = g_file_new_for_path (dir);

  /* an existing SoupServer can be injected at construction, together
   * with a root given as a path */
  server = g_object_new (PHODAV_TYPE_SERVER,
                         "server", soup,
                         "root", dir,
                         NULL);
  g_assert_true (phodav_server_get_soup_server (server) == soup);

  /* the redundant NULL "root-file" set during construction did not
   * clobber the root given with "root" */
  g_object_get (server, "root", &path, NULL);
  g_assert_cmpstr (path, ==, dir);

  /* the same works with a root given as a GFile */
  g_clear_object (&server);
  g_clear_pointer (&path, g_free);
  server = g_object_new (PHODAV_TYPE_SERVER,
                         "server", soup,
                         "root-file", root,
                         NULL);
  g_object_get (server, "root", &path, NULL);
  g_assert_cmpstr (path, ==, dir);

  /* the injected server serves the root like an internally created
   * one */
  g_assert_true (soup_server_listen_local (soup, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  g_assert_no_error (error);

  uris = soup_server_get_uris (soup);
  g_assert_nonnull (uris);
  file_uri = g_uri_parse_relative (uris->data, "hello.txt", G_URI_FLAGS_NONE, NULL);
  g_assert_nonnull (file_uri);

  session = soup_session_new ();
  msg = soup_message_new_from_uri (SOUP_METHOD_GET, file_uri);
  loop = g_main_loop_new (NULL, FALSE);
  d.loop = loop;

  soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT, NULL,
                                    injected_read_cb, &d);
  g_main_loop_run (loop);

  g_assert_nonnull (d.body);
  g_assert_cmpuint (soup_message_get_status (msg), ==, SOUP_STATUS_OK);
  g_assert_cmpmem (g_bytes_get_data (d.body, NULL), g_bytes_get_size (d.body), "hello", 5);

  g_bytes_unref (d.body);

  g_unlink (file);
  g_rmdir (dir);
}

static void
remove_recursively (const gchar *path)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(GFileEnumerator) e = NULL;
  GFileInfo *info;

  e = g_file_enumerate_children (file, "standard::name,standard::type",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 NULL, NULL);
  while (e && (info = g_file_enumerator_next_file (e, NULL, NULL)))
    {
      g_autofree gchar *child = g_build_filename (path,
                                                  g_file_info_get_name (info),
                                                  NULL);

      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        remove_recursively (child);
      else
        g_unlink (child);

      g_object_unref (info);
    }

  g_rmdir (path);
}

static gchar *
make_test_tree (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = g_dir_make_tmp ("phodav-backend-XXXXXX", &error);
  g_autofree gchar *file = NULL;
  g_autofree gchar *subdir = NULL;
  g_autofree gchar *nested = NULL;

  g_assert_no_error (error);
  g_assert_nonnull (dir);

  file = g_build_filename (dir, "hello.txt", NULL);
  g_assert_true (g_file_set_contents (file, "hello", -1, &error));
  g_assert_no_error (error);

  subdir = g_build_filename (dir, "dir", NULL);
  g_assert_cmpint (g_mkdir (subdir, 0700), ==, 0);

  nested = g_build_filename (subdir, "inner.txt", NULL);
  g_assert_true (g_file_set_contents (nested, "inner", -1, &error));
  g_assert_no_error (error);

  g_clear_pointer (&nested, g_free);
  nested = g_build_filename (subdir, "subdir", NULL);
  g_assert_cmpint (g_mkdir (nested, 0700), ==, 0);

#ifdef G_OS_UNIX
  {
    g_autofree gchar *link = g_build_filename (dir, "link", NULL);
    g_autofree gchar *dangling = g_build_filename (dir, "dangling", NULL);

    g_assert_cmpint (symlink ("hello.txt", link), ==, 0);
    g_assert_cmpint (symlink ("missing", dangling), ==, 0);
  }
#endif

  return g_steal_pointer (&dir);
}

static PhodavPath *
gfile_path_new (PhodavBackend *backend, const gchar *path, const gchar *name)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);

  return phodav_path_new (backend, file, -1, -1, name);
}

static void
test_status_from_errno (void)
{
  g_assert_cmpint (phodav_path_status_from_errno (ENOENT, PHODAV_PATH_EXISTING, TRUE),
                   ==, SOUP_STATUS_NOT_FOUND);
  g_assert_cmpint (phodav_path_status_from_errno (ENOENT, PHODAV_PATH_CREATE, TRUE),
                   ==, SOUP_STATUS_NOT_FOUND);
  g_assert_cmpint (phodav_path_status_from_errno (ENOENT, PHODAV_PATH_CREATE, FALSE),
                   ==, SOUP_STATUS_CONFLICT);
  g_assert_cmpint (phodav_path_status_from_errno (EACCES, PHODAV_PATH_EXISTING, TRUE),
                   ==, SOUP_STATUS_FORBIDDEN);
  g_assert_cmpint (phodav_path_status_from_errno (EPERM, PHODAV_PATH_EXISTING, TRUE),
                   ==, SOUP_STATUS_FORBIDDEN);
#ifdef G_OS_UNIX
  g_assert_cmpint (phodav_path_status_from_errno (ELOOP, PHODAV_PATH_EXISTING, TRUE),
                   ==, SOUP_STATUS_FORBIDDEN);
  g_assert_cmpint (phodav_path_status_from_errno (ENOTDIR, PHODAV_PATH_EXISTING, TRUE),
                   ==, SOUP_STATUS_CONFLICT);
#endif
  g_assert_cmpint (phodav_path_status_from_errno (EIO, PHODAV_PATH_EXISTING, TRUE),
                   ==, SOUP_STATUS_INTERNAL_SERVER_ERROR);
}

static void
test_status_from_error (void)
{
  struct
  {
    GQuark domain;
    gint code;
    gint status;
  } cases[] = {
    { G_IO_ERROR, G_IO_ERROR_NOT_FOUND, SOUP_STATUS_NOT_FOUND },
    { G_FILE_ERROR, G_FILE_ERROR_NOENT, SOUP_STATUS_NOT_FOUND },
    { G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, SOUP_STATUS_FORBIDDEN },
    { G_IO_ERROR, G_IO_ERROR_READ_ONLY, SOUP_STATUS_FORBIDDEN },
    { G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, SOUP_STATUS_FORBIDDEN },
    { G_IO_ERROR, G_IO_ERROR_EXISTS, SOUP_STATUS_METHOD_NOT_ALLOWED },
    { G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY, SOUP_STATUS_CONFLICT },
    { G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, SOUP_STATUS_CONFLICT },
    { G_IO_ERROR, G_IO_ERROR_NOT_EMPTY, SOUP_STATUS_CONFLICT },
    { G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE, SOUP_STATUS_CONFLICT },
    { G_IO_ERROR, G_IO_ERROR_NO_SPACE, SOUP_STATUS_INSUFFICIENT_STORAGE },
    { G_IO_ERROR, G_IO_ERROR_CANCELLED, SOUP_STATUS_SERVICE_UNAVAILABLE },
    { G_IO_ERROR, G_IO_ERROR_FAILED, SOUP_STATUS_INTERNAL_SERVER_ERROR },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr(GError) error = g_error_new_literal (cases[i].domain,
                                                     cases[i].code, "test");

      g_assert_cmpint (phodav_status_from_error (error), ==, cases[i].status);
    }
}

static void
test_backend_selection (void)
{
  g_autofree gchar *dir = make_test_tree ();
  g_autoptr(GFile) root = g_file_new_for_path (dir);
  g_autoptr(GFile) remote = g_file_new_for_uri ("smb://example.invalid/share");
  g_autoptr(PhodavBackend) local = phodav_backend_new (root);
  g_autoptr(PhodavBackend) generic = phodav_backend_new (remote);

  g_assert_true (g_strcmp0 (local->klass->name, "native-dirfd") == 0 ||
                 g_strcmp0 (local->klass->name, "gfile") == 0);
  g_assert_cmpstr (generic->klass->name, ==, "gfile");

  remove_recursively (dir);
}

static void
test_gfile_backend_resolve (void)
{
  g_autofree gchar *dir = make_test_tree ();
  g_autoptr(GFile) root = g_file_new_for_path (dir);
  g_autoptr(PhodavBackend) backend = phodav_backend_gfile_new (root);
  g_autoptr(PhodavPath) path = NULL;
  gint status = 0;

  /* the root itself */
  path = phodav_backend_gfile_resolve (backend, NULL, "", PHODAV_PATH_EXISTING, &status);
  g_assert_nonnull (path);
  g_assert_nonnull (phodav_path_get_file (path));
  g_clear_pointer (&path, phodav_path_unref);

  /* an existing file and an existing directory */
  path = phodav_backend_gfile_resolve (backend, NULL, "/hello.txt", PHODAV_PATH_EXISTING, &status);
  g_assert_nonnull (path);
  g_assert_cmpstr (phodav_path_get_name (path), ==, "hello.txt");
  g_clear_pointer (&path, phodav_path_unref);

  path = phodav_backend_gfile_resolve (backend, NULL, "dir/inner.txt", PHODAV_PATH_EXISTING, &status);
  g_assert_nonnull (path);
  g_clear_pointer (&path, phodav_path_unref);

  /* a missing resource, existing or as the last component to create */
  path = phodav_backend_gfile_resolve (backend, NULL, "missing", PHODAV_PATH_EXISTING, &status);
  g_assert_null (path);
  g_assert_cmpint (status, ==, SOUP_STATUS_NOT_FOUND);

  path = phodav_backend_gfile_resolve (backend, NULL, "missing", PHODAV_PATH_CREATE, &status);
  g_assert_nonnull (path);
  g_clear_pointer (&path, phodav_path_unref);

  /* a missing intermediate component cannot be created */
  path = phodav_backend_gfile_resolve (backend, NULL, "missing/sub", PHODAV_PATH_CREATE, &status);
  g_assert_null (path);
  g_assert_cmpint (status, ==, SOUP_STATUS_CONFLICT);

  /* a non-directory used as an intermediate component */
  path = phodav_backend_gfile_resolve (backend, NULL, "hello.txt/sub", PHODAV_PATH_EXISTING, &status);
  g_assert_null (path);
  g_assert_cmpint (status, ==, SOUP_STATUS_CONFLICT);

#ifdef G_OS_UNIX
  /* symlinks are never followed */
  path = phodav_backend_gfile_resolve (backend, NULL, "link", PHODAV_PATH_EXISTING, &status);
  g_assert_null (path);
  g_assert_cmpint (status, ==, SOUP_STATUS_FORBIDDEN);

  path = phodav_backend_gfile_resolve (backend, NULL, "dangling", PHODAV_PATH_CREATE, &status);
  g_assert_null (path);
  g_assert_cmpint (status, ==, SOUP_STATUS_FORBIDDEN);
#endif

  /* resolve the same entries one by one below a handle */
  path = phodav_backend_gfile_resolve (backend, NULL, "dir", PHODAV_PATH_EXISTING, &status);
  g_assert_nonnull (path);
  {
    g_autoptr(PhodavPath) child = phodav_path_resolve_child (path, "inner.txt", PHODAV_PATH_EXISTING, &status);
    g_assert_nonnull (child);
  }
  {
    g_autoptr(PhodavPath) child = phodav_path_resolve_child (path, "missing", PHODAV_PATH_CREATE, &status);
    g_assert_nonnull (child);
  }
  {
    g_autoptr(PhodavPath) child = phodav_path_resolve_child (path, "missing", PHODAV_PATH_EXISTING, &status);
    g_assert_null (child);
    g_assert_cmpint (status, ==, SOUP_STATUS_NOT_FOUND);
  }
#ifdef G_OS_UNIX
  {
    g_autoptr(PhodavPath) child = phodav_path_resolve_child (path, "symlink", PHODAV_PATH_EXISTING, &status);
    g_assert_null (child);
  }
#endif
  g_clear_pointer (&path, phodav_path_unref);

  /* invalid child names are rejected before the backend is called */
  path = phodav_backend_gfile_resolve (backend, NULL, "dir", PHODAV_PATH_EXISTING, &status);
  g_assert_nonnull (path);
  {
    const gchar *invalid[] = { NULL, "", ".", "..", "a/b" };

    for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
      {
        g_autoptr(PhodavPath) child = phodav_path_resolve_child (path, invalid[i], PHODAV_PATH_EXISTING, &status);
        g_assert_null (child);
        g_assert_cmpint (status, ==, SOUP_STATUS_FORBIDDEN);
      }
  }
  g_clear_pointer (&path, phodav_path_unref);

  /* a handle without any filesystem object */
  path = phodav_path_new (backend, NULL, -1, -1, NULL);
  {
    g_autoptr(PhodavPath) child = phodav_path_resolve_child (path, "x", PHODAV_PATH_EXISTING, &status);
    g_assert_null (child);
    g_assert_cmpint (status, ==, SOUP_STATUS_NOT_FOUND);
  }
  g_clear_pointer (&path, phodav_path_unref);

  remove_recursively (dir);
}

static void
test_gfile_backend_operations (void)
{
  g_autofree gchar *dir = make_test_tree ();
  g_autoptr(GFile) root = g_file_new_for_path (dir);
  g_autoptr(PhodavBackend) backend = phodav_backend_gfile_new (root);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *file = g_build_filename (dir, "hello.txt", NULL);
  g_autofree gchar *subdir = g_build_filename (dir, "dir", NULL);
  g_autofree gchar *nested = g_build_filename (subdir, "inner.txt", NULL);
  g_autofree gchar *fresh = g_build_filename (dir, "fresh", NULL);
  g_autofree gchar *copy = g_build_filename (dir, "copy.txt", NULL);
  g_autofree gchar *moved = g_build_filename (dir, "moved.txt", NULL);
  g_autoptr(PhodavPath) no_file = phodav_path_new (backend, NULL, -1, -1, NULL);
  g_autoptr(PhodavPath) file_path = gfile_path_new (backend, file, "hello.txt");
  g_autoptr(PhodavPath) subdir_path = gfile_path_new (backend, subdir, NULL);
  g_autoptr(PhodavPath) fresh_path = gfile_path_new (backend, fresh, "fresh");
  g_autoptr(PhodavPath) copy_path = gfile_path_new (backend, copy, "copy.txt");
  g_autoptr(PhodavPath) moved_path = gfile_path_new (backend, moved, "moved.txt");
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GFileInputStream) stream = NULL;
  g_autoptr(GFileEnumerator) enumerator = NULL;

  /* query, open and enumerate a regular file and a directory */
  info = phodav_path_query_info (file_path, "standard::type", 0, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (info);
  g_clear_object (&info);

  stream = phodav_path_open (file_path, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (stream);
  g_clear_object (&stream);

  enumerator = phodav_path_enumerate (subdir_path, "standard::name", 0, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (enumerator);
  g_clear_object (&enumerator);

  /* all the operations fail cleanly without a filesystem object */
  g_assert_null (phodav_path_query_info (no_file, "standard::type", 0, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  g_assert_null (phodav_path_open (no_file, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  g_assert_null (phodav_path_enumerate (no_file, "standard::name", 0, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  g_assert_false (phodav_backend_gfile_unlink_child (no_file, "x", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  g_assert_false (phodav_backend_gfile_copy_file (no_file, file_path, 0, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  g_assert_false (phodav_backend_gfile_copy_file (file_path, no_file, 0, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  g_assert_false (phodav_backend_gfile_move (no_file, file_path, 0, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  g_assert_false (phodav_backend_gfile_move (file_path, no_file, 0, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  /* create, copy, rename and delete */
  g_assert_true (phodav_backend_gfile_mkdir (fresh_path, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (fresh, G_FILE_TEST_IS_DIR));
  g_assert_true (phodav_backend_gfile_unlink (fresh_path, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (fresh, G_FILE_TEST_EXISTS));

  g_assert_true (phodav_backend_gfile_copy_file (file_path, copy_path, 0, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (copy, G_FILE_TEST_EXISTS));
  g_assert_true (phodav_backend_gfile_unlink (copy_path, NULL, &error));
  g_assert_no_error (error);

  g_assert_true (phodav_backend_gfile_copy_file (file_path, copy_path, 0, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (phodav_backend_gfile_move (copy_path, moved_path, 0, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (copy, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (moved, G_FILE_TEST_EXISTS));
  g_assert_true (phodav_backend_gfile_unlink (moved_path, NULL, &error));
  g_assert_no_error (error);

  /* remove an entry relative to a directory handle */
  g_assert_true (phodav_path_unlink_child (subdir_path, "inner.txt", NULL, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (nested, G_FILE_TEST_EXISTS));

  remove_recursively (dir);
}

static void
test_path_copy_and_delete (void)
{
  g_autofree gchar *dir = make_test_tree ();
  g_autoptr(GFile) root = g_file_new_for_path (dir);
  g_autoptr(PhodavBackend) backend = phodav_backend_gfile_new (root);
  g_autoptr(GError) error = NULL;
  g_autoptr(PhodavPath) source = NULL;
  g_autoptr(PhodavPath) dest = NULL;
  g_autofree gchar *copied = g_build_filename (dir, "copied", NULL);
  g_autofree gchar *copied_inner = g_build_filename (copied, "inner.txt", NULL);
  g_autofree gchar *copied_sub = g_build_filename (copied, "subdir", NULL);
  g_autoptr(GPtrArray) removed = g_ptr_array_new_with_free_func (g_free);
  gint status = 0;

  /* copy a whole directory tree */
  source = phodav_backend_gfile_resolve (backend, NULL, "dir", PHODAV_PATH_EXISTING, &status);
  g_assert_nonnull (source);
  dest = phodav_backend_gfile_resolve (backend, NULL, "copied", PHODAV_PATH_CREATE, &status);
  g_assert_nonnull (dest);

  g_assert_true (phodav_path_copy (source, dest, 0, TRUE, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (copied_inner, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (copied_sub, G_FILE_TEST_IS_DIR));

  /* copying again onto the existing destination fails */
  g_assert_false (phodav_path_copy (source, dest, 0, TRUE, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);

  /* a non-recursive copy only creates the directory */
  g_clear_pointer (&dest, phodav_path_unref);
  {
    g_autofree gchar *shallow = g_build_filename (dir, "shallow", NULL);
    g_autofree gchar *shallow_inner = g_build_filename (shallow, "inner.txt", NULL);

    dest = phodav_backend_gfile_resolve (backend, NULL, "shallow", PHODAV_PATH_CREATE, &status);
    g_assert_nonnull (dest);
    g_assert_true (phodav_path_copy (source, dest, 0, FALSE, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_file_test (shallow, G_FILE_TEST_IS_DIR));
    g_assert_false (g_file_test (shallow_inner, G_FILE_TEST_EXISTS));
  }

  /* the tree removal collects the removed entries and visits every child */
  g_clear_pointer (&dest, phodav_path_unref);
  dest = phodav_backend_gfile_resolve (backend, NULL, "copied", PHODAV_PATH_EXISTING, &status);
  g_assert_nonnull (dest);
  g_assert_cmpint (phodav_path_delete_tree (dest, "/copied", NULL, NULL, removed, NULL),
                   ==, SOUP_STATUS_NO_CONTENT);
  g_assert_cmpuint (removed->len, >=, 2);
  g_assert_false (g_file_test (copied, G_FILE_TEST_EXISTS));

#ifdef G_OS_UNIX
  /* symlinked sources are refused, with either backend */
  {
    g_autofree gchar *link = g_build_filename (dir, "link", NULL);
    g_autofree gchar *link_copy = g_build_filename (dir, "link-copy", NULL);
    g_autoptr(PhodavPath) link_path = gfile_path_new (backend, link, "link");
    g_autoptr(PhodavPath) link_dest = gfile_path_new (backend, link_copy, "link-copy");

    g_assert_false (phodav_path_copy (link_path, link_dest, 0, TRUE, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error (&error);
  }
#endif

  /* a generic backend has no descriptor to enumerate a copy recursively */
  {
    g_autofree gchar *generic_source = g_build_filename (dir, "dir", NULL);
    g_autofree gchar *generic_dest = g_build_filename (dir, "generic-copy", NULL);
    g_autoptr(PhodavPath) from = gfile_path_new (backend, generic_source, NULL);
    g_autoptr(PhodavPath) to = gfile_path_new (backend, generic_dest, "generic-copy");

    g_assert_false (phodav_path_copy (from, to, 0, TRUE, NULL, &error));
    g_assert_nonnull (error);
    g_clear_error (&error);
  }

  remove_recursively (dir);
}

static void
test_path_operations (void)
{
  g_autofree gchar *dir = make_test_tree ();
  g_autoptr(GFile) root = g_file_new_for_path (dir);
  g_autoptr(PhodavBackend) backend = phodav_backend_gfile_new (root);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *file = g_build_filename (dir, "hello.txt", NULL);
  g_autofree gchar *renamed = g_build_filename (dir, "renamed.txt", NULL);
  g_autofree gchar *moved = g_build_filename (dir, "moved.txt", NULL);
  g_autoptr(PhodavPath) source = NULL;
  g_autoptr(PhodavPath) dest = NULL;
  g_autoptr(GFile) missing = NULL;
  g_autoptr(GFile) remote = NULL;
  gint status = 0;

  /* rename with the native descriptor fast path */
  source = phodav_backend_gfile_resolve (backend, NULL, "hello.txt", PHODAV_PATH_EXISTING, &status);
  g_assert_nonnull (source);
  dest = phodav_backend_gfile_resolve (backend, NULL, "renamed.txt", PHODAV_PATH_CREATE, &status);
  g_assert_nonnull (dest);
  g_assert_true (phodav_path_rename (source, dest, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (file, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (renamed, G_FILE_TEST_EXISTS));

  /* the generic implementations have no descriptors to operate on */
  g_clear_pointer (&source, phodav_path_unref);
  g_clear_pointer (&dest, phodav_path_unref);
  source = gfile_path_new (backend, renamed, "renamed.txt");
  dest = gfile_path_new (backend, moved, "moved.txt");
  g_assert_true (phodav_path_move (source, dest, 0, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (renamed, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (moved, G_FILE_TEST_EXISTS));

  /* sync an existing file, a missing one, and a non-native GFile */
  {
    g_autoptr(GFile) existing = g_file_new_for_path (moved);

    g_assert_true (phodav_path_sync (existing, NULL, &error));
    g_assert_no_error (error);
  }
  missing = g_file_new_for_path (renamed);
  g_assert_false (phodav_path_sync (missing, NULL, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  remote = g_file_new_for_uri ("smb://example.invalid/share/file");
  g_assert_true (phodav_path_sync (remote, NULL, &error));
  g_assert_no_error (error);

  /* the GFile based rename used by the PUT commit */
  {
    g_autoptr(GFile) from = g_file_new_for_path (moved);

    g_assert_true (phodav_path_rename_files (from, missing, NULL, &error));
    g_assert_no_error (error);
  }

  remove_recursively (dir);
}

static void
test_davdoc_parse (void)
{
  g_auto(DavDoc) dd = { 0 };
  const gchar *wrong = "<wrong/>";
  static const gchar zeros[4096];
  // gobject-linter-ignore-next-line: use_auto_cleanup
  SoupMessageBody *body;

  /* a document with an unexpected root element is rejected */
  body = soup_message_body_new ();
  soup_message_body_append (body, SOUP_MEMORY_COPY, wrong, strlen (wrong));
  g_assert_false (davdoc_parse (&dd, body, "multistatus"));
  soup_message_body_unref (body);

  /* and so is a body over the in-memory parser limit */
  body = soup_message_body_new ();
  for (guint i = 0; i < PHODAV_MAX_REQUEST_BODY / sizeof zeros + 1; i++)
    soup_message_body_append (body, SOUP_MEMORY_COPY, zeros, sizeof zeros);
  g_assert_false (davdoc_parse (&dd, body, "multistatus"));
  soup_message_body_unref (body);
}

static void
test_path_canonicalize (void)
{
  g_autofree gchar *path = NULL;

  path = phodav_path_canonicalize ("");
  g_assert_cmpstr (path, ==, "/");
  g_clear_pointer (&path, g_free);
  path = phodav_path_canonicalize ("/");
  g_assert_cmpstr (path, ==, "/");
  g_clear_pointer (&path, g_free);
  path = phodav_path_canonicalize ("///a//b///");
  g_assert_cmpstr (path, ==, "/a/b");
}

static void
test_handler_relative_path (void)
{
  g_autoptr(GFile) root = g_file_new_for_path (g_get_tmp_dir ());
  g_autoptr(PhodavServer) server = phodav_server_new_for_root_file (root);
  PathHandler *handler = server->root_handler;

  g_assert_cmpstr (handler_get_relative_path (handler, "/"), ==, "/");
  g_assert_cmpstr (handler_get_relative_path (handler, "/a/b"), ==, "/a/b");

  /* the share mounted at a prefix strips it */
  g_object_set (server, "base-path", "/dav", NULL);
  handler = server->root_handler;
  g_assert_cmpstr (handler_get_relative_path (handler, "/dav"), ==, "/");
  g_assert_cmpstr (handler_get_relative_path (handler, "/dav/a"), ==, "/a");
  g_assert_null (handler_get_relative_path (handler, "/other/a"));
  g_assert_true (handler_path_is_covered (handler, "/dav/a"));
  g_assert_false (handler_path_is_covered (handler, "/other/a"));

  /* a request path must be absolute */
  g_test_expect_message ("phodav", G_LOG_LEVEL_CRITICAL, "*path != NULL*");
  g_assert_null (handler_get_relative_path (handler, "relative"));
  g_test_assert_expected_messages ();
}

static void
test_server_properties (void)
{
  g_autoptr(GFile) root = g_file_new_for_path (g_get_tmp_dir ());
  g_autoptr(GFile) other = g_file_new_for_path ("/");
  g_autoptr(PhodavServer) server = phodav_server_new_for_root_file (root);
  g_autofree gchar *path = NULL;

  /* the root may change after construction, with either property */
  g_object_set (server, "root-file", other, NULL);
  g_assert_true (server->root_file == other);

  g_object_set (server, "root", g_get_tmp_dir (), NULL);
  g_object_get (server, "root", &path, NULL);
  g_assert_cmpstr (path, ==, g_get_tmp_dir ());
  g_clear_pointer (&path, g_free);

  /* a NULL root clears it */
  g_object_set (server, "root", NULL, NULL);
  g_assert_null (server->root_file);
  g_object_get (server, "root", &path, NULL);
  g_assert_null (path);

  /* the base path is canonicalized, NULL and empty mean the root */
  g_object_set (server, "base-path", NULL, NULL);
  g_assert_cmpstr (server->base_path, ==, "/");
  g_object_set (server, "base-path", "", NULL);
  g_assert_cmpstr (server->base_path, ==, "/");
  g_object_set (server, "base-path", "///dav//", NULL);
  g_assert_cmpstr (server->base_path, ==, "/dav");
  g_object_get (server, "base-path", &path, NULL);
  g_assert_cmpstr (path, ==, "/dav");
  g_clear_pointer (&path, g_free);

  /* setting the same base path does not re-register the handler */
  g_object_set (server, "base-path", "/dav", NULL);
  g_assert_cmpstr (server->base_path, ==, "/dav");

  /* an unsafe base path is rejected with a warning */
  g_test_expect_message ("phodav", G_LOG_LEVEL_WARNING, "*invalid base path*");
  g_object_set (server, "base-path", "/dav/../etc", NULL);
  g_test_assert_expected_messages ();
  g_assert_cmpstr (server->base_path, ==, "/dav");
}

static void
test_lock_store_edge_cases (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *token = new_token ();
  g_autofree gchar *missing = new_token ();
  g_autofree gchar *refreshed = NULL;
  g_autolist(LockSubmitted) submitted = NULL;

  /* an infinite lock is not queued on the expiry heap */
  g_assert_cmpint (phodav_lock_store_add_lock (server, "/forever", token,
                                               DAV_LOCK_SCOPE_EXCLUSIVE,
                                               DAV_LOCK_WRITE, DEPTH_ZERO,
                                               NULL, 0),
                   ==, SOUP_STATUS_OK);
  /* removing it does not touch the heap either */
  g_assert_cmpint (phodav_lock_store_remove_lock (server, "/forever", token),
                   ==, SOUP_STATUS_NO_CONTENT);

  /* refreshing an unknown token finds no lock */
  submitted = g_list_append (submitted, lock_submitted_new ("/x", missing));
  g_assert_cmpint (phodav_lock_store_refresh_lock (server, "/x", submitted,
                                                   10, &refreshed),
                   ==, SOUP_STATUS_BAD_REQUEST);
  g_assert_null (refreshed);

  /* a lock node is only built for a known token */
  g_assert_null (phodav_lock_store_lock_node (server, "/x", missing, NULL));

  /* freeing no paths at all is harmless */
  phodav_lock_store_paths_free (NULL);
}

static void
test_path_error_operations (void)
{
  g_autofree gchar *dir = make_test_tree ();
  g_autoptr(GFile) root = g_file_new_for_path (dir);
  g_autoptr(PhodavBackend) backend = phodav_backend_gfile_new (root);
  g_autoptr(GError) error = NULL;
  g_autoptr(PhodavPath) source = NULL;
  g_autoptr(PhodavPath) dest = NULL;
  gint status = 0;

  /* copy a file onto a directory */
  source = phodav_backend_gfile_resolve (backend, NULL, "hello.txt", PHODAV_PATH_EXISTING, &status);
  dest = phodav_backend_gfile_resolve (backend, NULL, "dir", PHODAV_PATH_EXISTING, &status);
  g_assert_nonnull (source);
  g_assert_nonnull (dest);
  g_assert_false (phodav_path_copy (source, dest, 0, TRUE, NULL, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

#if defined(G_OS_UNIX) && defined(HAVE_DIRFD_OPERATIONS)
  /* a symbolic link inside a copied directory is refused */
  {
    g_autofree gchar *src = g_build_filename (dir, "srclink", NULL);
    g_autofree gchar *link = g_build_filename (src, "link", NULL);
    g_autofree gchar *dst = g_build_filename (dir, "dstlink", NULL);

    g_assert_cmpint (g_mkdir (src, 0700), ==, 0);
    g_assert_cmpint (symlink ("missing", link), ==, 0);

    g_clear_pointer (&source, phodav_path_unref);
    g_clear_pointer (&dest, phodav_path_unref);
    source = phodav_backend_gfile_resolve (backend, NULL, "srclink", PHODAV_PATH_EXISTING, &status);
    dest = phodav_backend_gfile_resolve (backend, NULL, "dstlink", PHODAV_PATH_CREATE, &status);
    g_assert_nonnull (source);
    g_assert_nonnull (dest);
    g_assert_false (phodav_path_copy (source, dest, 0, TRUE, NULL, &error));
    g_assert_nonnull (error);
    g_clear_error (&error);
  }
#endif

  /* move a file onto an existing destination without overwrite */
  g_clear_pointer (&source, phodav_path_unref);
  g_clear_pointer (&dest, phodav_path_unref);
  source = phodav_backend_gfile_resolve (backend, NULL, "hello.txt", PHODAV_PATH_EXISTING, &status);
  dest = phodav_backend_gfile_resolve (backend, NULL, "dir", PHODAV_PATH_EXISTING, &status);
  g_assert_false (phodav_path_move (source, dest, 0, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);

  /* move to a destination with a missing parent */
  g_clear_pointer (&dest, phodav_path_unref);
  {
    g_autofree gchar *missing = g_build_filename (dir, "missing", "x", NULL);

    dest = gfile_path_new (backend, missing, "x");
  }
  g_assert_false (phodav_path_move (source, dest, G_FILE_COPY_OVERWRITE, NULL, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  remove_recursively (dir);
}

static void
test_xml_node_edge_cases (void)
{
  g_autoptr(xmlNode) plain = xmlNewNode (NULL, BAD_CAST "foo");
  xmlNsPtr empty_ns;

  /* a namespace with no href does not match */
  empty_ns = xmlNewNs (plain, NULL, BAD_CAST "E");
  xmlSetNs (plain, empty_ns);
  g_assert_false (xml_node_has_name_ns (plain, "foo", "DAV:"));
  g_assert_false (xml_node_has_name (plain, "foo"));

  g_test_expect_message ("phodav", G_LOG_LEVEL_CRITICAL, "*assertion 'node != NULL' failed*");
  g_assert_false (xml_node_has_name_ns (NULL, "foo", NULL));
  g_test_assert_expected_messages ();

  g_test_expect_message ("phodav", G_LOG_LEVEL_CRITICAL, "*assertion 'node != NULL' failed*");
  g_assert_false (xml_node_has_name (NULL, "foo"));
  g_test_assert_expected_messages ();
}

gint
main (gint argc, gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_set_nonfatal_assertions ();

  g_test_add_func ("/utils/remove-trailing", test_remove_trailing);
  g_test_add_func ("/utils/dot-dot-segment", test_path_has_dot_dot_segment);
  g_test_add_func ("/utils/path-is-safe", test_path_is_safe);
  g_test_add_func ("/utils/depth", test_depth);
  g_test_add_func ("/utils/path-is-covered", test_path_is_covered);
  g_test_add_func ("/utils/etag-header-matches", test_etag_header_matches);
  g_test_add_func ("/utils/http-time-to-string", test_http_time_to_string);
  g_test_add_func ("/utils/http-not-modified", test_http_request_not_modified);
  g_test_add_func ("/utils/http-range", test_http_range);
  g_test_add_func ("/utils/path-canonicalize", test_path_canonicalize);
  g_test_add_func ("/utils/xml-node-edge-cases", test_xml_node_edge_cases);
  g_test_add_func ("/utils/timeout", test_timeout);
  g_test_add_func ("/utils/xml-node-helpers", test_xml_node_helpers);
  g_test_add_func ("/utils/davdoc-parse", test_davdoc_parse);

  g_test_add_func ("/path/lifecycle", test_path_lifecycle);
  g_test_add_func ("/path/orphaned-locks", test_path_orphaned_locks);
  g_test_add_func ("/path/status-from-errno", test_status_from_errno);
  g_test_add_func ("/path/status-from-error", test_status_from_error);
  g_test_add_func ("/path/copy-and-delete", test_path_copy_and_delete);
  g_test_add_func ("/path/operations", test_path_operations);
  g_test_add_func ("/path/error-operations", test_path_error_operations);

  g_test_add_func ("/backend/selection", test_backend_selection);
  g_test_add_func ("/backend/gfile-resolve", test_gfile_backend_resolve);
  g_test_add_func ("/backend/gfile-operations", test_gfile_backend_operations);

  g_test_add_func ("/lock/new", test_dav_lock_new);
  g_test_add_func ("/lock/activelock-node", test_dav_lock_activelock_node);
  g_test_add_func ("/lock/submitted", test_lock_submitted);

  g_test_add_func ("/server/paths", test_server_paths);
  g_test_add_func ("/server/trie-paths", test_server_trie_paths);
  g_test_add_func ("/server/lock-manager", test_server_lock_manager);
  g_test_add_func ("/server/tree-other-locks", test_server_tree_other_locks);
  g_test_add_func ("/lock/expiry", test_dav_lock_expiry);
  g_test_add_func ("/server/lock-expiry", test_server_lock_expiry);
  g_test_add_func ("/server/expiry-index", test_server_expiry_index);
  g_test_add_func ("/server/lock-limit", test_server_lock_limit);
  g_test_add_func ("/server/lock-conflict", test_server_lock_conflict);
  g_test_add_func ("/server/dispose", test_server_dispose);
  g_test_add_func ("/server/stop", test_server_stop);
  g_test_add_func ("/server/properties", test_server_properties);
  g_test_add_func ("/server/relative-path", test_handler_relative_path);
  g_test_add_func ("/lock/store-edge-cases", test_lock_store_edge_cases);
  g_test_add_func ("/server/injected-soup-server", test_server_injected_soup_server);

  return g_test_run ();
}
