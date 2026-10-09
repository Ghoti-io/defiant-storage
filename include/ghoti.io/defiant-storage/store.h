/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Defiant Storage.
 *
 * Ghoti.io Defiant Storage is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Defiant Storage is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file store.h
 * @brief The store handle, the writer queue and the backend vtable.
 * @stability free
 *
 * A read sees the last commit. Writes to one tenant are serializable: a
 * second writer on a fiber parks until the first commits or aborts, and a
 * caller that is not a fiber gets ::GDEFSTOR_ERR_BUSY. The tenant is an
 * argument on every call. One store is used from one thread at a time.
 */

#ifndef GHOTI_IO_GDEFSTOR_STORE_H
#define GHOTI_IO_GDEFSTOR_STORE_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/core.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** An open store. Opaque. */
typedef struct GDEFSTOR_Store GDEFSTOR_Store;

/** An uncommitted write. Opaque. One tenant. */
typedef struct GDEFSTOR_Write GDEFSTOR_Write;

/**
 * @brief What a backend stores: committed bytes under a tenant and a key.
 *
 * The port keeps uncommitted writes and the writer queue. A backend receives
 * a key when the port commits. Keys are storage keys, not filesystem paths.
 * Function pointers through `list` are required. `begin`, `commit` and
 * `abort` are optional and come as a set: when `begin` is set, one commit is
 * one transaction and a failure aborts it instead of removing keys. A
 * backend zeroes the struct before filling it, so those three stay NULL
 * when a backend has no transaction.
 *
 * `get` writes a fresh block the port frees with the store allocator, or
 * returns ::GDEFSTOR_ERR_INVALID when the key is absent. `list` writes a
 * freshly allocated array of freshly allocated keys whose prefix matches.
 */
typedef struct GDEFSTOR_Backend {
  void * ctx; ///< Passed to every function. May be NULL.
  void (*destroy)(void * ctx);
  GDEFSTOR_Result (*put)(void * ctx, const char * tenant, size_t tenant_len,
      const char * key, size_t key_len, const uint8_t * bytes, size_t len);
  GDEFSTOR_Result (*get)(void * ctx, const char * tenant, size_t tenant_len,
      const char * key, size_t key_len, uint8_t ** out_bytes, size_t * out_len);
  GDEFSTOR_Result (*remove)(void * ctx, const char * tenant,
      size_t tenant_len, const char * key, size_t key_len);
  GDEFSTOR_Result (*list)(void * ctx, const char * tenant, size_t tenant_len,
      const char * prefix, size_t prefix_len, char *** out_keys,
      size_t ** out_key_lens, size_t * out_count);
  /**
   * Start one transaction for @p tenant. NULL when each `put` is its own
   * commit. `commit` and `abort` are then also NULL.
   */
  GDEFSTOR_Result (*begin)(void * ctx, const char * tenant, size_t tenant_len);
  /** Finish the transaction started by `begin`. */
  GDEFSTOR_Result (*commit)(void * ctx);
  /** Drop the transaction started by `begin`. The previous bytes remain. */
  GDEFSTOR_Result (*abort)(void * ctx);
} GDEFSTOR_Backend;

/**
 * @brief Open a store over a backend.
 *
 * The backend is copied. Its `ctx` is borrowed until
 * gdefstor_store_destroy(), which calls `destroy`.
 *
 * @param backend Required.
 * @param allocator NULL uses the default.
 * @param limits NULL uses gdefstor_limits_default().
 * @param out_store Receives the store on success. Not written on failure.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, or ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_store_create(
    const GDEFSTOR_Backend * backend, const GDEFSTOR_Allocator * allocator,
    const GDEFSTOR_Limits * limits, GDEFSTOR_Store ** out_store);

/**
 * @brief Close a store and destroy the backend. NULL is ignored.
 *
 * An open write is aborted first, so nothing uncommitted is published.
 *
 * @param store The store, or NULL.
 */
GDEFSTOR_API void gdefstor_store_destroy(GDEFSTOR_Store * store);

/**
 * @brief Begin a write for one tenant.
 *
 * If this tenant already has an open write, a fiber parks until that write
 * ends and then proceeds. Any other caller gets ::GDEFSTOR_ERR_BUSY.
 *
 * @param store The store.
 * @param tenant Tenant bytes. Not connection state. Required, non-empty.
 * @param tenant_len Length of @p tenant.
 * @param limits NULL uses the store's limits.
 * @param out_write Receives the write on success.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_BUSY, ::GDEFSTOR_ERR_INVALID,
 *   ::GDEFSTOR_ERR_LIMIT, or ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_write_begin(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Write ** out_write);

/**
 * @brief Publish every change in the write, then wake the next writer.
 *
 * @param write The write. Consumed on success and on failure after a
 *   partial apply has been rolled back; not reused either way.
 * @return ::GDEFSTOR_OK, or the backend's error. On failure the commit is
 *   not visible.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_write_commit(GDEFSTOR_Write * write);

/**
 * @brief Drop every change in the write, then wake the next writer.
 *
 * @param write The write, or NULL. Consumed.
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_write_abort(GDEFSTOR_Write * write);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_STORE_H */
