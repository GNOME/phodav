/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "phodav-multistatus.h"

#include <libxml/xmlwriter.h>

#include "phodav-utils.h"

ResponseProp *
response_prop_new (xmlNodePtr node, gint status)
{
  ResponseProp *prop;

  g_return_val_if_fail (node != NULL, NULL);

  prop = g_new0 (ResponseProp, 1);
  prop->node = node;
  prop->status = status;

  return prop;
}

void
response_prop_free (ResponseProp *prop)
{
  if (!prop)
    return;

  if (prop->node)
    xmlFreeNode (prop->node);

  g_free (prop);
}

static gint
response_prop_compare (gconstpointer a, gconstpointer b)
{
  const ResponseProp *pa = a;
  const ResponseProp *pb = b;

  return pa->status - pb->status;
}

Response *
response_new (GList *props, gint status)
{
  Response *r;

  g_return_val_if_fail (props != NULL || status > 0, NULL);

  r = g_new0 (Response, 1);
  r->status = status;
  /* the response groups the properties by status */
  r->props = g_list_sort (props, response_prop_compare);

  return r;
}

void
response_free (Response *h)
{
  g_list_free_full (h->props, (GDestroyNotify) response_prop_free);
  g_free (h);
}

static gchar *
status_to_string (gint status)
{
  return g_strdup_printf ("HTTP/1.1 %d %s",
                          status, soup_status_get_phrase (status));
}

static int
multistatus_write (void *context, const char *buffer, int len)
{
  PhodavStream *stream = context;

  return phodav_stream_write (stream, (const guint8 *) buffer, len) ? len : -1;
}

static void
writer_start (xmlTextWriterPtr writer, const gchar *name)
{
  xmlTextWriterStartElementNS (writer, BAD_CAST "D", BAD_CAST name, NULL);
}

static void
writer_text (xmlTextWriterPtr writer, const gchar *name, const gchar *text)
{
  writer_start (writer, name);
  xmlTextWriterWriteString (writer, BAD_CAST text);
  xmlTextWriterEndElement (writer);
}

static void
writer_status (xmlTextWriterPtr writer, gint status)
{
  g_autofree gchar *text = status_to_string (status);

  writer_text (writer, "status", text);
}

static void
writer_prop_node (xmlTextWriterPtr writer, xmlNodePtr node)
{
  xmlBufferPtr buffer = xmlBufferCreate ();

  xmlNodeDump (buffer, NULL, node, 0, 0);
  xmlTextWriterWriteRaw (writer, xmlBufferContent (buffer));
  xmlBufferFree (buffer);
}

static void
writer_propstat (xmlTextWriterPtr writer, GList *props)
{
  GList *l = props;

  while (l)
    {
      gint status = ((ResponseProp *) l->data)->status;

      writer_start (writer, "propstat");
      writer_start (writer, "prop");

      while (l && ((ResponseProp *) l->data)->status == status)
        {
          writer_prop_node (writer, ((ResponseProp *) l->data)->node);
          l = l->next;
        }

      xmlTextWriterEndElement (writer); /* prop */
      writer_status (writer, status);
      xmlTextWriterEndElement (writer); /* propstat */
    }
}

gint
set_response_multistatus (PhodavRequest *req,
                          GHashTable *mstatus)
{
  GHashTableIter iter;
  Response *resp;
  gchar *path;
  PhodavStream *stream;
  xmlOutputBufferPtr output;
  xmlTextWriterPtr writer;

  stream = phodav_request_stream_start (req, SOUP_STATUS_MULTI_STATUS,
                                        "application/xml");
  if (!stream)
    return SOUP_STATUS_INTERNAL_SERVER_ERROR;

  output = xmlOutputBufferCreateIO (multistatus_write, NULL, stream, NULL);
  if (!output)
    {
      phodav_stream_end (stream);
      return SOUP_STATUS_INTERNAL_SERVER_ERROR;
    }

  writer = xmlNewTextWriter (output);
  if (!writer)
    {
      xmlOutputBufferClose (output);
      phodav_stream_end (stream);
      return SOUP_STATUS_INTERNAL_SERVER_ERROR;
    }

  xmlTextWriterStartDocument (writer, NULL, "UTF-8", NULL);
  xmlTextWriterStartElementNS (writer, BAD_CAST "D",
                               BAD_CAST "multistatus", BAD_CAST "DAV:");

  g_hash_table_iter_init (&iter, mstatus);
  while (g_hash_table_iter_next (&iter, (gpointer *) &path, (gpointer *) &resp))
    {
      g_autofree gchar *escaped = g_uri_escape_bytes ((const guint8 *) path, strlen (path),
                                                      G_URI_RESERVED_CHARS_ALLOWED_IN_PATH);
      g_autoptr(GUri) new_uri = g_uri_parse_relative (phodav_request_get_uri (req), escaped,
                                                      SOUP_HTTP_URI_FLAGS, NULL);
      g_autofree gchar *text = new_uri ? g_uri_to_string (new_uri) : g_strdup (escaped);

      writer_start (writer, "response");
      writer_text (writer, "href", text);

      if (resp->props)
        writer_propstat (writer, resp->props);
      else if (resp->status)
        writer_status (writer, resp->status);

      xmlTextWriterEndElement (writer); /* response */
    }

  xmlTextWriterEndDocument (writer);
  xmlFreeTextWriter (writer);
  phodav_stream_end (stream);

  return SOUP_STATUS_MULTI_STATUS;
}
