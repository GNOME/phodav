/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <libsoup/soup.h>

#include "phodav-priv.h"

G_BEGIN_DECLS

typedef struct _Response
{
  GList *props;
  gint status;
} Response;

Response *response_new (GList *props,
                        gint status);
void response_free (Response *h);

gint set_response_multistatus (SoupServerMessage *msg,
                               GHashTable *mstatus);
G_END_DECLS
