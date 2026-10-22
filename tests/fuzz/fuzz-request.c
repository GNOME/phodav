/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
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
