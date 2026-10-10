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
 * @file revision.h
 * @brief Immutable revision documents, `defiant-revision`.
 * @stability free
 *
 * A revision is written once. Appending the same entity and sequence again
 * returns ::GDEFSTOR_ERR_INVALID and leaves the first document in place.
 * The port does not hash plaintext and does not invent public ids: a large
 * field is a `{"blob":"<caller-hash>"}` the caller already computed, and
 * `body` is either empty (the body is inline) or the location an archive
 * module stored.
 */

#ifndef GHOTI_IO_GDEFSTOR_REVISION_H
#define GHOTI_IO_GDEFSTOR_REVISION_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/store.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Current revision format name. Writers emit this. */
#define GDEFSTOR_REVISION_NAME "defiant-revision"

/** Current revision format version. Readers accept every lower positive one. */
#define GDEFSTOR_REVISION_VERSION 1

/** A parsed revision. Opaque. Owned by the caller. */
typedef struct GDEFSTOR_Revision GDEFSTOR_Revision;

/**
 * @brief Parse one revision document.
 *
 * On failure nothing is returned for the caller to free. Over a limit is
 * ::GDEFSTOR_ERR_LIMIT and the input is not truncated. An unknown format
 * name, or a version newer than ::GDEFSTOR_REVISION_VERSION, is
 * ::GDEFSTOR_ERR_UNSUPPORTED.
 *
 * @param bytes The document. Required.
 * @param len Its length.
 * @param limits NULL uses the defaults.
 * @param allocator NULL uses the default.
 * @param out_revision Receives the revision on success.
 * @return ::GDEFSTOR_OK or an error. @p out_revision is not written on failure.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_revision_parse(const uint8_t * bytes,
    size_t len, const GDEFSTOR_Limits * limits,
    const GDEFSTOR_Allocator * allocator, GDEFSTOR_Revision ** out_revision);

/**
 * @brief Write a revision as the current format version.
 *
 * @param revision The parsed revision.
 * @param limits NULL uses the defaults. The written size is capped by
 *   `max_document_bytes`.
 * @param allocator NULL uses the default.
 * @param out_bytes Receives a fresh block on success.
 * @param out_len Receives its length.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_LIMIT, ::GDEFSTOR_ERR_OOM, or
 *   ::GDEFSTOR_ERR_INVALID.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_revision_write(
    const GDEFSTOR_Revision * revision, const GDEFSTOR_Limits * limits,
    const GDEFSTOR_Allocator * allocator, uint8_t ** out_bytes,
    size_t * out_len);

/**
 * @brief Release a parsed revision. NULL is ignored.
 *
 * @param revision The revision, or NULL.
 */
GDEFSTOR_API void gdefstor_revision_free(GDEFSTOR_Revision * revision);

/**
 * @brief Append a revision to an open write.
 *
 * The bytes are parsed and stored in the current version. The same
 * `(row, seq)` twice, in this write or already committed, is
 * ::GDEFSTOR_ERR_INVALID and the first document stays. The key is the
 * decimal `row`. The document's `entity` string stays the public id.
 *
 * @param write The open write.
 * @param bytes The document.
 * @param len Its length.
 * @return ::GDEFSTOR_OK or an error. Nothing is staged on failure.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_revision_append(GDEFSTOR_Write * write,
    const uint8_t * bytes, size_t len);

/**
 * @brief Read a committed revision. An open write is not visible.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param entity Decimal row id. Not the document's public id.
 * @param entity_len Its length.
 * @param seq Sequence number from the document.
 * @param out_bytes Receives a fresh copy on success.
 * @param out_len Receives its length.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID when it is absent, or
 *   ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_revision_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * entity,
    size_t entity_len, uint64_t seq, uint8_t ** out_bytes, size_t * out_len);

/** @return The revision's sequence, or 0 when @p revision is NULL. */
GDEFSTOR_API uint64_t gdefstor_revision_seq(const GDEFSTOR_Revision * revision);

/** @return The revision's row id, or 0 when @p revision is NULL. */
GDEFSTOR_API uint64_t gdefstor_revision_row(const GDEFSTOR_Revision * revision);

/**
 * @brief Borrow the entity public id. Valid until the revision is freed.
 *
 * @param revision The revision.
 * @param out_len Receives the length. May be NULL.
 * @return The bytes, or NULL.
 */
GDEFSTOR_API const char * gdefstor_revision_entity(
    const GDEFSTOR_Revision * revision, size_t * out_len);

/**
 * @brief Borrow the `lang` object as JSON. Valid until the revision is freed.
 *
 * @param revision The revision.
 * @param out_len Receives the length. May be NULL.
 * @return The bytes, or NULL.
 */
GDEFSTOR_API const char * gdefstor_revision_lang(
    const GDEFSTOR_Revision * revision, size_t * out_len);

/**
 * @brief Borrow the body location. Empty means the body is inline.
 *
 * @param revision The revision.
 * @param out_len Receives the length. May be NULL.
 * @return The bytes, or NULL.
 */
GDEFSTOR_API const char * gdefstor_revision_body(
    const GDEFSTOR_Revision * revision, size_t * out_len);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_REVISION_H */
