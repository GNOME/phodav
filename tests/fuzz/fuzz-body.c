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

/* Fuzz the request bodies: the framing (the method, the path and the
 * fixed headers) is chosen from the first input byte, the rest of
 * the input is the body. This reaches the XML request parsers
 * (PROPFIND, PROPPATCH, LOCK) with a consistent Content-Length,
 * which mutations of a raw request would rarely keep. */

#include "config.h"

#include <glib.h>

#include "fuzz-server.h"

typedef struct _Framing
{
  const gchar *method;
  const gchar *path;
  const gchar *headers; /* \r\n-terminated header lines, or "" */
} Framing;

/* the LOCK framings take a short timeout, so that the locks taken
 * over the iterations do not accumulate */
static const Framing framings[] = {
  { "PROPFIND", "/test", "Depth: 0\r\n" },
  { "PROPFIND", "/dir", "Depth: 1\r\n" },
  /* depth infinity on a collection is forbidden */
  { "PROPFIND", "/dir", "Depth: infinity\r\n" },
  { "PROPPATCH", "/test", "" },
  { "LOCK", "/test", "Timeout: Second-1\r\n" },
  /* an empty body makes it a lock refresh, which requires an If
   * header identifying the lock */
  { "LOCK", "/test", "Timeout: Second-1\r\nIf: (<urn:uuid:00000000-0000-0000-0000-000000000000>)\r\n" },
  { "PUT", "/test", "" },
  { "MKCOL", "/dir/new", "" },
};

gint
LLVMFuzzerTestOneInput (const guint8 *data, gsize size)
{
  const Framing *framing;
  g_autoptr(GString) request = NULL;

  if (size == 0)
    return 0;

  framing = &framings[data[0] % G_N_ELEMENTS (framings)];

  request = g_string_new (NULL);
  g_string_append_printf (request,
                          "%s %s HTTP/1.1\r\n"
                          "Host: fuzz\r\n"
                          "Content-Length: %" G_GSIZE_FORMAT "\r\n"
                          "Connection: close\r\n"
                          "%s"
                          "\r\n",
                          framing->method, framing->path, size - 1,
                          framing->headers);
  g_string_append_len (request, (const gchar *) data + 1, size - 1);

  fuzz_server_send ((const guint8 *) request->str, request->len);

  return 0;
}
