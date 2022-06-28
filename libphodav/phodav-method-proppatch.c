/*
 * Copyright (C) 2013 Red Hat, Inc.
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
#include "phodav-multistatus.h"
#include "phodav-utils.h"
#include "phodav-virtual-dir.h"

#include <sys/types.h>
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
set_attr (GFile *file, xmlNodePtr attrnode,
          GFileAttributeType type, gchar *mem, GCancellable *cancellable)
{
  gchar *attrname;
  gint status = SOUP_STATUS_OK;
  GError *error = NULL;

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
      gchar *path = g_file_get_path (file);
#ifdef HAVE_SYS_XATTR_H
#ifdef __APPLE__
      removexattr (path, attrname, 0);
#else
      removexattr (path, attrname);
#endif
#else
      g_debug ("cannot remove xattr from %s, not supported", path); /* FIXME? */
#endif
      g_free (path);
      g_object_unref (file);
    }
  else
    {
      attrname = xml_node_get_xattr_name (attrnode, "xattr::");
      g_return_val_if_fail (attrname, SOUP_STATUS_BAD_REQUEST);

      g_file_set_attribute (file, attrname, type, mem,
                            G_FILE_QUERY_INFO_NONE, cancellable, &error);
    }

  g_free (attrname);

  if (error)
    {
      g_warning ("failed to set property: %s", error->message);
      g_clear_error (&error);
      status = SOUP_STATUS_NOT_FOUND;
    }

  return status;
}

static gint
prop_set (SoupServerMessage *msg,
          GFile *file, xmlNodePtr parent, xmlNodePtr *attr,
          gboolean remove, GCancellable *cancellable)
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

          if (!remove)
            {
              *attr = xmlCopyNode (attrnode, 2);

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
phodav_method_proppatch (PathHandler *handler, SoupServerMessage *msg,
                         const char *path, GError **err)
{
  GCancellable *cancellable = handler_get_cancellable (handler);
  GFile *file = NULL;
  GHashTable *mstatus = NULL;   // path -> statlist
  DavDoc doc = {0, };
  xmlNodePtr node = NULL, attr = NULL;
  GList *props = NULL, *submitted = NULL;
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
        }
    }

  g_hash_table_insert (mstatus, g_strdup (path),
                       response_new (props, 0));

  if (g_hash_table_size (mstatus) > 0)
    status = set_response_multistatus (msg, mstatus);

end:
  davdoc_free (&doc);
  if (mstatus)
    g_hash_table_unref (mstatus);
  g_clear_object (&file);

  return status;
}
