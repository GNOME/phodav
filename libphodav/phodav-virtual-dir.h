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

#ifndef __PHODAV_VIRTUAL_DIR_H__
#define __PHODAV_VIRTUAL_DIR_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define PHODAV_TYPE_VIRTUAL_DIR phodav_virtual_dir_get_type ()
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

#endif /* __PHODAV_VIRTUAL_DIR_H__ */
