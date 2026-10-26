/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include "phodav-priv.h"

G_BEGIN_DECLS

typedef struct _PhodavBackend PhodavBackend;
typedef struct _PhodavBackendClass PhodavBackendClass;

/* A resolved resource. The backend owns the implementation details of
 * how @fd, @parent_fd and the GFiles are kept alive. */
struct _PhodavPath
{
  gint ref_count;
  PhodavBackend *backend;
  GFile *file;       /* the resource as it was named */
  GFile *read_file;  /* the held object, or @file without a descriptor */
  GFile *named_file; /* the entry below the held parent, or @read_file */
  gchar *name;       /* the final component, NULL for the root */
  int fd;            /* descriptor of the resolved object, or -1 */
  int parent_fd;     /* descriptor of its parent directory, or -1 */
};

/* The filesystem backend contract. Implementations only look resources
 * up and perform the primitive operations; the path API below applies
 * the common existence and error policy at the interface boundary. */
struct _PhodavBackendClass
{
  const gchar *name;

  PhodavPath *(*resolve) (PhodavBackend *backend,
                          PathHandler *handler,
                          const gchar *relative,
                          PhodavPathMode mode,
                          gint *status);
  PhodavPath *(*resolve_child) (PhodavBackend *backend,
                                PhodavPath *parent,
                                const gchar *name,
                                PhodavPathMode mode,
                                gint *status);

  GFileInfo *(*query_info) (PhodavPath *path,
                            const gchar *attributes,
                            GFileQueryInfoFlags flags,
                            GCancellable *cancellable,
                            GError **error);
  GFileInputStream *(*open) (PhodavPath *path,
                             GCancellable *cancellable,
                             GError **error);
  GFileEnumerator *(*enumerate) (PhodavPath *path,
                                 const gchar *attributes,
                                 GFileQueryInfoFlags flags,
                                 GCancellable *cancellable,
                                 GError **error);

  gboolean (*mkdir) (PhodavPath *path,
                     GCancellable *cancellable,
                     GError **error);
  gboolean (*unlink) (PhodavPath *path,
                      GCancellable *cancellable,
                      GError **error);
  gboolean (*unlink_child) (PhodavPath *parent,
                            const gchar *name,
                            GCancellable *cancellable,
                            GError **error);
  gboolean (*copy_file) (PhodavPath *from,
                         PhodavPath *to,
                         GFileCopyFlags flags,
                         GCancellable *cancellable,
                         GError **error);
  gboolean (*move) (PhodavPath *from,
                    PhodavPath *to,
                    GFileCopyFlags flags,
                    GCancellable *cancellable,
                    GError **error);

  void (*destroy) (PhodavBackend *backend);
};

struct _PhodavBackend
{
  const PhodavBackendClass *klass;
  gint ref_count; /* atomic: paths share it across worker threads */
  GFile *root;
};

/* selected once per handler, from the root file type */
PhodavBackend *phodav_backend_new (GFile *root);
PhodavBackend *phodav_backend_native_new (GFile *root);
PhodavBackend *phodav_backend_gfile_new (GFile *root);
PhodavBackend *phodav_backend_virtual_new (GFile *root);
PhodavBackend *phodav_backend_ref (PhodavBackend *backend);
void phodav_backend_unref (PhodavBackend *backend);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PhodavBackend, phodav_backend_unref)

/* shared by the implementations */
PhodavPath *phodav_path_new (PhodavBackend *backend,
                             GFile *file,
                             int fd,
                             int parent_fd,
                             const gchar *name);
PhodavPath *phodav_path_new_from_gfile (PhodavBackend *backend,
                                        GFile *file,
                                        PhodavPathMode mode,
                                        gint *status);
gint phodav_path_status_from_errno (gint errsv,
                                    PhodavPathMode mode,
                                    gboolean last);
void phodav_path_set_error_from_errno (GError **error,
                                       gint errsv,
                                       const gchar *what);
GFile *phodav_path_query_file (PhodavPath *path);
int phodav_path_dir_fd (PhodavPath *path);
gboolean phodav_backend_mkdir_at (PhodavPath *path,
                                  GCancellable *cancellable,
                                  GError **error);
gboolean phodav_backend_unlink_at (PhodavPath *path,
                                   GCancellable *cancellable,
                                   GError **error);
gboolean phodav_backend_unlink_child_at (PhodavPath *parent,
                                         const gchar *name,
                                         GCancellable *cancellable,
                                         GError **error);
gboolean phodav_backend_move_at (PhodavPath *from,
                                 PhodavPath *to,
                                 GFileCopyFlags flags,
                                 GCancellable *cancellable,
                                 GError **error);

/* the generic GFile implementation, also used for the real children of
 * a virtual directory */
PhodavPath *phodav_backend_gfile_resolve (PhodavBackend *backend,
                                          PathHandler *handler,
                                          const gchar *relative,
                                          PhodavPathMode mode,
                                          gint *status);
PhodavPath *phodav_backend_gfile_resolve_child (PhodavBackend *backend,
                                                PhodavPath *parent,
                                                const gchar *name,
                                                PhodavPathMode mode,
                                                gint *status);
GFileInfo *phodav_backend_gfile_query_info (PhodavPath *path,
                                            const gchar *attributes,
                                            GFileQueryInfoFlags flags,
                                            GCancellable *cancellable,
                                            GError **error);
GFileInputStream *phodav_backend_gfile_open (PhodavPath *path,
                                             GCancellable *cancellable,
                                             GError **error);
GFileEnumerator *phodav_backend_gfile_enumerate (PhodavPath *path,
                                                 const gchar *attributes,
                                                 GFileQueryInfoFlags flags,
                                                 GCancellable *cancellable,
                                                 GError **error);
gboolean phodav_backend_gfile_mkdir (PhodavPath *path,
                                     GCancellable *cancellable,
                                     GError **error);
gboolean phodav_backend_gfile_unlink (PhodavPath *path,
                                      GCancellable *cancellable,
                                      GError **error);
gboolean phodav_backend_gfile_unlink_child (PhodavPath *parent,
                                            const gchar *name,
                                            GCancellable *cancellable,
                                            GError **error);
gboolean phodav_backend_gfile_copy_file (PhodavPath *from,
                                         PhodavPath *to,
                                         GFileCopyFlags flags,
                                         GCancellable *cancellable,
                                         GError **error);
gboolean phodav_backend_gfile_move (PhodavPath *from,
                                    PhodavPath *to,
                                    GFileCopyFlags flags,
                                    GCancellable *cancellable,
                                    GError **error);

G_END_DECLS
