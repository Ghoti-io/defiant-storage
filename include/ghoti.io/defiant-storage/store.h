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
 * @brief The store handle and the writer queue.
 * @stability free
 *
 * A store is opened over an entity backend (model.h, model_backend.h) with
 * gdefstor_store_open(). A read sees the last commit. Writes to one tenant
 * are serializable: a second writer on a fiber parks until the first commits
 * or aborts, and a caller that is not a fiber gets ::GDEFSTOR_ERR_BUSY. The
 * tenant is an argument on every call. One store is used from one thread at
 * a time.
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
 * @param write The write. Consumed on success and on failure (the backend's
 *   transaction is aborted then); not reused either way.
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
