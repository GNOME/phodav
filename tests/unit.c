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
#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>

#include "phodav-lock.h"
#include "phodav-path.h"
#include "phodav-priv.h"
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
test_timeout (void)
{
  g_assert_cmpuint (timeout_from_string (NULL), ==, 0);
  g_assert_cmpuint (timeout_from_string ("Infinite"), ==, 0);
  g_assert_cmpuint (timeout_from_string ("Second-3600"), ==, 3600);
  g_assert_cmpuint (timeout_from_string ("second-42"), ==, 42);
  g_assert_cmpuint (timeout_from_string ("Second-"), ==, 0);

  /* invalid values are rejected with a critical */
  g_test_expect_message ("phodav", G_LOG_LEVEL_CRITICAL, "*should not be reached*");
  g_assert_cmpuint (timeout_from_string ("later"), ==, 0);
  g_test_assert_expected_messages ();
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

  /* the hash table reference goes away, the lock keeps the path alive */
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

  /* refreshing with 0 makes the lock infinite */
  dav_lock_refresh_timeout (lock, 0);
  g_assert_cmpuint (lock->timeout, ==, 0);
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
  g_autoptr(Path) p = path_new_test ("/a/b");
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

  content = node_content (find_child (find_child (node, "lockroot"), "href"));
  g_assert_cmpstr (content, ==, "/a/b");
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

static gboolean
collect_path (const gchar *key, Path *path, gpointer data)
{
  GList **collected = data;

  *collected = g_list_append (*collected, g_strdup (key));

  return TRUE;
}

static gboolean
stop_at_first (const gchar *key, Path *path, gpointer data)
{
  GList **collected = data;

  *collected = g_list_append (*collected, g_strdup (key));

  return FALSE;
}

static void
test_server_paths (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  Path *a, *a_again;

  a = server_get_path (server, "/a");
  g_assert_cmpstr (a->path, ==, "/a");
  g_assert_cmpuint (a->refs, ==, 1);

  /* trailing slashes are ignored, the same path is returned */
  a_again = server_get_path (server, "/a/");
  g_assert_true (a_again == a);
  g_assert_cmpuint (a->refs, ==, 1);

  server_get_path (server, "/a/b");

  {
    GList *collected = NULL;
    GList *l;

    /* only the known parent paths are visited, in hierarchical order */
    g_assert_true (server_foreach_parent_path (server, "/a/b/c", collect_path, &collected));
    g_assert_cmpuint (g_list_length (collected), ==, 2);
    l = collected;
    g_assert_cmpstr (l->data, ==, "/a");
    g_assert_cmpstr (l->next->data, ==, "/a/b");

    /* unknown paths are simply not visited */
    g_assert_true (server_foreach_parent_path (server, "/x/y", collect_path, &collected));
    g_assert_cmpuint (g_list_length (collected), ==, 2);

    g_list_free_full (collected, g_free);
  }

  {
    GList *collected = NULL;

    /* a callback returning FALSE stops the iteration */
    g_assert_false (server_foreach_parent_path (server, "/a/b", stop_at_first, &collected));
    g_assert_cmpuint (g_list_length (collected), ==, 1);
    g_assert_cmpstr (collected->data, ==, "/a");

    g_list_free_full (collected, g_free);
  }
}

static void
test_server_lock_manager (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *token = new_token ();
  g_autofree gchar *other = new_token ();
  g_autoptr(DAVLock) lock = NULL;
  g_autolist(LockSubmitted) submitted = NULL;
  Path *p;

  p = server_get_path (server, "/a");
  lock = dav_lock_new (p, token, DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                       DEPTH_ZERO, NULL, 60);
  path_add_lock (p, lock);

  /* locks on parent paths apply to their children */
  g_assert_true (server_path_get_lock (server, "/a", token) == lock);
  g_assert_true (server_path_get_lock (server, "/a/b/c", token) == lock);
  g_assert_null (server_path_get_lock (server, "/b", token));

  /* a resource is locked when a lock is not submitted */
  g_assert_true (server_path_has_other_locks (server, "/a/b", NULL));
  g_assert_true (server_path_has_other_locks (server, "/a/b/c", NULL));
  g_assert_false (server_path_has_other_locks (server, "/x/y", NULL));

  /* submitting the lock makes the resource accessible */
  submitted = g_list_append (NULL, lock_submitted_new ("/a", token));
  g_assert_false (server_path_has_other_locks (server, "/a/b", submitted));
  g_assert_false (server_path_has_other_locks (server, "/a/b/c", submitted));

  /* submitting another token does not help */
  lock_submitted_free (submitted->data);
  submitted->data = lock_submitted_new ("/a", other);
  g_assert_true (server_path_has_other_locks (server, "/a/b", submitted));

  /* the lock is released with the path */
  path_remove_lock (p, lock);
  g_assert_null (server_path_get_lock (server, "/a/b/c", token));
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
  DAVLock *lock;
  Path *p;

  p = server_get_path (server, "/a");
  lock = dav_lock_new (p, token, DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                       DEPTH_ZERO, NULL, 60);
  path_add_lock (p, lock);

  /* the lock is found while it has not expired */
  g_assert_true (server_path_lookup_lock (server, "/a/b", token) == lock);

  /* expire it behind the server's back */
  lock->timeout = g_get_monotonic_time () / G_USEC_PER_SEC - 1;

  /* the expired lock is not honored anymore */
  g_assert_null (server_path_lookup_lock (server, "/a/b", token));

  /* and the sweep reclaims the empty paths from the table */
  server_expire_locks (server);
  {
    GList *collected = NULL;

    g_assert_true (server_foreach_parent_path (server, "/a/b", collect_path, &collected));
    g_assert_cmpuint (g_list_length (collected), ==, 0);
    g_list_free_full (collected, g_free);
  }
}

static void
test_server_lock_conflict (void)
{
  g_autoptr(PhodavServer) server = phodav_server_new (NULL);
  g_autofree gchar *member_token = new_token ();
  g_autofree gchar *tree_token = new_token ();
  g_autoptr(DAVLock) member_lock = NULL;
  g_autoptr(DAVLock) tree_lock = NULL;
  Path *p;

  /* a lock on a member of the collection */
  p = server_get_path (server, "/coll/member");
  member_lock = dav_lock_new (p, member_token, DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                              DEPTH_ZERO, NULL, 60);
  path_add_lock (p, member_lock);

  /* an exclusive depth-infinity lock on the collection conflicts with
   * it (RFC 4918 section 7.4)... */
  tree_lock = dav_lock_new (server_get_path (server, "/coll"), tree_token,
                            DAV_LOCK_SCOPE_EXCLUSIVE, DAV_LOCK_WRITE,
                            DEPTH_INFINITY, NULL, 60);
  g_assert_true (server_tree_has_lock_conflict (server, "/coll", tree_lock));

  /* ...but a shared tree lock does not conflict with a shared member lock */
  g_clear_pointer (&member_lock, dav_lock_free);
  g_clear_pointer (&tree_lock, dav_lock_free);
  member_lock = dav_lock_new (server_get_path (server, "/coll/member"), member_token,
                              DAV_LOCK_SCOPE_SHARED, DAV_LOCK_WRITE,
                              DEPTH_ZERO, NULL, 60);
  path_add_lock (server_get_path (server, "/coll/member"), member_lock);
  tree_lock = dav_lock_new (server_get_path (server, "/coll"), tree_token,
                            DAV_LOCK_SCOPE_SHARED, DAV_LOCK_WRITE,
                            DEPTH_INFINITY, NULL, 60);
  g_assert_false (server_tree_has_lock_conflict (server, "/coll", tree_lock));

  /* a depth-infinity lock elsewhere does not conflict either */
  g_assert_false (server_tree_has_lock_conflict (server, "/other", tree_lock));
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

gint
main (gint argc, gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_set_nonfatal_assertions ();

  g_test_add_func ("/utils/remove-trailing", test_remove_trailing);
  g_test_add_func ("/utils/dot-dot-segment", test_path_has_dot_dot_segment);
  g_test_add_func ("/utils/depth", test_depth);
  g_test_add_func ("/utils/path-is-covered", test_path_is_covered);
  g_test_add_func ("/utils/etag-header-matches", test_etag_header_matches);
  g_test_add_func ("/utils/timeout", test_timeout);
  g_test_add_func ("/utils/xml-node-helpers", test_xml_node_helpers);

  g_test_add_func ("/path/lifecycle", test_path_lifecycle);
  g_test_add_func ("/path/orphaned-locks", test_path_orphaned_locks);

  g_test_add_func ("/lock/new", test_dav_lock_new);
  g_test_add_func ("/lock/activelock-node", test_dav_lock_activelock_node);
  g_test_add_func ("/lock/submitted", test_lock_submitted);

  g_test_add_func ("/server/paths", test_server_paths);
  g_test_add_func ("/server/lock-manager", test_server_lock_manager);
  g_test_add_func ("/lock/expiry", test_dav_lock_expiry);
  g_test_add_func ("/server/lock-expiry", test_server_lock_expiry);
  g_test_add_func ("/server/lock-conflict", test_server_lock_conflict);
  g_test_add_func ("/server/dispose", test_server_dispose);
  g_test_add_func ("/server/injected-soup-server", test_server_injected_soup_server);

  return g_test_run ();
}
