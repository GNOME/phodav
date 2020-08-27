/*
 * Copyright (C) 2020 Red Hat, Inc.
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

#include <gio/gio.h>

#include "phodav-virtual-dir.h"

/**
 * SECTION:phodav-virtual-dir
 * @title: PhodavVirtualDir
 * @short_description: in-memory directory
 * @see_also: #GFileIface
 * @include: libphodav/phodav.h
 *
 * PhodavVirtualDir implements #GFileIface and can be used with all #GFile functions.
 *
 * PhodavVirtualDir, as the name suggests, does not represent any real file.
 * Instead, it is a virtual element that can be used to build a directory tree structure
 * in the memory. However, a PhodavVirtualDir can have a real file as its child.
 *
 * The first building block of such tree must be phodav_virtual_dir_new_root().
 * Further directories can be added using phodav_virtual_dir_new_dir().
 * To link a real file as a child of a PhodavVirtualDir, use phodav_virtual_dir_attach_real_child().
 *
 * PhodavVirtualDir allows you to easily share two resources when they have no common ancestor
 * (like "C:\fileA" and "D:\fileB") or when sharing their common ancestor would be impractical
 * (sharing the whole "/" when the files are "/d1/d2/fileA" and "/d3/d4/fileB").
 *
 * PhodavVirtualDir was designed primarily for the purposes of the
 * [SPICE project](https://www.spice-space.org/) and hence the functionality is very narrow.
 * If other projects find it handy as well, it could be extended quite easily.
 *
 * Supported methods:
 * - GET
 * - PROPFIND
 * - LOCK
 * - UNLOCK
 *
 * This concerns only the virtual directories. If you have a real #GFile
 * linked to a virtual directory, all the other methods are supported on such file.
 *
 * You currently cannot delete a #PhodavVirtualDir. Once the last reference to the root
 * is dropped, the whole structure is destroyed. Children that have other references (non-internal)
 * become dummies, otherwise they're freed.
 *
 * #PhodavVirtualDir is available since phodav 2.5.
 */

struct _PhodavVirtualDir {
  GObject           parent_instance;

  gboolean          dummy;
  PhodavVirtualDir *parent;
  GList            *children;
  GFile            *real_root; /* only set for virtual root, otherwise NULL */

  /* TODO: we could store just the base name and build up the path when needed */
  gchar            *path;
};

static void phodav_virtual_dir_file_interface_init (GFileIface *iface);

G_DEFINE_TYPE_WITH_CODE (PhodavVirtualDir, phodav_virtual_dir, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
                                                phodav_virtual_dir_file_interface_init))

struct _PhodavVirtualDirEnumerator {
  GFileEnumerator      parent_instance;
  gchar               *attributes;
  GFileQueryInfoFlags  flags;
  GList               *children;
  GList               *current;
  GFileEnumerator     *real_root_enumerator;
};

G_DEFINE_TYPE (PhodavVirtualDirEnumerator, phodav_virtual_dir_enumerator, G_TYPE_FILE_ENUMERATOR)

static GFileInfo *
phodav_virtual_dir_enumerator_next_file (GFileEnumerator *enumerator,
                                         GCancellable    *cancellable,
                                         GError         **error)
{
  PhodavVirtualDirEnumerator *self = PHODAV_VIRTUAL_DIR_ENUMERATOR (enumerator);
  GFile *file;

  if (!self->current || !self->current->data)
    {
      if (self->real_root_enumerator)
        return g_file_enumerator_next_file (self->real_root_enumerator, cancellable, error);
      return NULL;
    }

  file = G_FILE (self->current->data);
  self->current = self->current->next;
  return g_file_query_info (G_FILE (file), self->attributes, self->flags, cancellable, error);
}

