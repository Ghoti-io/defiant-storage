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
 * Projection rows as length-prefixed columns. Explicit shifts, no overlay.
 */

#include "internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static GDEFSTOR_Result put_u32(uint8_t * p, size_t cap, size_t * at,
    uint32_t v) {
  if (*at > cap || cap - *at < 4) {
    return GDEFSTOR_ERR_LIMIT;
  }
  p[*at] = (uint8_t)(v & 0xffu);
  p[*at + 1] = (uint8_t)((v >> 8) & 0xffu);
  p[*at + 2] = (uint8_t)((v >> 16) & 0xffu);
  p[*at + 3] = (uint8_t)((v >> 24) & 0xffu);
  *at += 4;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result get_u32(const uint8_t * p, size_t len, size_t * at,
    uint32_t * out) {
  if (*at > len || len - *at < 4) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  *out = (uint32_t)p[*at] | ((uint32_t)p[*at + 1] << 8) |
         ((uint32_t)p[*at + 2] << 16) | ((uint32_t)p[*at + 3] << 24);
  *at += 4;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_projection_put(GDEFSTOR_Write * write,
    const char * type, size_t type_len, const char * entity, size_t entity_len,
    const GDEFSTOR_Column * columns, size_t column_count) {
  GDEFSTOR_Pending * row;
  size_t i;
  size_t j;
  size_t bytes_need = 4;
  size_t at = 0;
  size_t count = 0;
  GDEFSTOR_Result rc;
  if (write == NULL || !write->alive ||
      !gdefstor_span_ok(type, type_len, write->limits.max_type_bytes) ||
      !gdefstor_span_ok(entity, entity_len, write->limits.max_public_id_bytes) ||
      (columns == NULL && column_count != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (column_count > write->limits.max_columns) {
    return GDEFSTOR_ERR_LIMIT;
  }
  for (i = 0; i < column_count; i++) {
    if (!gdefstor_span_ok(columns[i].name, columns[i].name_len,
            write->limits.max_column_name_bytes) ||
        (columns[i].value == NULL && columns[i].value_len != 0)) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (columns[i].value_len > write->limits.max_column_value_bytes) {
      return GDEFSTOR_ERR_LIMIT;
    }
    for (j = 0; j < i; j++) {
      if (columns[i].name_len == columns[j].name_len &&
          memcmp(columns[i].name, columns[j].name, columns[i].name_len) == 0) {
        return GDEFSTOR_ERR_INVALID;
      }
    }
    if (bytes_need > SIZE_MAX - 8 - columns[i].name_len ||
        bytes_need > SIZE_MAX - 8 - columns[i].name_len -
                         columns[i].value_len) {
      return GDEFSTOR_ERR_LIMIT;
    }
    bytes_need += 8 + columns[i].name_len + columns[i].value_len;
  }
  row = (GDEFSTOR_Pending *)gdefstor_mem0(write->store->alloc, sizeof *row);
  if (row == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  row->kind = GDEFSTOR_KIND_PROJ;
  row->encoded = 1;
  rc = gdefstor_key_hex(write->store->alloc, "p:", type, type_len, entity,
      entity_len, 1, &row->key, &row->key_len);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
    return rc;
  }
  rc = gdefstor_count(write->store, write, "p:", 2, &count);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
    return rc;
  }
  {
    uint8_t * existing = NULL;
    size_t existing_len = 0;
    GDEFSTOR_Result got = gdefstor_lookup(write->store, write->tenant,
        write->tenant_len, row->key, row->key_len, 1, &existing, &existing_len);
    if (got == GDEFSTOR_OK) {
      gcu_allocator_free(write->store->alloc, existing);
    } else if (got != GDEFSTOR_ERR_INVALID) {
      gdefstor_free_pending(write->store->alloc, row);
      return got;
    } else if (count >= write->limits.max_projections) {
      gdefstor_free_pending(write->store->alloc, row);
      return GDEFSTOR_ERR_LIMIT;
    }
  }
  row->bytes = (uint8_t *)gdefstor_mem(write->store->alloc, bytes_need);
  if (row->bytes == NULL) {
    gdefstor_free_pending(write->store->alloc, row);
    return GDEFSTOR_ERR_OOM;
  }
  row->len = bytes_need;
  rc = put_u32(row->bytes, row->len, &at, (uint32_t)column_count);
  for (i = 0; rc == GDEFSTOR_OK && i < column_count; i++) {
    rc = put_u32(row->bytes, row->len, &at, (uint32_t)columns[i].name_len);
    if (rc != GDEFSTOR_OK) {
      break;
    }
    memcpy(row->bytes + at, columns[i].name, columns[i].name_len);
    at += columns[i].name_len;
    rc = put_u32(row->bytes, row->len, &at, (uint32_t)columns[i].value_len);
    if (rc != GDEFSTOR_OK) {
      break;
    }
    if (columns[i].value_len > 0) {
      memcpy(row->bytes + at, columns[i].value, columns[i].value_len);
      at += columns[i].value_len;
    }
  }
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
    return rc;
  }
  rc = gdefstor_stage(write, row);
  if (rc != GDEFSTOR_OK) {
    /* A replacement of the same key is staged by dropping the previous
     * pending row, which stage refuses. Replace it here. */
    if (rc == GDEFSTOR_ERR_INVALID) {
      GDEFSTOR_Pending ** pp = &write->head;
      while (*pp != NULL) {
        if ((*pp)->key != NULL && (*pp)->key_len == row->key_len &&
            memcmp((*pp)->key, row->key, row->key_len) == 0) {
          GDEFSTOR_Pending * old = *pp;
          row->next = old->next;
          *pp = row;
          if (write->tail == old) {
            write->tail = row;
          }
          gdefstor_free_pending(write->store->alloc, old);
          return GDEFSTOR_OK;
        }
        pp = &(*pp)->next;
      }
    }
    gdefstor_free_pending(write->store->alloc, row);
  }
  return rc;
}

static GDEFSTOR_Result decode_projection(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Limits * limits, const uint8_t * bytes, size_t len,
    GDEFSTOR_Projection ** out_row) {
  size_t at = 0;
  uint32_t count = 0;
  uint32_t i;
  GDEFSTOR_Result rc;
  GDEFSTOR_Projection * row;
  rc = get_u32(bytes, len, &at, &count);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (limits != NULL && count > limits->max_columns) {
    return GDEFSTOR_ERR_LIMIT;
  }
  row = (GDEFSTOR_Projection *)gdefstor_mem0(alloc, sizeof *row);
  if (row == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  row->alloc = gdefstor_alloc(alloc);
  row->count = count;
  if (count > 0) {
    row->columns =
        (GDEFSTOR_Column *)gdefstor_mem0(row->alloc, count * sizeof *row->columns);
    if (row->columns == NULL) {
      gdefstor_projection_free(row);
      return GDEFSTOR_ERR_OOM;
    }
  }
  for (i = 0; i < count; i++) {
    uint32_t name_len = 0;
    uint32_t value_len = 0;
    char * name;
    uint8_t * value;
    rc = get_u32(bytes, len, &at, &name_len);
    if (rc != GDEFSTOR_OK) {
      gdefstor_projection_free(row);
      return rc;
    }
    if (at > len || len - at < name_len) {
      gdefstor_projection_free(row);
      return GDEFSTOR_ERR_CORRUPT;
    }
    name = gdefstor_dup(row->alloc, (const char *)bytes + at, name_len);
    at += name_len;
    if (name == NULL) {
      gdefstor_projection_free(row);
      return GDEFSTOR_ERR_OOM;
    }
    rc = get_u32(bytes, len, &at, &value_len);
    if (rc != GDEFSTOR_OK) {
      gcu_allocator_free(row->alloc, name);
      gdefstor_projection_free(row);
      return rc;
    }
    if (at > len || len - at < value_len) {
      gcu_allocator_free(row->alloc, name);
      gdefstor_projection_free(row);
      return GDEFSTOR_ERR_CORRUPT;
    }
    value = (uint8_t *)gdefstor_mem(row->alloc, value_len);
    if (value == NULL) {
      gcu_allocator_free(row->alloc, name);
      gdefstor_projection_free(row);
      return GDEFSTOR_ERR_OOM;
    }
    if (value_len > 0) {
      memcpy(value, bytes + at, value_len);
    }
    at += value_len;
    row->columns[i].name = name;
    row->columns[i].name_len = name_len;
    row->columns[i].value = value;
    row->columns[i].value_len = value_len;
  }
  if (at != len) {
    gdefstor_projection_free(row);
    return GDEFSTOR_ERR_CORRUPT;
  }
  *out_row = row;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_projection_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const char * entity, size_t entity_len, GDEFSTOR_Projection ** out_row) {
  char * key = NULL;
  size_t key_len = 0;
  uint8_t * bytes = NULL;
  size_t len = 0;
  GDEFSTOR_Result rc;
  if (store == NULL || out_row == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes) ||
      !gdefstor_span_ok(type, type_len, store->limits.max_type_bytes) ||
      !gdefstor_span_ok(entity, entity_len, store->limits.max_public_id_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_key_hex(store->alloc, "p:", type, type_len, entity, entity_len,
      1, &key, &key_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_lookup(store, tenant, tenant_len, key, key_len, 0, &bytes, &len);
  gcu_allocator_free(store->alloc, key);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = decode_projection(store->alloc, &store->limits, bytes, len, out_row);
  gcu_allocator_free(store->alloc, bytes);
  return rc;
}

size_t gdefstor_projection_column_count(const GDEFSTOR_Projection * row) {
  return row == NULL ? 0 : row->count;
}

GDEFSTOR_Result gdefstor_projection_column(const GDEFSTOR_Projection * row,
    size_t index, GDEFSTOR_Column * out_column) {
  if (row == NULL || out_column == NULL || index >= row->count) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_column = row->columns[index];
  return GDEFSTOR_OK;
}

void gdefstor_projection_free(GDEFSTOR_Projection * row) {
  size_t i;
  if (row == NULL) {
    return;
  }
  if (row->columns != NULL) {
    for (i = 0; i < row->count; i++) {
      gcu_allocator_free(row->alloc, (void *)row->columns[i].name);
      gcu_allocator_free(row->alloc, (void *)row->columns[i].value);
    }
    gcu_allocator_free(row->alloc, row->columns);
  }
  gcu_allocator_free(row->alloc, row);
}
