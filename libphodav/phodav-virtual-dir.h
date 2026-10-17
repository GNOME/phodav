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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define PHODAV_TYPE_VIRTUAL_DIR phodav_virtual_dir_get_type ()

/**
 * PhodavVirtualDir:
 *
 * An in-memory #GFile implementation of a directory, which can be used
 * with all #GFile functions.
 *
 * A #PhodavVirtualDir does not represent any real file. Instead, it is a
 * virtual element that can be used to build a directory tree structure
 * in the memory. However, a #PhodavVirtualDir can have a real file as
 * its child.
 *
 * The first building block of such tree must be phodav_virtual_dir_new_root().
 * Further directories can be added using phodav_virtual_dir_new_dir().
 * To link a real file as a child of a #PhodavVirtualDir, use
 * phodav_virtual_dir_attach_real_child().
 *
 * #PhodavVirtualDir allows you to easily share two resources when they
 * have no common ancestor (like "C:\fileA" and "D:\fileB") or when sharing
 * their common ancestor would be impractical (sharing the whole "/" when
 * the files are "/d1/d2/fileA" and "/d3/d4/fileB").
 *
 * It was designed primarily for the purposes of the
 * [SPICE project](https://www.spice-space.org/) and hence the functionality
 * is very narrow. If other projects find it handy as well, it could be
 * extended quite easily.
 *
 * Supported methods on virtual directories:
 * - GET
 * - PROPFIND
 * - LOCK
 * - UNLOCK
 *
 * This concerns only the virtual directories. If you have a real #GFile
 * linked to a virtual directory, all the other methods are supported on
 * such file.
 *
 * You currently cannot delete a #PhodavVirtualDir. Once the last reference
 * to the root is dropped, the whole structure is destroyed. Children that
 * have other references (non-internal) become dummies, otherwise they are
 * freed.
 *
 * Since: 2.5
 */
G_DECLARE_FINAL_TYPE (PhodavVirtualDir, phodav_virtual_dir, PHODAV, VIRTUAL_DIR, GObject)

PhodavVirtualDir *    phodav_virtual_dir_new_root             (void);
PhodavVirtualDir *    phodav_virtual_dir_new_dir              (PhodavVirtualDir *root,
                                                               const gchar      *path,
                                                               GError          **error);
gboolean              phodav_virtual_dir_attach_real_child    (PhodavVirtualDir *parent,
                                                               GFile            *child);
void                  phodav_virtual_dir_root_set_real        (PhodavVirtualDir *root,
                                                               const gchar      *real_root_path);

GFile *               phodav_virtual_dir_root_get_real        (PhodavVirtualDir *root);

#define PHODAV_TYPE_VIRTUAL_DIR_ENUMERATOR phodav_virtual_dir_enumerator_get_type ()
G_DECLARE_FINAL_TYPE (PhodavVirtualDirEnumerator, phodav_virtual_dir_enumerator,
                      PHODAV, VIRTUAL_DIR_ENUMERATOR, GFileEnumerator)

G_END_DECLS
