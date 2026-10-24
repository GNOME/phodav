/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <libsoup/soup.h>

#include "phodav-priv.h"

G_BEGIN_DECLS

void remove_trailing (gchar *str, gchar c);
gboolean path_has_dot_dot_segment (const gchar *path);

/* canonicalize a resource path: empty segments ("//") and trailing
 * slashes are skipped, so that two spellings of the same resource
 * compare equal */
gchar *phodav_path_canonicalize (const gchar *path);

/* is @path a resource path under the share root? It must be absolute,
 * with no second absolute anchor (which would escape the root on
 * Windows) and no dot-dot segment. */
gboolean phodav_path_is_safe (const gchar *path);

/* parse a Depth header value: a missing header defaults to DEPTH_INFINITY
 * (RFC 4918 section 9.1); returns FALSE for a syntactically invalid
 * value, which the caller must reject with 400 */
gboolean depth_from_string (const gchar *depth, DepthType *out);
const gchar *depth_to_string (DepthType depth);

/* parse the Timeout header of a LOCK request (RFC 4918 section 10.7):
 * a missing header, "Infinite" and an unrecognized value all mean
 * no timeout */
guint timeout_from_string (const gchar *timeout);

/* is @path the tree rooted at @root, or one of its members? */
gboolean path_is_covered (const gchar *path, const gchar *root);

/* a strong ETag for the representation described by @info. GIO builds
 * the local file ETags from the modification time only: two writes
 * within the same filesystem timestamp tick produce the same one.
 * Include the size and the inode when the file backend reports them,
 * so that the tag changes with the representation. Returns NULL when
 * @info has no ETag. */
gchar *phodav_etag_from_info (GFileInfo *info);

/* does an If-Match / If-None-Match header value (RFC 9110 section 13.1)
 * match the current @etag of the resource, which exists per @exists? */
gboolean etag_header_matches (const gchar *header, const gchar *etag, gboolean exists);

/* format @time as an HTTP-date (RFC 9110 section 5.6.7) */
gchar *http_time_to_string (guint64 time);

/* do the If-None-Match / If-Modified-Since preconditions of a GET or
 * HEAD request hold, i.e. does the client hold a current
 * representation? RFC 9110 sections 13.1.2 and 13.1.3 */
gboolean http_request_not_modified (SoupMessageHeaders *headers,
                                    const gchar *etag,
                                    guint64 mtime);

/* how does a Range request header apply to the current
 * representation? RFC 9110 section 14 */
typedef enum
{
  HTTP_RANGE_NONE,          /* no usable range: serve it all */
  HTTP_RANGE_PARTIAL,       /* serve @first to @last, inclusive */
  HTTP_RANGE_UNSATISFIABLE, /* there is no such byte: answer 416 */
} HttpRangeType;

/* evaluate a Range request header against the representation
 * validated by @etag and @mtime and of length @size: the range only
 * applies when an If-Range precondition holds. Returns the bytes to
 * serve in @first/@last (inclusive) when returning
 * HTTP_RANGE_PARTIAL. */
HttpRangeType http_request_range (SoupMessageHeaders *headers,
                                  const gchar *etag,
                                  guint64 mtime,
                                  guint64 size,
                                  guint64 *first,
                                  guint64 *last);

typedef struct _DavDoc DavDoc;

struct _DavDoc
{
  xmlDocPtr doc;
  xmlNodePtr root;
};

gboolean davdoc_parse (DavDoc *dd, SoupMessageBody *body, const gchar *name);
void davdoc_free (DavDoc *dd);

/* GLib auto cleanup support for the libxml2 tree types and DavDoc,
 * so that the request documents do not have to be freed by hand */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (DavDoc, davdoc_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (xmlDoc, xmlFreeDoc)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (xmlParserCtxt, xmlFreeParserCtxt)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (xmlNode, xmlFreeNode)

void xml_node_to_string (xmlNodePtr root, xmlChar **mem, gint *size);
gboolean xml_node_is_element (xmlNodePtr node);
gboolean xml_node_has_name (xmlNodePtr node, const gchar *name);
gboolean xml_node_has_name_ns (xmlNodePtr node, const gchar *name, const gchar *ns_href);
void xml_node_debug (xmlNodePtr node);
gchar *xml_node_get_xattr_name (xmlNodePtr node, const gchar *prefix);

G_END_DECLS
