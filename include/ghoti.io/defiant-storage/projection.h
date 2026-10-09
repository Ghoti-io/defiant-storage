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
 * @file projection.h
 * @brief Opaque projection rows. The caller names the columns.
 * @stability free
 *
 * Which columns an entity type has is not decided here. A row is replaced
 * as a whole by a later put of the same type and entity.
 */

#ifndef GHOTI_IO_GDEFSTOR_PROJECTION_H
#define GHOTI_IO_GDEFSTOR_PROJECTION_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/store.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One column. The name and value are borrowed for the call that
 * takes the struct.
 */
typedef struct GDEFSTOR_Column {
  const char * name;       ///< Required.
  size_t name_len;         ///< Length of @ref name.
  const uint8_t * value;   ///< May be NULL when @ref value_len is 0.
  size_t value_len;        ///< Length of @ref value.
} GDEFSTOR_Column;

/** A row read back. Opaque. */
typedef struct GDEFSTOR_Projection GDEFSTOR_Projection;

/**
 * @brief Stage a projection row.
 *
 * @param write The open write.
 * @param type Entity type, caller-supplied.
 * @param type_len Its length.
 * @param entity Public id.
 * @param entity_len Its length.
 * @param columns The columns. NULL only when @p column_count is 0.
 * @param column_count How many.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_LIMIT, ::GDEFSTOR_ERR_INVALID,
 *   or ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_projection_put(GDEFSTOR_Write * write,
    const char * type, size_t type_len, const char * entity,
    size_t entity_len, const GDEFSTOR_Column * columns, size_t column_count);

/**
 * @brief Read a committed projection row.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param type Entity type.
 * @param type_len Its length.
 * @param entity Public id.
 * @param entity_len Its length.
 * @param out_row Receives the row on success.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID when absent, or
 *   ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_projection_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type,
    size_t type_len, const char * entity, size_t entity_len,
    GDEFSTOR_Projection ** out_row);

/** @return How many columns, or 0. */
GDEFSTOR_API size_t gdefstor_projection_column_count(
    const GDEFSTOR_Projection * row);

/**
 * @brief Borrow one column. Valid until the row is freed.
 *
 * @param row The row.
 * @param index Zero-based.
 * @param out_column Receives pointers into the row.
 * @return ::GDEFSTOR_OK or ::GDEFSTOR_ERR_INVALID.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_projection_column(
    const GDEFSTOR_Projection * row, size_t index,
    GDEFSTOR_Column * out_column);

/** @brief Release a row. NULL is ignored. */
GDEFSTOR_API void gdefstor_projection_free(GDEFSTOR_Projection * row);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_PROJECTION_H */
