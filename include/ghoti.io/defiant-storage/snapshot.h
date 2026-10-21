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
 * @brief A tenant snapshot: a zip whose manifest is `defiant-snapshot/1`.
 * @stability free
 *
 * A snapshot (AD-23) is a logical export of one tenant's truth, readable
 * without the framework: what the entity port holds, read through the port -
 * type declarations, entities with their state history, revisions with who,
 * when and why, every item version in every language, drafts, records,
 * counters and outbox rows - as JSON members under `model/`
 * (documentation/formats.md). Each entity's member says its own format,
 * `defiant-revision/1`. Derived data (current tables, the blind table,
 * indexes) is never in it, and neither is a key: a personal value travels as
 * it is stored, ciphertext and subject. So a snapshot of one backend imports
 * into any other. The data set of a backup pair
 * (gdefstor_snapshot_export_tenant()) also says when it was taken and what
 * the tenant needs to come back up: its edition and the components whose
 * data it holds (core and its modules) with the schema version each one's
 * current data is in. Every revision, draft and record carries the schema
 * stamp it was written with (model.h), and a snapshot carries each exactly.
 *
 * A member named `ledger` is a copy of an erasure ledger. It is not imported
 * into the tenant. Import refuses any other member the manifest does not
 * list with ::GDEFSTOR_ERR_CORRUPT and leaves the tenant unchanged. The same
 * truth always exports to the same bytes.
 *
 * Every format here is at version 1, and a reader refuses every other
 * version (AD-34: until the suite's first release a format is replaced in
 * place and stays at 1).
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

/** The snapshot format's version, and the only one a reader accepts. */
#define GDEFSTOR_SNAPSHOT_VERSION 1

/** The format of one entity's revisions and item versions, a member of a
 * snapshot (AD-34). */
#define GDEFSTOR_REVISION_NAME "defiant-revision"

/** Its version, and the only one a reader accepts. */
#define GDEFSTOR_REVISION_VERSION 1

/**
 * @brief Export one tenant's truth to a zip.
 *
 * The entity part, as of the last commit. Nothing is written to the store.
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
 * The ledger is not part of the tenant's truth. An empty copy is
 * @p ledger_len 0.
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

/** The edition identifier's name (AD-18, AD-34): an edition is written
 * `defiant-edition/<n>`, n a positive integer without leading zeros. */
#define GDEFSTOR_EDITION_NAME "defiant-edition"

/** The longest module name a data set holds, in bytes. */
#define GDEFSTOR_SNAPSHOT_MODULE_NAME_MAX 64

/** The most modules a data set names. */
#define GDEFSTOR_SNAPSHOT_MODULES_MAX 1024

/**
 * @brief One component a data set names (AD-23): core (`core`) or a module
 * whose data the tenant holds, and the version of the stored-data schema its
 * current data is in (AD-15).
 *
 * The name is a module's machine name: a lowercase letter, then lowercase
 * letters, digits and hyphens, at most ::GDEFSTOR_SNAPSHOT_MODULE_NAME_MAX
 * bytes. @ref schema is 1 or more.
 */
typedef struct GDEFSTOR_SnapshotModule {
  const char * name; ///< The module's machine name.
  size_t name_len;   ///< Its length.
  uint32_t schema;   ///< Its stored-data schema version, 1 or more.
} GDEFSTOR_SnapshotModule;

/**
 * @brief Export one tenant as the data set of a backup pair: when it was
 * taken, and what the tenant needs to come back up (AD-23).
 *
 * As gdefstor_snapshot_export_at(), and the manifest also says the tenant's
 * edition (`"edition"`, `defiant-edition/<n>`, or null for a tenant that pins
 * none) and the components whose data it holds with the schema version of
 * each one's current data (`"modules"`, by name). Storage does not read the
 * tenant's records itself: the caller (core) names the components. Nothing is
 * written to the store.
 *
 * @param edition `defiant-edition/<n>`, or NULL with @p edition_len 0 for
 *   none.
 * @param modules The modules in strictly ascending byte order of name, at
 *   most ::GDEFSTOR_SNAPSHOT_MODULES_MAX; NULL when @p module_count is 0.
 * @return ::GDEFSTOR_OK or an error; ::GDEFSTOR_ERR_INVALID for a negative
 *   @p taken_ms, an edition that is not `defiant-edition/<n>`, a module name
 *   that is not a machine name, a schema of 0, or modules out of order or
 *   named twice.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_export_tenant(
    GDEFSTOR_Store * store, const char * tenant, size_t tenant_len,
    const GDEFSTOR_Limits * limits, int64_t taken_ms, const char * edition,
    size_t edition_len, const GDEFSTOR_SnapshotModule * modules,
    size_t module_count, uint8_t ** out_bytes, size_t * out_len);

/** What a data set's manifest says the tenant needs. Opaque. */
typedef struct GDEFSTOR_SnapshotAbout GDEFSTOR_SnapshotAbout;