static gboolean
phodav_virtual_dir_enumerator_close (GFileEnumerator *enumerator,
                                     GCancellable    *cancellable,
                                     GError         **error)
{
  PhodavVirtualDirEnumerator *self = PHODAV_VIRTUAL_DIR_ENUMERATOR (enumerator);
  g_clear_pointer (&self->attributes, g_free);
  g_list_free_full (self->children, g_object_unref);
  self->children = NULL;
  g_clear_object (&self->real_root_enumerator);
  return TRUE;
}

static void
phodav_virtual_dir_enumerator_class_init (PhodavVirtualDirEnumeratorClass *klass)
{
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  enumerator_class->next_file = phodav_virtual_dir_enumerator_next_file;
  enumerator_class->close_fn  = phodav_virtual_dir_enumerator_close;
}

static void
phodav_virtual_dir_enumerator_init (PhodavVirtualDirEnumerator *self)
{
}

// --------------------------------------------------------

static gboolean
is_root (PhodavVirtualDir *file)
{
  return !g_strcmp0 (file->path, "/");
}

/* create a new instance of PhodavVirtualDir
 * that is not attached to any parent */
static GFile *
virtual_dir_dummy_new (void)
{
  PhodavVirtualDir *dummy;
  dummy = g_object_new (PHODAV_TYPE_VIRTUAL_DIR, NULL);
  dummy->dummy = TRUE;
  return G_FILE (dummy);
}

static gchar *
phodav_virtual_dir_get_basename (GFile *file)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (file);
  return g_path_get_basename (self->path);
}

static gchar *
phodav_virtual_dir_get_path (GFile *file)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (file);
  if (self->real_root)
    return g_file_get_path (self->real_root);
  return g_strdup (self->path);
}

static GFileInfo *
phodav_virtual_dir_query_info (GFile                *file,
                               const char           *attributes,
                               GFileQueryInfoFlags   flags,
                               GCancellable         *cancellable,
                               GError              **error)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (file);
  GFileInfo *info;
  gchar *base;

  if (self->dummy)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "file has no parent");
      return NULL;
    }

  if (self->real_root)
    return g_file_query_info (self->real_root, attributes, flags, cancellable, error);

  info = g_file_info_new ();
  base = phodav_virtual_dir_get_basename (file);
  g_file_info_set_name (info, base);
  g_file_info_set_display_name (info, base);
  g_free (base);
  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
  /* TODO: set more attributes? e.g. etag, time*, access* */

  return info;
}

static GFileInfo *
phodav_virtual_dir_query_filesystem_info (GFile         *file,
                                          const char    *attributes,
                                          GCancellable  *cancellable,
                                          GError       **error)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (file);
  GFileInfo *info;

  if (self->dummy)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "file has no parent");
      return NULL;
    }

  if (self->real_root)
    return g_file_query_filesystem_info (self->real_root, attributes, cancellable, error);

  info = g_file_info_new ();
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, 0);
  return info;
}

static gboolean
phodav_virtual_dir_measure_disk_usage (GFile                         *file,
                                       GFileMeasureFlags              flags,
                                       GCancellable                  *cancellable,
                                       GFileMeasureProgressCallback   progress_callback,
                                       gpointer                       progress_data,
                                       guint64                       *disk_usage,
                                       guint64                       *num_dirs,
                                       guint64                       *num_files,
                                       GError                       **error)
{
  /* prop_quota_used in phodav-method-propfind.c is only interested in @disk_usage */
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (file);
  if (self->real_root)
    return g_file_measure_disk_usage (self->real_root, flags, cancellable, progress_callback,
                                      progress_data, disk_usage, num_dirs, num_files, error);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Operation not supported");
  return FALSE;
}

static GFile *
phodav_virtual_dir_get_parent (GFile *file)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (file);

  if (is_root (self))
    return NULL;

  if (self->parent)
    return G_FILE (g_object_ref (self->parent));

  return virtual_dir_dummy_new ();
}

