/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/* Running the daemon as a Windows service. */

#pragma once

#include <glib.h>

#include "drive-mapping.h"

G_BEGIN_DECLS

/* the daemon main loop, run until it is asked to stop */
typedef void (*Win32RunFunc) (gpointer user_data);

/* ask the daemon to stop; called from the service control handler,
 * on its own thread */
typedef void (*Win32StopFunc) (gpointer user_data);

void win32_service_run (SharedDrive *drive,
                        Win32RunFunc run,
                        Win32StopFunc stop,
                        gpointer user_data);

G_END_DECLS
