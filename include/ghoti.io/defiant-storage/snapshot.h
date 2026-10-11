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
 * @file snapshot.h
 * @brief A tenant snapshot: a zip whose manifest is `defiant-snapshot`.
 * @stability free
 *
 * Member names are storage keys, except a member named `ledger`. That
 * member is a copy of an erasure ledger. It is not a storage key, it is
 * not imported into the tenant, and it is not one of `r:`, `b:`, `p:` or
 * `o:`. Import refuses any other unexpected member with
 * ::GDEFSTOR_ERR_CORRUPT and leaves the tenant unchanged.
 */

#ifndef GHOTI_IO_GDEFSTOR_SNAPSHOT_H
#define GHOTI_IO_GDEFSTOR_SNAPSHOT_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/store.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Current snapshot format name. */
#define GDEFSTOR_SNAPSHOT_NAME "defiant-snapshot"

/** Current snapshot format version. */
#define GDEFSTOR_SNAPSHOT_VERSION 1

/**
 * @brief Export one tenant's committed rows to a zip.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param limits NULL uses the store's limits.
 * @param out_bytes Receives the zip on success.
 * @param out_len Receives its length.
 * @return ::GDEFSTOR_OK or an error. Nothing is returned on failure.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_export(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    uint8_t ** out_bytes, size_t * out_len);

/**
 * @brief Export one tenant and attach @p ledger as the `ledger` member.
 *
 * The ledger is not a storage key. An empty copy is @p ledger_len 0.
 * @p ledger may be NULL only then.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_export_ledger(
    GDEFSTOR_Store * store, const char * tenant, size_t tenant_len,
    const GDEFSTOR_Limits * limits, const uint8_t * ledger, size_t ledger_len,
    uint8_t ** out_bytes, size_t * out_len);

/**
 * @brief Copy the `ledger` member. A snapshot without one yields length 0.
 *
 * The copy is allocated with the default allocator, not the store's.
 * Free @p out_bytes with gdefstor_bytes_free() and a NULL allocator.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_ledger(GDEFSTOR_Store * store,
    const uint8_t * bytes, size_t len, const GDEFSTOR_Limits * limits,
    uint8_t ** out_bytes, size_t * out_len);

/**
 * @brief Import a snapshot into a tenant that has no rows.
 *
 * A tenant that already has a row is ::GDEFSTOR_ERR_INVALID and is left
 * alone. A bad member is ::GDEFSTOR_ERR_CORRUPT and nothing from the
 * archive is kept.
 *
 * @param store The store.
 * @param tenant Tenant the rows are imported into. Not read from the zip.
 * @param tenant_len Its length.
 * @param bytes The zip.
 * @param len Its length.
 * @param limits NULL uses the store's limits.
 * @return ::GDEFSTOR_OK or an error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_import(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const uint8_t * bytes, size_t len,
    const GDEFSTOR_Limits * limits);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_SNAPSHOT_H */
