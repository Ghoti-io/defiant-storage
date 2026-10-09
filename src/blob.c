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
 * @file
 *
 * Blobs addressed by a hash the caller already computed.
 */

#include "internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static int same_bytes(const uint8_t * a, size_t a_len, const uint8_t * b,
    size_t b_len) {
  if (a_len != b_len) {
    return 0;
  }
  if (a_len == 0) {
    return 1;
  }
  return memcmp(a, b, a_len) == 0;
}

GDEFSTOR_Result gdefstor_blob_put(GDEFSTOR_Write * write, const char * hash,
    size_t hash_len, const uint8_t * bytes, size_t len) {
  GDEFSTOR_Pending * row;
  uint8_t * existing = NULL;
  size_t existing_len = 0;
  size_t count = 0;
  GDEFSTOR_Result rc;
  if (write == NULL || !write->alive ||
      !gdefstor_span_ok(hash, hash_len, write->limits.max_hash_bytes) ||
      (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (len > write->limits.max_blob_bytes) {
    return GDEFSTOR_ERR_LIMIT;
  }
  row = (GDEFSTOR_Pending *)gdefstor_mem0(write->store->alloc, sizeof *row);
  if (row == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  row->kind = GDEFSTOR_KIND_BLOB;
  row->encoded = 1;
  rc = gdefstor_key_hex(write->store->alloc, "b:", hash, hash_len, NULL, 0, 0,
      &row->key, &row->key_len);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
    return rc;
  }
  rc = gdefstor_lookup(write->store, write->tenant, write->tenant_len, row->key,
      row->key_len, 1, &existing, &existing_len);
  if (rc == GDEFSTOR_OK) {
    int same = same_bytes(existing, existing_len, bytes, len);
    gcu_allocator_free(write->store->alloc, existing);
    gdefstor_free_pending(write->store->alloc, row);
    return same ? GDEFSTOR_OK : GDEFSTOR_ERR_INVALID;
  }
  if (rc != GDEFSTOR_ERR_INVALID) {
    gdefstor_free_pending(write->store->alloc, row);
    return rc;
  }
  rc = gdefstor_count(write->store, write, "b:", 2, &count);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
    return rc;
  }
  if (count >= write->limits.max_blobs) {
    gdefstor_free_pending(write->store->alloc, row);
    return GDEFSTOR_ERR_LIMIT;
  }
  row->bytes = (uint8_t *)gdefstor_mem(write->store->alloc, len);
  if (row->bytes == NULL) {
    gdefstor_free_pending(write->store->alloc, row);
    return GDEFSTOR_ERR_OOM;
  }
  if (len > 0) {
    memcpy(row->bytes, bytes, len);
  }
  row->len = len;
  rc = gdefstor_stage(write, row);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
  }
  return rc;
}

GDEFSTOR_Result gdefstor_blob_get(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * hash, size_t hash_len, uint8_t ** out_bytes,
    size_t * out_len) {
  char * key = NULL;
  size_t key_len = 0;
  GDEFSTOR_Result rc;
  if (store == NULL || out_bytes == NULL || out_len == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes) ||
      !gdefstor_span_ok(hash, hash_len, store->limits.max_hash_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_key_hex(store->alloc, "b:", hash, hash_len, NULL, 0, 0, &key,
      &key_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_lookup(store, tenant, tenant_len, key, key_len, 0, out_bytes,
      out_len);
  gcu_allocator_free(store->alloc, key);
  return rc;
}