static GFileEnumerator *
phodav_virtual_dir_enumerate_children (GFile                *file,
                                       const char           *attributes,
                                       GFileQueryInfoFlags   flags,
                                       GCancellable         *cancellable,
                                       GError              **error)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (file);
  PhodavVirtualDirEnumerator *enumerator;

  enumerator = g_object_new (PHODAV_TYPE_VIRTUAL_DIR_ENUMERATOR, "container", file, NULL);
  enumerator->attributes = g_strdup (attributes);
  enumerator->flags = flags;
  enumerator->children = g_list_copy_deep (self->children, (GCopyFunc) g_object_ref, NULL);
  enumerator->current = enumerator->children;
  if (self->real_root)
    {
      enumerator->real_root_enumerator =
        g_file_enumerate_children(self->real_root,
                                  attributes,
                                  flags,
                                  cancellable,
                                  error);
    }
  return G_FILE_ENUMERATOR (enumerator);
}

/* searches for an immediate descendant of @file with the given @name,
 * the reference count of the returned child is not incremented */
static GFile *
phodav_virtual_dir_find_direct_child (PhodavVirtualDir *parent,
                                      const gchar      *name)
{
  GFile *child;
  GList *l;
  gchar *base;

  /* TODO: perhaps a hash table would be better? */
  for (l = parent->children; l; l = l->next)
    {
      child = G_FILE (l->data);
      base = g_file_get_basename (child);
      if (!g_strcmp0 (name, base))
        {
          g_free (base);
          return child;
        }
      g_free (base);
    }

  return NULL;
}

/* recursively searches for a child of @parent given the relative @path.
 * The result can be either:
 *     1) PhodavVirtualDir
 *     2) GFile attached to PhodavVirtualDir
 *     3) GFile (child of attached GFile)
 *     4) NULL (when not found)
 * If returned value is non-NULL, call g_object_unref(). */
static GFile *
phodav_virtual_dir_find_child_recursive (PhodavVirtualDir *parent,
                                         const gchar      *path,
                                         gboolean         *common_segment)
{
  GFile *current;
  gchar **segments, **segment_ptr, *real;

  g_return_val_if_fail (parent != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);
  g_return_val_if_fail (path[0] != '\0', NULL);

  if (common_segment)
    *common_segment = FALSE;

  segments = g_strsplit (path, "/", -1);

  current = G_FILE (parent);
  for (segment_ptr = segments; *segment_ptr; segment_ptr++)
    {
      /* empty string means there were leading/trailing/consecutive slashes, skip them  */
      if (*segment_ptr[0] == '\0')
        continue;

      /* we arrived at a real file */
      if (!PHODAV_IS_VIRTUAL_DIR (current))
        {
          /* build up a new path from the remaining segments
           * and let GLib handle the rest */
          real = g_build_pathv ("/", segment_ptr);
          current = g_file_get_child (current, real);
          g_free (real);
          g_strfreev (segments);
          return current;
        }
      current = phodav_virtual_dir_find_direct_child (PHODAV_VIRTUAL_DIR (current), *segment_ptr);
      if (current)
        {
          if (common_segment)
            *common_segment = TRUE;
        }
      else
        {
          break;
        }
    }

  g_strfreev (segments);
  if (current)
    g_object_ref (current);
  return current;
}

static GFile *
phodav_virtual_dir_resolve_relative_path (GFile       *file,
                                          const gchar *relative_path)
{
  PhodavVirtualDir *parent;
  GFile *child;
  gboolean common_segment;

  if (relative_path[0] == '\0')
    return g_object_ref (file);

  /* try to find the file inside virtual dirs first */
  parent = PHODAV_VIRTUAL_DIR (file);
  child = phodav_virtual_dir_find_child_recursive (parent, relative_path, &common_segment);
  if (child)
    return child;
  if (common_segment)
    return virtual_dir_dummy_new ();
  if (parent->real_root)
    return g_file_resolve_relative_path (parent->real_root, relative_path);

  return virtual_dir_dummy_new ();
}

