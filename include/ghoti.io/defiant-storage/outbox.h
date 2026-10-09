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
 * @file outbox.h
 * @brief Outbox rows and the in-process runner.
 * @stability free
 *
 * A row is inserted in the same write as the change it describes. The
 * runner is a function the caller invokes after commit: it claims due rows
 * with an expiring lease, appends `defiant-outbox` JSON lines to a
 * caller-supplied sink, and parks a row whose sink fails. Delivery is
 * at-least-once. The log is not part of the write. There is no scheduler
 * and no job type here.
 */

#ifndef GHOTI_IO_GDEFSTOR_OUTBOX_H
#define GHOTI_IO_GDEFSTOR_OUTBOX_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/store.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Current outbox format name. */
#define GDEFSTOR_OUTBOX_NAME "defiant-outbox"

/** Current outbox format version. */
#define GDEFSTOR_OUTBOX_VERSION 1

/** One claimed or loaded row. Opaque. */
typedef struct GDEFSTOR_Outbox_Row GDEFSTOR_Outbox_Row;

/**
 * @brief Where a delivered line is appended. The only sink is this callback:
 * an append-only log the caller owns.
 */
typedef struct GDEFSTOR_Sink {
  GDEFSTOR_Result (*append)(void * ctx, const uint8_t * line, size_t len);
  void * ctx;
} GDEFSTOR_Sink;

/**
 * @brief Stage an outbox row in the open write.
 *
 * The payload is one JSON value. The port assigns the per-stream sequence
 * when the write commits. A public id already staged or committed is
 * ::GDEFSTOR_ERR_INVALID.
 *
 * @param write The open write.
 * @param id Caller-supplied public id.
 * @param id_len Its length.
 * @param stream Stream name.
 * @param stream_len Its length.
 * @param payload JSON value bytes.
 * @param payload_len Their length.
 * @return ::GDEFSTOR_OK or an error. Nothing is staged on failure.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_outbox_append(GDEFSTOR_Write * write,
    const char * id, size_t id_len, const char * stream, size_t stream_len,
    const uint8_t * payload, size_t payload_len);

/**
 * @brief Read a committed row by public id, parked or not.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param id Public id.
 * @param id_len Its length.
 * @param out_row Receives the row on success.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID when absent, or
 *   ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_outbox_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * id, size_t id_len,
    GDEFSTOR_Outbox_Row ** out_row);

/**
 * @brief Claim the next due row in a stream.
 *
 * Due means not parked and the previous lease has expired. The claim's
 * lease lasts @p lease_ms of the caller's clock. Nothing due is
 * ::GDEFSTOR_ERR_INVALID.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param stream Stream name.
 * @param stream_len Its length.
 * @param lease_ms Lease length. Negative is ::GDEFSTOR_ERR_INVALID.
 * @param clock Required.
 * @param out_row Receives the row on success.
 * @return ::GDEFSTOR_OK or an error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_outbox_claim(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * stream,
    size_t stream_len, int64_t lease_ms, const GDEFSTOR_Clock * clock,
    GDEFSTOR_Outbox_Row ** out_row);

/**
 * @brief Delete a delivered row.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param id Public id.
 * @param id_len Its length.
 * @return ::GDEFSTOR_OK or ::GDEFSTOR_ERR_INVALID when absent.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_outbox_ack(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * id, size_t id_len);

/**
 * @brief Park a row so later claims skip it. The row is not deleted.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param id Public id.
 * @param id_len Its length.
 * @return ::GDEFSTOR_OK or ::GDEFSTOR_ERR_INVALID when absent.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_outbox_fail(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * id, size_t id_len);

/**
 * @brief Claim due rows, append a JSON line each, and ack.
 *
 * A sink error parks that row and stops. Earlier rows in the call that
 * were acked stay acked. The line is `defiant-outbox/<version>` plus a
 * trailing newline.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param stream Stream name.
 * @param stream_len Its length.
 * @param lease_ms Lease for each claim.
 * @param clock Required.
 * @param sink Required.
 * @param limits NULL uses the store's limits.
 * @return ::GDEFSTOR_OK, or the sink's error after the row is parked.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_outbox_run(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * stream,
    size_t stream_len, int64_t lease_ms, const GDEFSTOR_Clock * clock,
    const GDEFSTOR_Sink * sink, const GDEFSTOR_Limits * limits);

/** @return 1 when the row is parked, 0 otherwise. */
GDEFSTOR_API int gdefstor_outbox_row_parked(const GDEFSTOR_Outbox_Row * row);

/** @return The stream sequence, or 0. */
GDEFSTOR_API uint64_t gdefstor_outbox_row_seq(const GDEFSTOR_Outbox_Row * row);

/**
 * @brief Borrow the payload JSON. Valid until the row is freed.
 *
 * @param row The row.
 * @param out_len Receives the length. May be NULL.
 * @return The bytes, or NULL.
 */
GDEFSTOR_API const uint8_t * gdefstor_outbox_row_payload(
    const GDEFSTOR_Outbox_Row * row, size_t * out_len);

/** @brief Release a row. NULL is ignored. */
GDEFSTOR_API void gdefstor_outbox_row_free(GDEFSTOR_Outbox_Row * row);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_OUTBOX_H */
