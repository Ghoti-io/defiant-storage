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
 * @file blob.h
 * @brief Large values, stored once under a caller-supplied hash.
 * @stability free
 *
 * The port does not derive a key and does not hash the bytes. The same hash
 * with the same bytes is the one stored copy. The same hash with different
 * bytes is ::GDEFSTOR_ERR_INVALID and the first body stays.
 */

#ifndef GHOTI_IO_GDEFSTOR_BLOB_H
#define GHOTI_IO_GDEFSTOR_BLOB_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/store.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stage a blob in an open write.
 *
 * @param write The open write.
 * @param hash Caller-supplied hash bytes. Not a hex requirement.
 * @param hash_len Its length. Empty is ::GDEFSTOR_ERR_INVALID.
 * @param bytes The body. May be NULL only when @p len is 0.
 * @param len Body length.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_LIMIT, ::GDEFSTOR_ERR_INVALID,
 *   or ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_blob_put(GDEFSTOR_Write * write,
    const char * hash, size_t hash_len, const uint8_t * bytes, size_t len);

/**
 * @brief Read a committed blob.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param hash The hash the caller stored it under.
 * @param hash_len Its length.
 * @param out_bytes Receives a fresh copy. Empty bodies are a non-NULL
 *   zero-size block.
 * @param out_len Receives the length.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID when absent, or
 *   ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_blob_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * hash,
    size_t hash_len, uint8_t ** out_bytes, size_t * out_len);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_BLOB_H */
