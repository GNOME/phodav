/* guuid.h - UUID functions
 *
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * licence, or (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA.
 *
 * Authors: Marc-André Lureau <marcandre.lureau@redhat.com>
 */

#ifndef __G_UUID_H__
#define __G_UUID_H__

#include <glib.h>

G_BEGIN_DECLS

/**
 * GUuidNamespace:
 * @G_UUID_NAMESPACE_DNS: for fully-qualified domain name
 * @G_UUID_NAMESPACE_URL: for URLs
 * @G_UUID_NAMESPACE_OID: for ISO Object IDs (OIDs)
 * @G_UUID_NAMESPACE_X500: for X.500 istinguished Names (DNs)
 *
 * The well-known UUID namespace to look up with
 * g_uuid_get_namespace().
 *
 * Note that the #GUuidNamespace enumeration may be extended at a
 * later date to include new namespaces.
 *
 * Since: 2.40
 */
typedef enum
{
  G_UUID_NAMESPACE_DNS,
  G_UUID_NAMESPACE_URL,
  G_UUID_NAMESPACE_OID,
  G_UUID_NAMESPACE_X500
} GUuidNamespace;

typedef struct _GUuid GUuid;

struct _GUuid {
  guint8 bytes[16];
};


#define G_UUID_INIT(u0,u1,u2,u3,u4,u5,u6,u7,u8,u9,u10,u11,u12,u13,u14,u15) \
  { { u0,u1,u2,u3,u4,u5,u6,u7,u8,u9,u10,u11,u12,u13,u14,u15 } }

#define G_UUID_INIT_NIL G_UUID_INIT(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)

#define G_UUID_DEFINE_STATIC(name,u0,u1,u2,u3,u4,u5,u6,u7,u8,u9,u10,u11,u12,u13,u14,u15) \
  static const GUuid name =                                             \
    G_UUID_INIT(u0,u1,u2,u3,u4,u5,u6,u7,u8,u9,u10,u11,u12,u13,u14,u15);

gboolean      g_uuid_is_nil                (const GUuid   *uuid);
gboolean      g_uuid_equal                 (gconstpointer uuid1,
                                            gconstpointer uuid2);

gchar *       g_uuid_to_string             (const GUuid   *uuid);
gboolean      g_uuid_string_is_valid       (const gchar   *str,
                                            gssize         len);
gboolean      g_uuid_from_string           (const gchar   *str,
                                            gssize         len,
                                            GUuid         *uuid);

gchar *       g_uuid_string_random         (void);
void          g_uuid_generate4             (GUuid         *uuid);

const GUuid * g_uuid_get_namespace         (GUuidNamespace namespace);
void          g_uuid_generate3             (GUuid         *uuid,
                                            const GUuid   *namespace,
                                            const guchar  *name,
                                            gssize         length);
void          g_uuid_generate5             (GUuid         *uuid,
                                            const GUuid   *namespace,
                                            const guchar  *name,
                                            gssize         length);

G_END_DECLS

#endif  /* __G_UUID_H__ */
