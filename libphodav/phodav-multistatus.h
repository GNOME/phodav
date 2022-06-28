/*
 * Copyright (C) 2013 Red Hat, Inc.
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
#ifndef __PHODAV_MULTISTATUS_H__
#define __PHODAV_MULTISTATUS_H__

#include <libsoup/soup.h>
#include "phodav-priv.h"

G_BEGIN_DECLS

typedef struct _Response
{
  GList *props;
  gint   status;
} Response;

Response *     response_new                      (GList       *props,
                                                  gint         status);
void           response_free                     (Response    *h);

gint           set_response_multistatus          (SoupServerMessage *msg,
                                                  GHashTable        *mstatus);
G_END_DECLS

#endif /* __PHODAV_MULTISTATUS_H__ */
