/* guuid.c - UUID functions
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

#include "config.h"

#include <string.h>
#include <glib.h>

#include "guuid.h"

/*
 * SECTION:uuid
 * @title: GUuid
 * @short_description: a universal unique identifier
 *
 * A UUID, or Universally unique identifier, is intended to uniquely
 * identify information in a distributed environment. For the
 * definition of UUID, see <ulink
 * url="tools.ietf.org/html/rfc4122.html">RFC 4122</ulink>.
 *
 * The creation of UUIDs does not require a centralized authority.
 *
 * UUIDs are of relatively small size (128 bits, or 16 bytes). The
 * common string representation (ex:
 * 1d6c0810-2bd6-45f3-9890-0268422a6f14) needs 37 bytes.
 *
 * There are different mechanisms to generate UUIDs. The UUID
 * specification defines 5 versions. If all you want is a unique ID,
 * you should probably call g_uuid_string_random() or g_uuid_generate4(),
 * which is the version 4.
 *
 * If you want to generate UUID based on a name within a namespace
 * (%G_UUID_NAMESPACE_DNS for fully-qualified domain name for
 * example), you may want to use version 5, g_uuid_generate5() using a
 * SHA-1 hash, or its alternative based on a MD5 hash, version 3
 * g_uuid_generate3().
 *
 * You can look up well-known namespaces with g_uuid_get_namespace().
 *
 * Since: 2.40
 **/

/**
 * GUuid:
 *
 * A structure that holds a UUID.
 *
 * Since: 2.40
 */

/**
 * G_UUID_DEFINE_STATIC:
 * @name: the read-only variable name to define.
 *
 * A convenience macro to define a #GUuid.
 *
 * Since: 2.40
 */

/**
 * G_UUID_NIL:
 *
 * A macro that can be used to initialize a #GUuid to the nil value.
 * It can be used as initializer when declaring a variable, but it
 * cannot be assigned to a variable.
 *
 * |[
 *   GUuid uuid = G_UUID_NIL;
 * ]|
 *
 * Since: 2.40
 */

