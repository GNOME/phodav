/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <libsoup/soup.h>

#include "phodav-priv.h"

G_BEGIN_DECLS

/* a property of a response, with the status of the operation
 * reported for it */
typedef struct _ResponseProp
{
  xmlNodePtr node;
  gint status;
} ResponseProp;

ResponseProp *response_prop_new (xmlNodePtr node,
                                 gint status);
void response_prop_free (ResponseProp *prop);

/* the properties of a response, in the order they were collected */
typedef struct _Response
{
  GList *props; /* ResponseProp */
  gint status;
} Response;

Response *response_new (GList *props,
                        gint status);
void response_free (Response *h);

gint set_response_multistatus (PhodavRequest *req,
                               GHashTable *mstatus);
G_END_DECLS
