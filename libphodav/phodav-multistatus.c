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

#define MULTISTATUS_CHUNK_SIZE (64 * 1024)

typedef struct _MultistatusPull
{
  GHashTable *mstatus;
  PhodavRequest *req;
  GHashTableIter iter;
  gboolean iter_started;
  gboolean doc_started;
  gboolean doc_done;
  xmlTextWriterPtr writer;
  xmlOutputBufferPtr output;
  GByteArray *buffer;
  GList *pending_buffers;
  xmlNsPtr ns;
} MultistatusPull;

static gint
multistatus_write (void *context, const gchar *buffer, gint len)
{
  MultistatusPull *pull = context;

  g_byte_array_append (pull->buffer, (const guint8 *) buffer, len);

  return len;
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
multistatus_free_buffers (MultistatusPull *pull)
{
  g_clear_list (&pull->pending_buffers, (GDestroyNotify) xmlBufferFree);
}

/* xmlTextWriterWriteRaw() keeps a reference to the raw buffer until the
 * output buffer is flushed: keep it alive until then */
static void
writer_prop_node (MultistatusPull *pull, xmlNodePtr node)
{
  xmlBufferPtr buffer = xmlBufferCreate ();

  xmlNodeDump (buffer, NULL, node, 0, 0);
  xmlTextWriterWriteRaw (pull->writer, xmlBufferContent (buffer));
  pull->pending_buffers = g_list_prepend (pull->pending_buffers, buffer);
}

static void
writer_propstat (MultistatusPull *pull, GList *props)
{
  xmlTextWriterPtr writer = pull->writer;
  GList *l = props;

  while (l)
    {
      gint status = ((ResponseProp *) l->data)->status;

      writer_start (writer, "propstat");
      writer_start (writer, "prop");

      while (l && ((ResponseProp *) l->data)->status == status)
        {
          writer_prop_node (pull, ((ResponseProp *) l->data)->node);
          l = l->next;
        }

      xmlTextWriterEndElement (writer); /* prop */
      writer_status (writer, status);
      xmlTextWriterEndElement (writer); /* propstat */
    }
}

static gboolean
multistatus_doc_start (MultistatusPull *pull, GError **error)
{
  pull->output = xmlOutputBufferCreateIO (multistatus_write, NULL, pull, NULL);
  if (!pull->output)
    goto fail;

  pull->writer = xmlNewTextWriter (pull->output);
  if (!pull->writer)
    goto fail;

  xmlTextWriterStartDocument (pull->writer, NULL, "UTF-8", NULL);
  xmlTextWriterStartElementNS (pull->writer, BAD_CAST "D",
                               BAD_CAST "multistatus", BAD_CAST "DAV:");
  pull->doc_started = TRUE;

  return TRUE;

fail:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Cannot create the multistatus writer");
  return FALSE;
}

static gboolean
multistatus_write_response (MultistatusPull *pull, const gchar *path, Response *resp)
{
  g_autofree gchar *escaped = g_uri_escape_bytes ((const guint8 *) path, strlen (path),
                                                  G_URI_RESERVED_CHARS_ALLOWED_IN_PATH);
  g_autoptr(GUri) new_uri = g_uri_parse_relative (phodav_request_get_uri (pull->req), escaped,
                                                  SOUP_HTTP_URI_FLAGS, NULL);
  g_autofree gchar *text = new_uri ? g_uri_to_string (new_uri) : g_strdup (escaped);

  writer_start (pull->writer, "response");
  writer_text (pull->writer, "href", text);

  if (resp->props)
    writer_propstat (pull, resp->props);
  else if (resp->status)
    writer_status (pull->writer, resp->status);

  xmlTextWriterEndElement (pull->writer); /* response */

  return TRUE;
}

static GBytes *
multistatus_pull (gpointer data, GCancellable *cancellable, GError **error)
{
  MultistatusPull *pull = data;

  if (!pull->doc_started && !multistatus_doc_start (pull, error))
    return NULL;

  while (!pull->doc_done && pull->buffer->len < MULTISTATUS_CHUNK_SIZE)
    {
      gchar *path;
      Response *resp;

      if (!pull->iter_started)
        {
          g_hash_table_iter_init (&pull->iter, pull->mstatus);
          pull->iter_started = TRUE;
        }

      if (g_hash_table_iter_next (&pull->iter, (gpointer *) &path, (gpointer *) &resp))
        {
          multistatus_write_response (pull, path, resp);
          xmlOutputBufferFlush (pull->output);
          multistatus_free_buffers (pull);
        }
      else
        {
          xmlTextWriterEndDocument (pull->writer);
          xmlOutputBufferFlush (pull->output);
          multistatus_free_buffers (pull);
          g_clear_pointer (&pull->writer, xmlFreeTextWriter);
          pull->output = NULL;
          pull->doc_done = TRUE;
        }
    }

  if (pull->buffer->len == 0)
    return NULL;

  {
    GBytes *bytes = g_byte_array_free_to_bytes (g_steal_pointer (&pull->buffer));

    pull->buffer = g_byte_array_new ();
    return bytes;
  }
}

static void
multistatus_pull_free (gpointer data)
{
  MultistatusPull *pull = data;

  if (pull->writer)
    xmlFreeTextWriter (pull->writer);
  else if (pull->output)
    xmlOutputBufferClose (pull->output);
  multistatus_free_buffers (pull);
  g_clear_pointer (&pull->buffer, g_byte_array_unref);
  g_clear_pointer (&pull->mstatus, g_hash_table_unref);
  if (pull->ns)
    xmlFreeNs (pull->ns);
  g_clear_pointer (&pull->req, phodav_request_unref);
  g_free (pull);
}

gint
set_response_multistatus (PhodavRequest *req,
                          GHashTable *mstatus,
                          xmlNsPtr ns)
{
  MultistatusPull *pull = g_new0 (MultistatusPull, 1);

  pull->mstatus = g_hash_table_ref (mstatus);
  pull->req = phodav_request_ref (req);
  pull->buffer = g_byte_array_new ();
  pull->ns = ns;

  phodav_request_stream_start (req, SOUP_STATUS_MULTI_STATUS,
                               "application/xml",
                               multistatus_pull, pull,
                               multistatus_pull_free);

  return SOUP_STATUS_MULTI_STATUS;
}
