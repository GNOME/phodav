/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include "phodav-backend.h"
#include "phodav-priv.h"

G_BEGIN_DECLS

/* the state of a PhodavServer, shared only by the server, lock store and
 * request dispatch implementations; the method files see it as opaque */
struct _PhodavServer
{
  GObject parent;
  SoupServer *server;
  GCancellable *cancellable;
  GFile *root_file;
  gchar *base_path;
  PathHandler *root_handler; /* borrowed, owned by the soup handler registration */
  gchar *handler_path;       /* the path the handler is registered at */
  GHashTable *paths;
  gboolean readonly;
  guint64 max_upload;
  gboolean constructing;
  gboolean stopped;
  GMainContext *expire_locks_context;
  GSource *expire_locks_source;

  /* the private, bounded pools the DAV methods and the response
   * producers run on; keeping them apart lets a download keep making
   * progress while the request workers wait for the filesystem lock */
  GThreadPool *pool;
  GThreadPool *stream_pool;

  /* protects the lock table during a lookup or update. It never covers
   * a filesystem operation: the value-returning lock store API only
   * exposes owned decisions and XML nodes. */
  GMutex lock_store_mutex;
  /* serializes the filesystem operations: mutations take it for
   * writing, while reads may share it */
  GRWLock fs_lock;
};

struct _PathHandler
{
  PhodavServer *self; /* borrowed, kept alive by the request */
  GFile *file;
  PhodavBackend *backend; /* selected from @file, owns the resolution policy */
  gchar *base_path;       /* immutable snapshot of the server base path */
  gint ref_count;
};

/* the lock store reports its next timeout so the server can arm a
 * one-shot source for it instead of polling every second */
gint64 phodav_lock_store_next_expiry (PhodavServer *self);
void phodav_server_schedule_lock_expiry (PhodavServer *self);

G_END_DECLS
