/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include "phodav-backend.h"
#include "phodav-priv.h"

G_BEGIN_DECLS

typedef struct _PhodavPathTrie PhodavPathTrie;

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
  PhodavPathTrie *paths;
  gboolean readonly;
  guint64 max_upload;
  gboolean constructing;
  gboolean stopped;
  GMainContext *expire_locks_context;
  GSource *expire_locks_source;

  /* The namespace is serialised on the main context: one mutating
   * request at a time may inspect or change it. Blocking filesystem
   * work runs in a one-shot job on @namespace_pool, while every
   * decision and commit stays here. The completion of a job is
   * delivered on the main context, which then releases the namespace
   * to the next waiter. Reads run on @stream_pool beside the
   * namespace, and do not take it. */
  GThreadPool *namespace_pool;
  GThreadPool *stream_pool;
  GQueue *namespace_waiters;
  gboolean namespace_busy;

  /* protects the path trie from the read-only PROPFIND job, which
   * walks it beside the namespace to report the locks of the listed
   * resources */
  GMutex lock_store_mutex;
};

struct _PathHandler
{
  PhodavServer *self; /* borrowed, kept alive by the request */
  GFile *file;
  PhodavBackend *backend; /* selected from @file, owns the resolution policy */
  gchar *base_path;       /* immutable snapshot of the server base path */
  gint ref_count;
};

/* the path trie which owns the lock store entries */
PhodavPathTrie *phodav_lock_store_paths_new (void);
void phodav_lock_store_paths_free (PhodavPathTrie *paths);

/* the lock store reports its next timeout so the server can arm a
 * one-shot source for it instead of polling every second */
gint64 phodav_lock_store_next_expiry (PhodavServer *self);
void phodav_server_schedule_lock_expiry (PhodavServer *self);

G_END_DECLS
