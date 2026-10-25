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
#include "phodav-xattr.h"

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
prop_set_value (GFile *file, xmlNodePtr attrnode, GCancellable *cancellable)
{
  g_autofree gchar *attrname = xml_node_get_xattr_name (attrnode, "xattr::");
  g_autoptr(GError) error = NULL;
  xmlBufferPtr buf;
  gboolean ok;

  g_return_val_if_fail (attrname, SOUP_STATUS_BAD_REQUEST);

  buf = node_children_to_string (attrnode);
  ok = g_file_set_attribute (file, attrname, G_FILE_ATTRIBUTE_TYPE_STRING,
                             (gpointer) xmlBufferContent (buf),
                             G_FILE_QUERY_INFO_NONE, cancellable, &error);
  xmlBufferFree (buf);

  if (!ok)
    {
      g_debug ("PROPPATCH: failed to set the property: %s",
               error ? error->message : "no error details");

      if (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return SOUP_STATUS_NOT_FOUND;
      if (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        return SOUP_STATUS_FORBIDDEN;

      return SOUP_STATUS_INTERNAL_SERVER_ERROR;
    }

  return SOUP_STATUS_OK;
}

static gint
prop_remove (GFile *file, xmlNodePtr attrnode)
{
  g_autofree gchar *attrname = xml_node_get_xattr_name (attrnode, "user.");
  g_autoptr(GFile) target = NULL;
  g_autofree gchar *path = NULL;

  g_return_val_if_fail (attrname, SOUP_STATUS_BAD_REQUEST);

  target = xattr_target (file);
  if (!target)
    return SOUP_STATUS_FORBIDDEN;

  path = g_file_get_path (target);
  return phodav_xattr_remove (path, attrname);
}

typedef enum
{
  PROP_OP_SET,
  PROP_OP_REMOVE,
} PropOp;

/* one property instruction of the request, and the state needed to
 * undo it if a later instruction fails */
typedef struct _PropChange
{
  xmlNodePtr request;  /* borrowed from the request document */
  xmlNodePtr response; /* owned copy for the multistatus */
  gchar *gio_name;     /* GIO extended attribute name, "xattr::..." */
  gchar *raw_name;     /* POSIX extended attribute name, "user...." */
  PropOp op;
  gint status;
  gboolean applied;
  gboolean had_old;
  gchar *old_value;
  gsize old_len;
} PropChange;

static void
prop_change_free (PropChange *change)
{
  if (!change)
    return;

  if (change->response)
    xmlFreeNode (change->response);

  g_free (change->gio_name);
  g_free (change->raw_name);
  g_free (change->old_value);
  g_free (change);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PropChange, prop_change_free)

static PropChange *
prop_change_new (xmlNodePtr request, PropOp op)
{
  g_autofree gchar *gio_name = xml_node_get_xattr_name (request, "xattr::");
  g_autofree gchar *raw_name = xml_node_get_xattr_name (request, "user.");
  PropChange *change;

  if (!gio_name || !raw_name)
    return NULL;

  change = g_new0 (PropChange, 1);
  change->request = request;
  /* the response echoes the property name, not its value */
  change->response = xmlCopyNode (request, 2);
  change->gio_name = g_steal_pointer (&gio_name);
  change->raw_name = g_steal_pointer (&raw_name);
  change->op = op;
  change->status = SOUP_STATUS_OK;

  if (!change->response)
    {
      prop_change_free (change);
      return NULL;
    }

  return change;
}

/* record the value of the property before it is changed, so that the
 * change can be undone. The raw value is preferred: GIO escapes the
 * non-ASCII bytes of the values it returns. */
static gint
prop_snapshot (GFile *target, const gchar *gio_name, const gchar *raw_name, gchar **value, gsize *len, gboolean *exists)
{
  *value = NULL;
  *len = 0;
  *exists = FALSE;

#ifdef HAVE_SYS_XATTR_H
  {
    g_autofree gchar *path = g_file_get_path (target);

    if (path)
      {
        gssize size = phodav_xattr_get (path, raw_name, NULL, 0);

        if (size >= 0)
          {
            g_autofree gchar *buf = g_malloc (size + 1);

            if (phodav_xattr_get (path, raw_name, buf, size) != size)
              {
                g_debug ("PROPPATCH: failed to read the previous value of %s",
                         raw_name);
                return SOUP_STATUS_INTERNAL_SERVER_ERROR;
              }

            buf[size] = '\0';
            *value = g_steal_pointer (&buf);
            *len = size;
            *exists = TRUE;
            return SOUP_STATUS_OK;
          }

        {
          gint errsv = errno;

          if (errsv == phodav_xattr_no_attr_errno () || errsv == ENOENT)
            return SOUP_STATUS_OK;

          g_debug ("PROPPATCH: failed to read the previous value of %s: %s",
                   raw_name, g_strerror (errsv));
          return errsv == EACCES || errsv == EPERM
                   ? SOUP_STATUS_FORBIDDEN
                   : SOUP_STATUS_INTERNAL_SERVER_ERROR;
        }
      }
  }
#endif

  /* platforms without the POSIX extended attribute API: fall back to
   * the value GIO reports, even though it is escaped */
  {
    g_autoptr(GError) error = NULL;
    g_autoptr(GFileInfo) info = g_file_query_info (target, gio_name,
                                                   G_FILE_QUERY_INFO_NONE,
                                                   NULL, &error);

    if (!info)
      {
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
          return SOUP_STATUS_OK;

        g_debug ("PROPPATCH: failed to read the previous value of %s: %s",
                 gio_name, error ? error->message : "no error details");
        return SOUP_STATUS_INTERNAL_SERVER_ERROR;
      }

    if (g_file_info_has_attribute (info, gio_name))
      {
        *value = g_strdup (g_file_info_get_attribute_string (info, gio_name));
        *len = strlen (*value);
        *exists = TRUE;
      }

    return SOUP_STATUS_OK;
  }
}

static gint
prop_change_apply (GFile *file, PropChange *change, GCancellable *cancellable)
{
  if (change->op == PROP_OP_SET)
    return prop_set_value (file, change->request, cancellable);

  return prop_remove (file, change->request);
}

static void
prop_change_rollback (GFile *file, PropChange *change, GCancellable *cancellable)
{
  g_autoptr(GFile) target = xattr_target (file);
  g_autoptr(GError) error = NULL;

  if (!target)
    return;

  if (change->had_old)
    {
      g_autofree gchar *value = g_strndup (change->old_value, change->old_len);

      if (!g_file_set_attribute (target, change->gio_name,
                                 G_FILE_ATTRIBUTE_TYPE_STRING, value,
                                 G_FILE_QUERY_INFO_NONE, cancellable, &error))
        g_debug ("PROPPATCH: failed to restore %s: %s", change->raw_name,
                 error->message);
    }
  else
    {
      g_autofree gchar *path = g_file_get_path (target);

      if (phodav_xattr_remove (path, change->raw_name) != SOUP_STATUS_OK)
        g_debug ("PROPPATCH: failed to remove %s", change->raw_name);
    }
}

/* apply the changes in the order of the request; on the first failure
 * undo the changes already applied, and report the other properties as
 * failed dependencies (RFC 4918 section 9.9) */
static void
prop_changes_apply (GFile *file, GList *changes, GCancellable *cancellable)
{
  GList *l;
  gboolean failed = FALSE;

  for (l = changes; l; l = l->next)
    {
      PropChange *change = l->data;
      g_autoptr(GFile) target = xattr_target (file);
      gint status;

      if (!target)
        {
          change->status = SOUP_STATUS_FORBIDDEN;
          failed = TRUE;
          break;
        }

      status = prop_snapshot (target, change->gio_name, change->raw_name,
                              &change->old_value, &change->old_len,
                              &change->had_old);
      if (status != SOUP_STATUS_OK)
        {
          change->status = status;
          failed = TRUE;
          break;
        }

      status = prop_change_apply (file, change, cancellable);
      change->status = status;
      if (status != SOUP_STATUS_OK)
        {
          failed = TRUE;
          break;
        }

      change->applied = TRUE;
    }

  if (!failed)
    return;

  for (l = changes; l; l = l->next)
    {
      PropChange *change = l->data;

      if (change->status == SOUP_STATUS_OK)
        change->status = SOUP_STATUS_FAILED_DEPENDENCY;
    }

  /* the changes are independent, undo them newest first */
  for (l = g_list_last (changes); l; l = l->prev)
    {
      PropChange *change = l->data;

      if (change->applied)
        prop_change_rollback (file, change, cancellable);
    }
}

/* collect the properties of one set or remove instruction, in the
 * order of the request */
static gboolean
prop_collect (GList **changes, xmlNodePtr instruction, PropOp op)
{
  xmlNodePtr node, prop = NULL;
  gboolean found = FALSE;

  for (node = instruction->children; node; node = node->next)
    {
      if (xml_node_is_element (node) && xml_node_has_name (node, "prop"))
        {
          prop = node;
          break;
        }
    }

  if (!prop)
    return FALSE;

  for (node = prop->children; node; node = node->next)
    {
      PropChange *change;

      if (!xml_node_is_element (node))
        continue;

      change = prop_change_new (node, op);
      if (!change)
        return FALSE;

      *changes = g_list_append (*changes, change);
      found = TRUE;
    }

  /* an empty prop names no property to report */
  return found;
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
  xmlNodePtr node;
  GList *props = NULL;
  g_autolist(PropChange) changes = NULL;
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

  file = handler_get_file_for_path (handler, path);
  mstatus = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) response_free);

  for (node = doc.root->children; node; node = node->next)
    {
      PropOp op;

      if (!xml_node_is_element (node))
        continue;

      if (xml_node_has_name (node, "set"))
        op = PROP_OP_SET;
      else if (xml_node_has_name (node, "remove"))
        op = PROP_OP_REMOVE;
      else
        {
          /* RFC 4918 section 12.13: a propertyupdate carries set and
           * remove instructions, the other elements are ignored */
          g_debug ("PROPPATCH: ignoring the <%s> element", (gchar *) node->name);
          continue;
        }

      if (!prop_collect (&changes, node, op))
        {
          status = SOUP_STATUS_BAD_REQUEST;
          goto end;
        }
    }

  /* a body without any set or remove instruction (or with none in
   * the DAV namespace) names no property to report: it is a
   * malformed request (RFC 4918 section 9.9), the multistatus
   * response would be empty otherwise */
  if (!changes)
    {
      status = SOUP_STATUS_BAD_REQUEST;
      goto end;
    }

  prop_changes_apply (file, changes, cancellable);

  for (GList *l = changes; l; l = l->next)
    {
      PropChange *change = l->data;

      props = g_list_append (props,
                             response_prop_new (g_steal_pointer (&change->response),
                                                change->status));
    }

  g_hash_table_insert (mstatus, g_strdup (path),
                       response_new (props, 0));

  status = set_response_multistatus (msg, mstatus);

end:
  g_clear_object (&file);

  return status;
}