static GFile *
phodav_virtual_dir_dup (GFile *file)
{
  return NULL;
}

static guint
phodav_virtual_dir_hash (GFile *file)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (file);
  return g_str_hash (self->path);
}

static gboolean
phodav_virtual_dir_equal (GFile *file1,
                          GFile *file2)
{
  return file1 == file2;
}

static gboolean
phodav_virtual_dir_is_native (GFile *file)
{
  return FALSE;
}

static gboolean
phodav_virtual_dir_has_uri_scheme (GFile       *file,
                                   const gchar *uri_scheme)
{
  return FALSE;
}

static gchar *
phodav_virtual_dir_get_uri_scheme (GFile *file)
{
  return NULL;
}

static gchar *
phodav_virtual_dir_get_uri (GFile *file)
{
  return NULL;
}

static gchar *
phodav_virtual_dir_get_parse_name (GFile *file)
{
  return NULL;
}

static gboolean
phodav_virtual_dir_prefix_matches (GFile *prefix,
                                   GFile *file)
{
  return FALSE; /* TODO */
}

static gchar *
phodav_virtual_dir_get_relative_path (GFile *parent,
                                      GFile *descendant)
{
  return NULL; /* TODO */
}

static GFile *
phodav_virtual_dir_get_child_for_display_name (GFile        *file,
                                               const gchar  *display_name,
                                               GError      **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Operation not supported");
  return NULL;
}

static GFile *
phodav_virtual_dir_set_display_name (GFile         *file,
                                     const gchar   *display_name,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Operation not supported");
  return NULL;
}

static gboolean
phodav_virtual_dir_set_attribute (GFile                *file,
                                  const char           *attribute,
                                  GFileAttributeType    type,
                                  gpointer              value_p,
                                  GFileQueryInfoFlags   flags,
                                  GCancellable         *cancellable,
                                  GError              **error)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (file);
  if (self->real_root)
    return g_file_set_attribute (self->real_root, attribute, type,
                                 value_p, flags, cancellable, error);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Operation not supported");
  return FALSE;
}

static gboolean
phodav_virtual_dir_set_attributes_from_info (GFile                *file,
                                             GFileInfo            *info,
                                             GFileQueryInfoFlags   flags,
                                             GCancellable         *cancellable,
                                             GError              **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Operation not supported");
  return FALSE;
}

static void
phodav_virtual_dir_file_interface_init (GFileIface *iface)
{
  iface->get_basename = phodav_virtual_dir_get_basename;
  iface->get_path = phodav_virtual_dir_get_path;
  iface->query_info = phodav_virtual_dir_query_info;
  iface->query_filesystem_info = phodav_virtual_dir_query_filesystem_info;
  iface->measure_disk_usage = phodav_virtual_dir_measure_disk_usage;

  iface->get_parent = phodav_virtual_dir_get_parent;
  iface->enumerate_children = phodav_virtual_dir_enumerate_children;
  iface->resolve_relative_path = phodav_virtual_dir_resolve_relative_path;

  /* GLib does not check if these functions are implemented or not, it simply calls them,
   * so we need to provide these stubs to avoid crashes */
  iface->dup = phodav_virtual_dir_dup;
  iface->hash = phodav_virtual_dir_hash;
  iface->equal = phodav_virtual_dir_equal;
  iface->is_native = phodav_virtual_dir_is_native;
  iface->has_uri_scheme = phodav_virtual_dir_has_uri_scheme;
  iface->get_uri_scheme = phodav_virtual_dir_get_uri_scheme;
  iface->get_uri = phodav_virtual_dir_get_uri;
  iface->get_parse_name = phodav_virtual_dir_get_parse_name;
  iface->prefix_matches = phodav_virtual_dir_prefix_matches;
  iface->get_relative_path = phodav_virtual_dir_get_relative_path;
  iface->get_child_for_display_name = phodav_virtual_dir_get_child_for_display_name;
  iface->set_display_name = phodav_virtual_dir_set_display_name;
  iface->set_attribute = phodav_virtual_dir_set_attribute;
  iface->set_attributes_from_info = phodav_virtual_dir_set_attributes_from_info;
}