G_UUID_DEFINE_STATIC (uuid_nil, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

/*
 * Common namespaces as defined in rfc4122:
 * http://tools.ietf.org/html/rfc4122.html#appendix-C
 */
G_UUID_DEFINE_STATIC (uuid_dns,
                      0x6b, 0xa7, 0xb8, 0x10,
                      0x9d, 0xad,
                      0x11, 0xd1,
                      0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8)

G_UUID_DEFINE_STATIC (uuid_url,
                      0x6b, 0xa7, 0xb8, 0x11,
                      0x9d, 0xad,
                      0x11, 0xd1,
                      0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8)

G_UUID_DEFINE_STATIC (uuid_oid,
                      0x6b, 0xa7, 0xb8, 0x12,
                      0x9d, 0xad,
                      0x11, 0xd1,
                      0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8)

G_UUID_DEFINE_STATIC (uuid_x500,
                      0x6b, 0xa7, 0xb8, 0x14,
                      0x9d, 0xad,
                      0x11, 0xd1,
                      0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8)

/**
 * g_uuid_equal:
 * @uuid1: pointer to the first #GUuid
 * @uuid2: pointer to the second #GUuid
 *
 * Checks if two UUIDs are equal.
 *
 * Returns: %TRUE if @uuid1 is equal to @uuid2, %FALSE otherwise
 * Since: 2.40
 **/
gboolean
g_uuid_equal (gconstpointer uuid1,
              gconstpointer uuid2)
{
  g_return_val_if_fail (uuid1 != NULL, FALSE);
  g_return_val_if_fail (uuid2 != NULL, FALSE);

  return memcmp (uuid1, uuid2, sizeof (GUuid)) == 0;
}

/**
 * g_uuid_is_nil:
 * @uuid: a #GUuid
 *
 * Checks whether @uuid is the nil UUID (all the 128 bits are zero)
 *
 * Returns: %TRUE if @uuid is nil, %FALSE otherwise
 * Since: 2.40
 **/
gboolean
g_uuid_is_nil (const GUuid *uuid)
{
  g_return_val_if_fail (uuid != NULL, FALSE);

  return g_uuid_equal (uuid, &uuid_nil);
}

/**
 * g_uuid_to_string:
 * @uuid: a #GUuid
 *
 * Creates a string representation of @uuid, of the form
 * 06e023d5-86d8-420e-8103-383e4566087a (no braces nor urn:uuid:
 * prefix).
 *
 * Returns: A string that should be freed with g_free().
 * Since: 2.40
 **/
gchar *
g_uuid_to_string (const GUuid *uuid)
{
  const guint8 *bytes;

  g_return_val_if_fail (uuid != NULL, NULL);

  bytes = uuid->bytes;

  return g_strdup_printf ("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x"
                          "-%02x%02x%02x%02x%02x%02x",
                          bytes[0], bytes[1], bytes[2], bytes[3],
                          bytes[4], bytes[5], bytes[6], bytes[7],
                          bytes[8], bytes[9], bytes[10], bytes[11],
                          bytes[12], bytes[13], bytes[14], bytes[15]);
}

static gboolean
uuid_parse_string (const gchar *str,
                   gssize       len,
                   GUuid       *uuid)
{
  GUuid tmp;
  guint8 *bytes = tmp.bytes;
  gint i, j, hi, lo;
  guint expected_len = 36;

  if (g_str_has_prefix (str, "urn:uuid:"))
    str += 9;
  else if (str[0] == '{')
    expected_len += 2;

  if (len == -1)
    len = strlen (str);

  if (len != expected_len)
    return FALSE;

  /* only if str[0] == '{' above */
  if (expected_len == 38)
    {
      if (str[37] != '}')
        return FALSE;

      str++;
    }

  for (i = 0, j = 0; i < 16;)
    {
      if (j == 8 || j == 13 || j == 18 || j == 23)
        {
          if (str[j++] != '-')
            return FALSE;

          continue;
        }

      hi = g_ascii_xdigit_value (str[j++]);
      lo = g_ascii_xdigit_value (str[j++]);

      if (hi == -1 || lo == -1)
        return FALSE;

      bytes[i++] = hi << 8 | lo;
    }

  if (uuid != NULL)
    *uuid = tmp;

  return TRUE;
}

/**
 * g_uuid_from_string:
 * @str: a string representing a UUID
 * @len: the length of @str (may be -1 if is nul-terminated)
 * @uuid: (out) (caller-allocates): the #GUuid to store the parsed UUID value
 *
 * Reads a UUID from its string representation and set the value in
 * @uuid. See g_uuid_string_is_valid() for examples of accepted string
 * representations.
 *
 *
 * Returns: %TRUE if the @str string is successfully parsed, %FALSE
 * otherwise.
 * Since: 2.40
 **/
gboolean
g_uuid_from_string (const gchar *str,
                    gssize       len,
                    GUuid       *uuid)
{
  g_return_val_if_fail (str != NULL, FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);
  g_return_val_if_fail (len >= -1, FALSE);

  return uuid_parse_string (str, len, uuid);
}

/**
 * g_uuid_string_is_valid:
 * @str: a string representing a UUID
 * @len: the length of @str (may be -1 if is nul-terminated)
 *
 * Parses the string @str and verify if it is a UUID.
 *
 * The function accepts the following syntaxes:
 *
 * - simple forms (e.g. f81d4fae-7dec-11d0-a765-00a0c91e6bf6)
 * - simple forms with curly braces (e.g.
 *   {urn:uuid:f81d4fae-7dec-11d0-a765-00a0c91e6bf6})
 * - URN (e.g. urn:uuid:f81d4fae-7dec-11d0-a765-00a0c91e6bf6)
 *
 * Returns: %TRUE if @str is a valid UUID, %FALSE otherwise.
 * Since: 2.40
 **/
gboolean
g_uuid_string_is_valid (const gchar *str, gssize len)
{
  g_return_val_if_fail (str != NULL, FALSE);
  g_return_val_if_fail (len >= -1, FALSE);

  return uuid_parse_string (str, len, NULL);
}

static void
uuid_set_version (GUuid *uuid, int version)
{
  guint8 *bytes = uuid->bytes;

  /*
   * Set the four most significant bits (bits 12 through 15) of the
   * time_hi_and_version field to the 4-bit version number from
   * Section 4.1.3.
   */
  bytes[6] &= 0x0f;
  bytes[6] |= version << 4;
  /*
   * Set the two most significant bits (bits 6 and 7) of the
   * clock_seq_hi_and_reserved to zero and one, respectively.
   */
  bytes[8] &= 0x3f;
  bytes[8] |= 0x80;
}

/**
 * g_uuid_generate4:
 * @uuid: a #GUuid
 *
 * Generates a random UUID (RFC 4122 version 4).
 * Since: 2.40
 **/
void
g_uuid_generate4 (GUuid *uuid)
{
  int i;
  guint8 *bytes;
  guint32 *ints;

  g_return_if_fail (uuid != NULL);

  bytes = uuid->bytes;
  ints = (guint32*) bytes;
  for (i = 0; i < 4; i++)
    ints[i] = g_random_int ();

  uuid_set_version (uuid, 4);
}

/**
 * g_uuid_string_random:
 *
 * Generates a random UUID (RFC 4122 version 4) as a string.
 *
 * Returns: A string that should be freed with g_free().
 * Since: 2.40
 **/
gchar *
g_uuid_string_random (void)
{
  GUuid uuid;

  g_uuid_generate4 (&uuid);

  return g_uuid_to_string (&uuid);
}

/**
 * g_uuid_get_namespace:
 * @namespace: a #GUuidNamespace namespace
 *
 * Look up one of the well-known namespace UUIDs.
 *
 * Returns: a UUID, or %NULL on failure.
 * Since: 2.40
 **/
const GUuid *
g_uuid_get_namespace (GUuidNamespace namespace)
{
  switch (namespace)
    {
    case G_UUID_NAMESPACE_DNS:
      return &uuid_dns;
    case G_UUID_NAMESPACE_URL:
      return &uuid_url;
    case G_UUID_NAMESPACE_OID:
      return &uuid_oid;
    case G_UUID_NAMESPACE_X500:
      return &uuid_x500;
    default:
      g_return_val_if_reached (NULL);
    }
}

static void
uuid_generate3or5 (GUuid         *uuid,
                   gint           version,
                   GChecksumType  checksum_type,
                   const GUuid   *namespace,
                   const guchar  *name,
                   gssize         length)
{
  GChecksum *checksum;
  gssize digest_len;
  guint8 *digest;

  if (length < 0)
      length = strlen ((const gchar *)name);

  digest_len = g_checksum_type_get_length (checksum_type);
  g_assert (digest_len != -1);

  checksum = g_checksum_new (checksum_type);
  g_return_if_fail (checksum != NULL);

  g_checksum_update (checksum, namespace->bytes, sizeof (namespace->bytes));
  g_checksum_update (checksum, name, length);

  digest = g_malloc (digest_len);
  g_checksum_get_digest (checksum, digest, (gsize*) &digest_len);
  g_assert (digest_len >= 16);

  memcpy (uuid->bytes, digest, 16);
  uuid_set_version (uuid, version);

  g_checksum_free (checksum);
  g_free (digest);
}

/**
 * g_uuid_generate3:
 * @uuid: a #GUuid
 * @namespace: a namespace #GUuid
 * @name: a string
 * @length: size of the name, or -1 if @name is a null-terminated string
 *
 * Generates a UUID based on the MD5 hash of a namespace UUID and a
 * string (RFC 4122 version 3). MD5 is <emphasis>no longer considered
 * secure</emphasis>, and you should only use this if you need
 * interoperability with existing systems that use version 3 UUIDs.
 * For new code, you should use g_uuid_generate5().
 *
 * Since: 2.40
 **/
void
g_uuid_generate3 (GUuid         *uuid,
                  const GUuid   *namespace,
                  const guchar  *name,
                  gssize         length)
{
  g_return_if_fail (uuid != NULL);
  g_return_if_fail (namespace != NULL);
  g_return_if_fail (name != NULL);

  uuid_generate3or5 (uuid, 3, G_CHECKSUM_MD5, namespace, name, length);
}

/**
 * g_uuid_generate5:
 * @uuid: a #GUuid
 * @namespace: a namespace #GUuid
 * @name: a string
 * @length: size of the name, or -1 if @name is a null-terminated string
 *
 * Generates a UUID based on the SHA-1 hash of a namespace UUID and a
 * string (RFC 4122 version 5).
 *
 * Since: 2.40
 **/
void
g_uuid_generate5 (GUuid         *uuid,
                  const GUuid   *namespace,
                  const guchar  *name,
                  gssize         length)
{
  g_return_if_fail (uuid != NULL);
  g_return_if_fail (namespace != NULL);
  g_return_if_fail (name != NULL);

  uuid_generate3or5 (uuid, 5, G_CHECKSUM_SHA1, namespace, name, length);
}
