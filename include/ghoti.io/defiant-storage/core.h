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
 * @file core.h
 * @brief Result codes and limits.
 * @stability free
 *
 * Formats are specified in documentation/formats.md.
 */

#ifndef GHOTI_IO_GDEFSTOR_CORE_H
#define GHOTI_IO_GDEFSTOR_CORE_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/allocator.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a storage call.
 *
 * `GDEFSTOR_ERR_BUSY` and `GDEFSTOR_ERR_CONFLICT` are the codes past the
 * suite's common list: the writer queue parks a fiber, and a caller that is
 * not a fiber must not block its thread; and a write built on a revision that
 * is no longer the newest is refused whole (model.h), which a caller must tell
 * apart from a wrong argument to offer a merge.
 */
typedef enum GDEFSTOR_Result {
  GDEFSTOR_OK = 0,          ///< Succeeded.
  GDEFSTOR_ERR_IO,          ///< A stream or sink reported an error.
  GDEFSTOR_ERR_FORMAT,      ///< Well formed input that is not this format.
  GDEFSTOR_ERR_UNSUPPORTED, ///< Unknown format name, or a newer version.
  GDEFSTOR_ERR_LIMIT,       ///< A limit was exceeded. Nothing is truncated.
  GDEFSTOR_ERR_CORRUPT,     ///< This format, but the bytes are wrong.
  GDEFSTOR_ERR_OOM,         ///< The allocator returned NULL.
  GDEFSTOR_ERR_INVALID,     ///< A caller argument is wrong, or a missing row.
  GDEFSTOR_ERR_INTERNAL,    ///< A library invariant failed.
  GDEFSTOR_ERR_BUSY,        ///< The tenant's writer is busy and this is not a fiber.
  GDEFSTOR_ERR_CONFLICT,    ///< A write named a base revision that is no longer the newest.
  GDEFSTOR_RESULT_COUNT     ///< Not a result. Closes the string table.
} GDEFSTOR_Result;

/**
 * @brief A static description of a result. Never NULL.
 *
 * @param result The code, including out-of-range values.
 * @return A string the caller does not free.
 */
GDEFSTOR_API const char * gdefstor_result_string(GDEFSTOR_Result result);

/**
 * @brief Caps on every unbounded quantity the port parses or stores.
 *
 * A NULL pointer where a limits argument is accepted means these defaults.
 * Zero is a real cap, not a synonym for the default: a caller who wants the
 * defaults passes NULL or calls gdefstor_limits_default().
 */
typedef struct GDEFSTOR_Limits {
  size_t max_member_bytes;        ///< One snapshot member's declared size.
  size_t max_members;             ///< Snapshot members, manifest included.
  size_t max_snapshot_bytes;      ///< Sum of snapshot member sizes.
  uint32_t max_zip_ratio;         ///< Uncompressed / compressed, integer.
  size_t max_public_id_bytes;     ///< A caller-supplied public id.
  size_t max_tenant_bytes;        ///< The tenant argument.
  size_t max_type_bytes;          ///< An entity type's machine name.
  size_t max_json_depth;          ///< JSON nesting of a snapshot's members.
} GDEFSTOR_Limits;

/**
 * @brief Fill @p limits with the defaults. NULL is ignored.
 *
 * @param limits Caller-owned structure.
 */
GDEFSTOR_API void gdefstor_limits_default(GDEFSTOR_Limits * limits);

/**
 * @brief Release bytes the port returned (a record, a dump, a snapshot).
 *
 * @param allocator The allocator the store was created with, or NULL for
 *   the default. It must be the one that allocated @p bytes.
 * @param bytes The block, or NULL.
 */
GDEFSTOR_API void gdefstor_bytes_free(const GDEFSTOR_Allocator * allocator,
    void * bytes);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_CORE_H */
