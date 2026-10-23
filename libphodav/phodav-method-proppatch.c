/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <errno.h>
#include <sys/types.h>

#include "phodav-lock.h"
#include "phodav-multistatus.h"
#include "phodav-priv.h"
#include "phodav-utils.h"
#include "phodav-virtual-dir.h"
#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

static xmlBufferPtr
node_children_to_string (xmlNodePtr node)
{
  xmlBufferPtr buf = xmlBufferCreate ();

  for (node = node->children; node; node = node->next)
    xmlNodeDump (buf, node->doc, node, 0, 0);

  return buf;
}

/* the file whose extended attributes back a virtual directory */
static GFile *
xattr_target (GFile *file)
{
  /* https://gitlab.gnome.org/GNOME/glib/issues/1187 */
  if (PHODAV_IS_VIRTUAL_DIR (file))
    return phodav_virtual_dir_root_get_real (PHODAV_VIRTUAL_DIR (file));

  return g_object_ref (file);
}

static gint
xattr_remove (const gchar *path, const gchar *name)
{
#ifdef HAVE_SYS_XATTR_H
#if defined(ENOATTR)
  const gint no_attr = ENOATTR;
#elif defined(ENODATA)
  const gint no_attr = ENODATA;
#else
  const gint no_attr = ENOENT;
#endif

  if (!path)
    return SOUP_STATUS_FORBIDDEN;

#ifdef __APPLE__
  if (removexattr (path, name, 0) == 0)
#else
  if (removexattr (path, name) == 0)
#endif
    return SOUP_STATUS_OK;

  /* removing a property which is not set is not an error: PROPPATCH
   * removals are idempotent, and litmus deletes the same property
   * twice */
  if (errno == no_attr)
    return SOUP_STATUS_OK;

  if (errno == EACCES || errno == EPERM)
    return SOUP_STATUS_FORBIDDEN;

  if (errno == ENOENT)
    return SOUP_STATUS_NOT_FOUND;

  return SOUP_STATUS_INTERNAL_SERVER_ERROR;
#else
  g_debug ("cannot remove xattr from %s, not supported", path); /* FIXME? */
  return SOUP_STATUS_OK;
#endif
}

static gint
set_attr (GFile *file, xmlNodePtr attrnode, GFileAttributeType type, gchar *mem, GCancellable *cancellable)
{
  g_autofree gchar *attrname = NULL;
  gint status = SOUP_STATUS_OK;
  g_autoptr(GError) error = NULL;

  if (type == G_FILE_ATTRIBUTE_TYPE_INVALID)
    {
      g_autoptr(GFile) target = NULL;
      g_autofree gchar *path = NULL;

      attrname = xml_node_get_xattr_name (attrnode, "user.");
      g_return_val_if_fail (attrname, SOUP_STATUS_BAD_REQUEST);

      target = xattr_target (file);
      if (!target)
        return SOUP_STATUS_FORBIDDEN;

      path = g_file_get_path (target);
      status = xattr_remove (path, attrname);
    }
  else
    {
      attrname = xml_node_get_xattr_name (attrnode, "xattr::");
      g_return_val_if_fail (attrname, SOUP_STATUS_BAD_REQUEST);

      g_file_set_attribute (file, attrname, type, mem,
                            G_FILE_QUERY_INFO_NONE, cancellable, &error);
    }

  if (error)
    {
      g_debug ("PROPPATCH: failed to set the property: %s", error->message);
      status = SOUP_STATUS_NOT_FOUND;
    }

  return status;
}

static gint
prop_set (GFile *file,
          xmlNodePtr parent,
          xmlNodePtr *attr,
          gboolean remove,
          GCancellable *cancellable)
{
  xmlNodePtr node, attrnode;
  gint type = G_FILE_ATTRIBUTE_TYPE_INVALID;
  gint status;

  for (node = parent->children; node; node = node->next)
    {
      if (!xml_node_is_element (node))
        continue;

      if (xml_node_has_name (node, "prop"))
        {
          xmlBufferPtr buf = NULL;

          attrnode = node->children;
          if (!xml_node_is_element (attrnode))
            continue;

          *attr = xmlCopyNode (attrnode, 2);

          if (!remove)
            {
              buf = node_children_to_string (attrnode);
              type = G_FILE_ATTRIBUTE_TYPE_STRING;
            }

          status = set_attr (file, attrnode, type, (gchar *) xmlBufferContent (buf), cancellable);

          if (buf)
            xmlBufferFree (buf);

          return status;
        }
    }

  return SOUP_STATUS_BAD_REQUEST;
}

gint
phodav_method_proppatch (PathHandler *handler, SoupServerMessage *msg, const gchar *path, GError **err)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  GFile *file = NULL;
  g_autoptr(GHashTable) mstatus = NULL; // path -> statlist
  g_auto(DavDoc) doc = {
    0,
  };
  xmlNodePtr node = NULL, attr = NULL;
  GList *props = NULL;
  g_autolist(LockSubmitted) submitted = NULL;
  gint status;

  if (!davdoc_parse (&doc, soup_server_message_get_request_body (msg), "propertyupdate"))
    {
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }

  status = phodav_check_if (handler, msg, path, &submitted);
  if (status != SOUP_STATUS_OK)
    goto end;

  file = g_file_get_child (handler_get_file (handler), path + 1);
  mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) response_free);

  node = doc.root;
  for (node = node->children; node; node = node->next)
    {
      if (!xml_node_is_element (node))
        continue;

      if (xml_node_has_name (node, "set"))
        status = prop_set (file, node, &attr, FALSE, cancellable);
      else if (xml_node_has_name (node, "remove"))
        status = prop_set (file, node, &attr, TRUE, cancellable);
      else
        {
          /* RFC 4918 section 12.13: a propertyupdate carries set and
           * remove instructions, the other elements are ignored */
          g_debug ("PROPPATCH: ignoring the <%s> element", (gchar *) node->name);
        }

      if (attr)
        {
          attr->_private = GINT_TO_POINTER (status);
          props = g_list_append (props, attr);
          /* the list owns the node now */
          attr = NULL;
        }
    }

  /* a body without any set or remove instruction (or with none in
   * the DAV namespace) names no property to report: it is a
   * malformed request (RFC 4918 section 9.9), the multistatus
   * response would be empty otherwise */
  if (!props)
    {
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }

  g_hash_table_insert (mstatus, g_strdup (path),
                       response_new (props, 0));

  if (g_hash_table_size (mstatus) > 0)
    status = set_response_multistatus (msg, mstatus);

end:
  g_clear_object (&file);

  return status;
}
