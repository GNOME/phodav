/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/* Mapping the shared folder to a Windows drive letter, over the
 * WebDAV server of the daemon. */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SharedDrive SharedDrive;

SharedDrive *shared_drive_new (void);
void shared_drive_free (SharedDrive *drive);

gchar shared_drive_get_letter (SharedDrive *drive);
void shared_drive_map (SharedDrive *drive, guint port);
void shared_drive_unmap (SharedDrive *drive);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SharedDrive, shared_drive_free)

G_END_DECLS
