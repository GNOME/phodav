/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
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
