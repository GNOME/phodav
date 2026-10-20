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
  g_assert_cmpint (depth_from_string (NULL), ==, DEPTH_INFINITY);
  g_assert_cmpint (depth_from_string ("0"), ==, DEPTH_ZERO);
  g_assert_cmpint (depth_from_string ("1"), ==, DEPTH_ONE);
  g_assert_cmpint (depth_from_string ("infinity"), ==, DEPTH_INFINITY);

  g_assert_cmpstr (depth_to_string (DEPTH_ZERO), ==, "0");
  g_assert_cmpstr (depth_to_string (DEPTH_ONE), ==, "1");
  g_assert_cmpstr (depth_to_string (DEPTH_INFINITY), ==, "infinity");

  /* invalid values are rejected with a warning and map to infinity */
  g_test_expect_message ("phodav", G_LOG_LEVEL_WARNING, "Invalid depth: 2");
  g_assert_cmpint (depth_from_string ("2"), ==, DEPTH_INFINITY);
  g_test_assert_expected_messages ();
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
  g_test_expect_message ("phodav", G_LOG_LEVEL_CRITICAL, "*strlen (token) == sizeof (lock->token)*");
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

gint
main (gint argc, gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_set_nonfatal_assertions ();

  g_test_add_func ("/utils/remove-trailing", test_remove_trailing);
  g_test_add_func ("/utils/dot-dot-segment", test_path_has_dot_dot_segment);
  g_test_add_func ("/utils/depth", test_depth);
  g_test_add_func ("/utils/timeout", test_timeout);
  g_test_add_func ("/utils/xml-node-helpers", test_xml_node_helpers);

  g_test_add_func ("/path/lifecycle", test_path_lifecycle);
  g_test_add_func ("/path/orphaned-locks", test_path_orphaned_locks);

  g_test_add_func ("/lock/new", test_dav_lock_new);
  g_test_add_func ("/lock/activelock-node", test_dav_lock_activelock_node);
  g_test_add_func ("/lock/submitted", test_lock_submitted);

  g_test_add_func ("/server/paths", test_server_paths);
  g_test_add_func ("/server/lock-manager", test_server_lock_manager);

  return g_test_run ();
}
