/*
  Copyright (C) 2013 Red Hat, Inc.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifndef __PHODAV_SERVER_H__
#define __PHODAV_SERVER_H__

#include <glib-object.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define PHODAV_TYPE_SERVER            (phodav_server_get_type ())
#define PHODAV_SERVER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), PHODAV_TYPE_SERVER, PhodavServer))
#define PHODAV_SERVER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), PHODAV_TYPE_SERVER, PhodavServerClass))
#define PHODAV_IS_SERVER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PHODAV_TYPE_SERVER))
#define PHODAV_IS_SERVER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), PHODAV_TYPE_SERVER))
#define PHODAV_SERVER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), PHODAV_TYPE_SERVER, PhodavServerClass))

typedef struct _PhodavServer PhodavServer;
typedef struct _PhodavServerClass PhodavServerClass;

GType           phodav_server_get_type          (void);

PhodavServer *  phodav_server_new               (const gchar *root);
PhodavServer *  phodav_server_new_for_root_file (GFile *root);
SoupServer *    phodav_server_get_soup_server   (PhodavServer *server);

G_END_DECLS

#endif /* __PHODAV_SERVER_H__ */
