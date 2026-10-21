/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

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

static gint
set_attr (GFile *file, xmlNodePtr attrnode, GFileAttributeType type, gchar *mem, GCancellable *cancellable)
{
  g_autofree gchar *attrname = NULL;
  gint status = SOUP_STATUS_OK;
  g_autoptr(GError) error = NULL;

  if (type == G_FILE_ATTRIBUTE_TYPE_INVALID)
    {
      attrname = xml_node_get_xattr_name (attrnode, "user.");
      g_return_val_if_fail (attrname, SOUP_STATUS_BAD_REQUEST);

      /* https://gitlab.gnome.org/GNOME/glib/issues/1187 */
      if (PHODAV_IS_VIRTUAL_DIR (file))
        file = phodav_virtual_dir_root_get_real (PHODAV_VIRTUAL_DIR (file));
      else
        g_object_ref (file);
      if (!file)
        return SOUP_STATUS_FORBIDDEN;
      g_autofree gchar *path = g_file_get_path (file);
#ifdef HAVE_SYS_XATTR_H
#ifdef __APPLE__
      removexattr (path, attrname, 0);
#else
      removexattr (path, attrname);
#endif
#else
      g_debug ("cannot remove xattr from %s, not supported", path); /* FIXME? */
#endif
      g_object_unref (file);
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
      g_warning ("failed to set property: %s", error->message);
      status = SOUP_STATUS_NOT_FOUND;
    }

  return status;
}

static gint
prop_set (SoupServerMessage *msg,
          GFile *file,
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

  g_return_val_if_reached (SOUP_STATUS_BAD_REQUEST);
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

  if (!davdoc_parse (&doc, msg, soup_server_message_get_request_body (msg), "propertyupdate"))
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
        status = prop_set (msg, file, node, &attr, FALSE, cancellable);
      else if (xml_node_has_name (node, "remove"))
        status = prop_set (msg, file, node, &attr, TRUE, cancellable);
      else
        g_warn_if_reached ();

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
