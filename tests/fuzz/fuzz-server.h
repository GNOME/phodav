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
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Send @request to the fuzzed server and drain its response.
 * The first call sets the server up, over a temporary directory
 * with this fixture:
 *   /test      a regular file
 *   /dir/      a directory containing /dir/file */
void fuzz_server_send (const guint8 *request, gsize length);

/* Drop the server (with its locks) and restore the initial fixture,
 * which the requests may have mutated. Must not be called while a
 * request is in flight. */
void fuzz_server_reset (void);

G_END_DECLS