/**
 * @brief Read what a snapshot's manifest says the tenant needs to come back
 * up (gdefstor_snapshot_export_tenant()). Nothing is imported.
 *
 * A manifest that holds neither `"edition"` nor `"modules"` (one that
 * gdefstor_snapshot_export() or gdefstor_snapshot_export_at() made) reads as
 * not found. Free @p out_about with gdefstor_snapshot_about_free().
 *
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_CORRUPT for a zip without a readable
 *   manifest, one that holds one of the two and not the other, or either not
 *   as gdefstor_snapshot_export_tenant() writes it (an edition that is not
 *   `defiant-edition/<n>`, a module that is not `{"name":…,"schema":…}` with a
 *   machine name and a schema of 1 or more, modules out of order or named
 *   twice, or more than ::GDEFSTOR_SNAPSHOT_MODULES_MAX), or the archive's
 *   error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_about(GDEFSTOR_Store * store,
    const uint8_t * bytes, size_t len, const GDEFSTOR_Limits * limits,
    GDEFSTOR_SnapshotAbout ** out_about);

/** @return 1 when the manifest says the edition and the modules, else 0. */
GDEFSTOR_API int gdefstor_snapshot_about_found(
    const GDEFSTOR_SnapshotAbout * about);

/**
 * @brief The edition the manifest names: `defiant-edition/<n>`, or @p out_len
 * 0 for none. Borrowed until @p about is freed.
 */
GDEFSTOR_API void gdefstor_snapshot_about_edition(
    const GDEFSTOR_SnapshotAbout * about, const char ** out_edition,
    size_t * out_len);

/** @return How many modules the manifest names. */
GDEFSTOR_API size_t gdefstor_snapshot_about_module_count(
    const GDEFSTOR_SnapshotAbout * about);

/**
 * @brief Module @p index, in ascending byte order of name. The name is
 * borrowed until @p about is freed.
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID past the end.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_snapshot_about_module(
    const GDEFSTOR_SnapshotAbout * about, size_t index,
    GDEFSTOR_SnapshotModule * out);

/** @brief Release what gdefstor_snapshot_about() read. NULL is ignored. */
GDEFSTOR_API void gdefstor_snapshot_about_free(GDEFSTOR_SnapshotAbout * about);

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
 * @brief Import a snapshot into a tenant that holds nothing.
 *
 * Reads `defiant-snapshot/1`; any other version, of the snapshot or of a
 * member (an entity's `defiant-revision/1`), is ::GDEFSTOR_ERR_UNSUPPORTED.
 * The edition and modules a data set's manifest names are checked as
 * gdefstor_snapshot_about() checks them and not acted on: deciding whether
 * this installation can hold them is the caller's (core's restore). A
 * tenant that already holds any entity truth (a declaration, an entity, a
 * draft, a blind row, a record, a counter or an outbox row) is
 * ::GDEFSTOR_ERR_INVALID and is left alone. A bad member is
 * ::GDEFSTOR_ERR_CORRUPT and nothing from the archive is kept. An entity part
 * on a store whose backend cannot hold one (without the restore members of
 * model_backend.h) is ::GDEFSTOR_ERR_UNSUPPORTED.
 *
 * The entity part comes back exactly: item ids, revision numbers and their
 * commit sequences, `valid_from` and `valid_to`, order keys, creators, times
 * and languages; counters at their values (the next continues from there);
 * outbox rows with their sequences, leases, failures and parked flags. The
 * current tables are rebuilt as the revisions are written back. Blind rows
 * are not in a snapshot: the caller rebuilds them from decryptable values
 * (AD-23). Entity rows are numbered afresh, in the order they had.
 *
 * Everything is checked before anything is written, and the whole commits in
 * one write.
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
 * tenant is not empty, the snapshot or a member is at another version than
 * this build reads (naming the versions), the store cannot hold an entity
 * part, or which member is not what the format holds there. Free it with
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