static void
parent_gone_cb (gpointer  data,
                GObject  *where_the_object_was)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (data);
  self->dummy = TRUE;
  self->parent = NULL;
}

static void
phodav_virtual_dir_dispose (GObject *object)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (object);

  /* TODO: would be nice to have a test for this */
  if (self->parent)
    {
      g_object_weak_unref (G_OBJECT (self->parent), parent_gone_cb, self);
      self->parent = NULL;
    }
  self->dummy = TRUE;
  g_list_free_full (self->children, g_object_unref);
  self->children = NULL;

  G_OBJECT_CLASS (phodav_virtual_dir_parent_class)->dispose (object);
}

static void
phodav_virtual_dir_finalize (GObject *object)
{
  PhodavVirtualDir *self = PHODAV_VIRTUAL_DIR (object);
  g_free (self->path);
  G_OBJECT_CLASS (phodav_virtual_dir_parent_class)->finalize (object);
}

static void
phodav_virtual_dir_class_init (PhodavVirtualDirClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = phodav_virtual_dir_dispose;
  gobject_class->finalize = phodav_virtual_dir_finalize;
}

static void
phodav_virtual_dir_init (PhodavVirtualDir *self)
{
}

/**
 * phodav_virtual_dir_new_root:
 *
 * Creates a new root virtual directory that acts as the ancestor
 * of all further virtual directories.
 *
 * Since: 2.5
 * Returns: (transfer full): a new #PhodavVirtualDir with the path `/`
 **/
PhodavVirtualDir *
phodav_virtual_dir_new_root (void)
{
  PhodavVirtualDir *root;
  root = g_object_new (PHODAV_TYPE_VIRTUAL_DIR, NULL);
  root->path = g_strdup ("/");
  root->dummy = FALSE;
  return root;
}

/**
 * phodav_virtual_dir_root_set_real:
 * @root: #PhodavVirtualDir obtained from phodav_virtual_dir_new_root()
 * @real_root_path: (nullable): path to a real directory
 *
 * If @real_root_path is not %NULL, @root lists all files added with
 * phodav_virtual_dir_new_dir() and phodav_virtual_dir_attach_real_child()
 * as well as all files under @real_root_path as its children.
 *
 * This enables you to keep the server path to files in @real_root_path unchanged
 * while also using the virtual folders. (@real_root_path/fileA is still accessible as "/fileA",
 * if you used phodav_virtual_dir_attach_real_child(),
 * the path would change to "/real_root-basename/fileA")
 *
 * This does not check for any conflicts between the virtual directories and
 * the real files - virtual directories take precedence (e.g. in g_file_get_child()).
 *
 * Since: 2.5
 **/
void
phodav_virtual_dir_root_set_real (PhodavVirtualDir *root,
                                  const gchar      *real_root_path)
{
  g_return_if_fail (root != NULL);
  g_return_if_fail (is_root(root));

  g_clear_object (&root->real_root);
  if (real_root_path)
    root->real_root = g_file_new_for_path (real_root_path);
  else
    root->real_root = NULL;
}

/**
 * phodav_virtual_dir_root_get_real:
 * @root: #PhodavVirtualDir obtained from phodav_virtual_dir_new_root()
 *
 * Since: 2.5
 * Returns: (transfer full): the #GFile previously set by phodav_virtual_dir_root_set_real(),
 * otherwise NULL.
 **/
GFile *
phodav_virtual_dir_root_get_real (PhodavVirtualDir *root)
{
  g_return_val_if_fail (root != NULL, NULL);
  g_return_val_if_fail (is_root(root), NULL);

  if (root->real_root)
    return g_object_ref (root->real_root);
  return NULL;
}

