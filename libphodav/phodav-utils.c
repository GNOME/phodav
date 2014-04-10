/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2; -*- */
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

#include "phodav-utils.h"

void
xml_node_to_string (xmlNodePtr root, xmlChar **mem, int *size)
{
  xmlDocPtr doc;

  doc = xmlNewDoc (BAD_CAST "1.0");
  xmlDocSetRootElement (doc, root);
  // xmlReconciliateNs
  xmlDocDumpMemoryEnc (doc, mem, size, "utf-8");
  /* FIXME: validate document? */
  /*FIXME, pretty print?*/
  xmlFreeDoc (doc);
}
