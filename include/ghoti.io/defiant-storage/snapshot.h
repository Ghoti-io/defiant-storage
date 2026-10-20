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
 * A snapshot (AD-23) is a logical export of one tenant, readable without the
 * framework, in two parts while both kinds of storage exist:
 *
 * - **the key/bytes rows** (store.h): members named by their storage keys
 *   (`r:`, `b:`, `p:`, `o:`), as in `defiant-snapshot/1`;
 * - **the entity part** (model.h), on a store with the entity port: the
 *   truth it holds, read through the port - type declarations, entities with
 *   their state history, revisions with who, when and why, every item
 *   version in every language, drafts, records, counters and outbox rows -
 *   as JSON members under `model/` (documentation/formats.md). Derived data
 *   (current tables, the blind table, indexes) is never in it, and neither is
 *   a key: a personal value travels as it is stored, ciphertext and subject.
 *   So a snapshot of one backend imports into any other.
 *
 * A member named `ledger` is a copy of an erasure ledger. It is not a storage
 * key and is not imported into the tenant. Import refuses any other member
 * the manifest does not list with ::GDEFSTOR_ERR_CORRUPT and leaves the
 * tenant unchanged. The same truth always exports to the same bytes.
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

/** Current snapshot format version: 2 added the entity part. A reader
 * accepts every version up to this one (AD-34). */
#define GDEFSTOR_SNAPSHOT_VERSION 2

/**
 * @brief Export one tenant's committed rows to a zip.
 *
 * The key/bytes rows and, on a store with the entity port, the entity part,
 * each as of its last commit. Nothing is written to the store.
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
 * @brief Export one tenant and record when the export was taken.
 *
 * The manifest gains `"taken_ms"`, milliseconds since the Unix epoch. The
 * zip holds no `ledger` member. This is the data set of a backup pair
 * (keystore.h: a restore refuses a key set older than the data set it pairs
 * with); gdefstor_snapshot_taken() reads the time back, and an import does not
 * read it. As gdefstor_snapshot_export() otherwise.
 *
 * @param taken_ms Not negative.
 * @return ::GDEFSTOR_OK or an error; ::GDEFSTOR_ERR_INVALID for a negative
 *   @p taken_ms.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_export_at(
    GDEFSTOR_Store * store, const char * tenant, size_t tenant_len,
    const GDEFSTOR_Limits * limits, int64_t taken_ms, uint8_t ** out_bytes,
    size_t * out_len);

/**
 * @brief Read when a snapshot was taken, as gdefstor_snapshot_export_at()
 * recorded it. Nothing is imported.
 *
 * @param out_found Set to 0, with @p out_taken_ms 0, for a snapshot whose
 *   manifest records no time (gdefstor_snapshot_export() made it).
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_CORRUPT for a zip without a readable
 *   manifest or with a time that is not a non-negative integer, or the
 *   archive's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_taken(GDEFSTOR_Store * store,
    const uint8_t * bytes, size_t len, const GDEFSTOR_Limits * limits,
    int * out_found, int64_t * out_taken_ms);

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
 * Reads `defiant-snapshot/1` and `/2`; a newer version is
 * ::GDEFSTOR_ERR_UNSUPPORTED. A tenant that already holds a key/bytes row or
 * any entity truth (a declaration, an entity, a draft, a blind row, a
 * record, a counter or an outbox row) is ::GDEFSTOR_ERR_INVALID and is left
 * alone. A bad member is ::GDEFSTOR_ERR_CORRUPT and nothing from the archive
 * is kept. An entity part on a store that cannot hold one (no entity port,
 * or a backend without the restore members of model_backend.h) is
 * ::GDEFSTOR_ERR_UNSUPPORTED.
 *
 * The entity part comes back exactly: item ids, revision numbers and their
 * commit sequences, `valid_from` and `valid_to`, order keys, creators, times
 * and languages; counters at their values (the next continues from there);
 * outbox rows with their sequences, leases, failures and parked flags. The
 * current tables are rebuilt as the revisions are written back. Blind rows
 * are not in a snapshot: the caller rebuilds them from decryptable values
 * (AD-23). Entity rows are numbered afresh, in the order they had.
 *
 * Everything is checked before anything is written. The entity part commits
 * in one write; when the snapshot also holds key/bytes rows, they commit in a
 * second write after it (a write holds one kind or the other, model.h), and a
 * failure there leaves the entity part imported.
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

/**
 * @brief As gdefstor_snapshot_import(), and say why when it refuses.
 *
 * On any result but ::GDEFSTOR_OK, @p out_reason receives a sentence in
 * plain words, NUL-terminated (@p out_reason_len leaves the NUL out): the
 * tenant is not empty, the snapshot is a newer format than this build reads
 * (naming both versions), the store cannot hold an entity part, or which
 * member is not what the format holds there. Free it with
 * gdefstor_bytes_free() and the store's allocator. On ::GDEFSTOR_OK it is
 * NULL, and so it is when memory ran out for the sentence itself. A NULL
 * @p out_reason or @p out_reason_len is ::GDEFSTOR_ERR_INVALID.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_import_explain(
    GDEFSTOR_Store * store, const char * tenant, size_t tenant_len,
    const uint8_t * bytes, size_t len, const GDEFSTOR_Limits * limits,
    char ** out_reason, size_t * out_reason_len);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_SNAPSHOT_H */
