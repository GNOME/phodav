/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include "phodav-backend.h"
#include "phodav-priv.h"

G_BEGIN_DECLS

/*
 * Concurrency invariants
 * ======================
 *
 * These are the rules the request, method and lock-store code rely on.
 * Keep them in this one place when changing the model.
 *
 * 1. The main context owns every request. Request creation, admission,
 *    method validate and commit, response application, completion,
 *    cancellation and teardown all run there. Workers never touch a
 *    SoupServerMessage.
 *
 * 2. The namespace is a main-context FIFO for mutating requests. A
 *    mutating method holds it from validate through work completion and
 *    commit; the driver releases it exactly once in finalize. GET, HEAD
 *    and PROPFIND never acquire it. A streamed PUT stages its body
 *    outside the namespace and acquires it only for the final lock
 *    check and rename.
 *
 * 3. Blocking filesystem work runs only in one-shot jobs. Mutations use
 *    the single-threaded namespace pool, preserving their order. Reads,
 *    stream pulls and PUT staging use the parallel stream pool. A job
 *    may read immutable request fields, but its completion returns to
 *    the main context before any request state changes or response is
 *    written.
 *
 * 4. Method data is owned by the request driver. Only one work job may
 *    use it at a time; the driver waits for that job before commit or
 *    finalize. The driver frees it exactly once, after releasing the
 *    namespace and before completing the request.
 *
 * 5. The lock store is protected solely by @lock_store_mutex. Every
 *    public lock-store operation takes the mutex, whether it runs on
 *    the main context or in a PROPFIND worker. There is no
 *    main-context-only lock-store rule; the namespace is for filesystem
 *    mutations, not for protecting the lock trie.
 *
 * 6. Lock expiry is driven by the expiry min-heap. An operation first
 *    pops every elapsed lock from the heap, then reads the trie. Expiry
 *    never walks the trie to find expired locks; it only prunes a path
 *    once that path's last lock has been removed. The timeout source
 *    also runs on the main context and waits for the earliest heap
 *    deadline.
 *
 * 7. The message owns a reference to its request until request_detach().
 *    Streams hold their own request reference. The method driver holds
 *    a reference across each worker job, so method_data and request
 *    fields stay alive until the completion runs.
 */

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
