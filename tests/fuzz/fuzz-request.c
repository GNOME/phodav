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

/* Fuzz the whole HTTP request: the method, the headers (the If
 * conditions, Depth, Destination...) and the body, exactly as a
 * client would send them. */

#include "config.h"

#include <glib.h>

#include "fuzz-server.h"

/* the raw requests mutate the fixture and take locks which do not
 * expire: restore the initial state regularly, so that the fuzzing
 * keeps making progress */
#define RESET_INTERVAL 1000

static guint64 iterations;

gint
LLVMFuzzerTestOneInput (const guint8 *data, gsize size)
{
  if (iterations++ % RESET_INTERVAL == 0)
    fuzz_server_reset ();

  fuzz_server_send (data, size);

  return 0;
}