/**
 * phodav_virtual_dir_new_dir:
 * @root: #PhodavVirtualDir returned by phodav_virtual_dir_new_root()
 * @path: path of the #PhodavVirtualDir that should be created
 * @error: (nullable): #GError to set on error, or %NULL to ignore
 *
 * Tries to create a new virtual directory with the specified @path.
 * If it fails, @error is set accordingly.
 *
 * A real #GFile child cannot act as parent to #PhodavVirtualDir.
 *
 * Note that this does not create parent directories.
 * You have to call this repeatedly yourself if the parent(s) don't exist yet.
 *
 * Since: 2.5
 * Returns: (transfer full): a new #PhodavVirtualDir that corresponds to the @path
 **/
PhodavVirtualDir *
phodav_virtual_dir_new_dir (PhodavVirtualDir  *root,
                            const gchar       *path,
                            GError           **error)
{
  GFile *f = NULL;
  PhodavVirtualDir *file = NULL, *parent;
  gchar *dir = NULL, *base = NULL;

  g_return_val_if_fail (root != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  dir = g_path_get_dirname (path);
  if (!dir || !g_strcmp0 (dir, "."))
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_INVALID_FILENAME,
                           "invalid path");
      goto end;
    }

  f = phodav_virtual_dir_find_child_recursive (root, dir, NULL);
  if (!f)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_NOT_FOUND,
                           "parent dir not found");
      goto end;
    }
  if (!PHODAV_IS_VIRTUAL_DIR (f))
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "cannot add virtual dir to real parent");
      goto end;
    }
  parent = PHODAV_VIRTUAL_DIR (f);

  base = g_path_get_basename (path);
  if (phodav_virtual_dir_find_direct_child (parent, base))
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_EXISTS,
                           "dir already exists");
      goto end;
    }

  file = g_object_new (PHODAV_TYPE_VIRTUAL_DIR, NULL);
  file->path = g_strdup (path);
  file->dummy = FALSE;
  parent->children = g_list_prepend (parent->children, g_object_ref (file));
  g_object_weak_ref (G_OBJECT (parent), parent_gone_cb, file);
  /* weak ref to parent allows us to remove all subdirectories by dropping the last ref to parent */
  file->parent = parent;

end:
  g_clear_pointer (&dir, g_free);
  g_clear_pointer (&base, g_free);
  g_clear_object (&f);
  return file;
}

/**
 * phodav_virtual_dir_attach_real_child:
 * @parent: a #PhodavVirtualDir that should simulate the direct ancestor of @child
 * @child: a real file that should be linked to @parent
 *
 * If successful, @child becomes a descendant of @parent.
 *
 * Keep in mind that the link is only unidirectional - @child does not know about its @parent.
 * That means that functions such as g_file_enumerate_children(), g_file_resolve_relative_path()
 * (used with @parent) work as expected, but g_file_get_parent() (used with @child) does not.
 *
 * If you want to add a #PhodavVirtualDir to @parent, use phodav_virtual_dir_new_dir() instead.
 *
 * Since: 2.5
 * Returns: %TRUE on success, otherwise %FALSE
 **/
gboolean
phodav_virtual_dir_attach_real_child (PhodavVirtualDir *parent,
                                      GFile            *child)
{
  g_return_val_if_fail (parent != NULL, FALSE);
  g_return_val_if_fail (child != NULL, FALSE);
  g_return_val_if_fail (PHODAV_IS_VIRTUAL_DIR (parent), FALSE);
  g_return_val_if_fail (!PHODAV_IS_VIRTUAL_DIR (child), FALSE);

  gchar *base = g_file_get_basename (child);
  if (phodav_virtual_dir_find_direct_child (parent, base))
    {
      g_free (base);
      return FALSE;
    }
  g_free (base);

  parent->children = g_list_prepend (parent->children, g_object_ref (child));
  return TRUE;
}
