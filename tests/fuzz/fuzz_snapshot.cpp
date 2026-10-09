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
 * Fuzz snapshot zips by importing them into an empty in-process bag. The
 * first byte selects tighter limits. Member names are never opened as paths.
 */

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>

#include <cstring>
#include <stddef.h>
#include <stdint.h>

enum { BAG_SLOTS = 32, BAG_KEY = 256 };

struct BagSlot {
  char key[BAG_KEY];
  size_t key_len;
  uint8_t * bytes;
  size_t len;
  int used;
};

struct Bag {
  const GCU_Allocator * alloc;
  BagSlot slots[BAG_SLOTS];
};

extern "C" {

static void bag_destroy(void * ctx) {
  Bag * bag = (Bag *)ctx;
  int i;
  if (bag == NULL) {
    return;
  }
  for (i = 0; i < BAG_SLOTS; i++) {
    gcu_allocator_free(bag->alloc, bag->slots[i].bytes);
  }
  gcu_allocator_free(bag->alloc, bag);
}

static GDEFSTOR_Result bag_put(void * ctx, const char * tenant, size_t tenant_len,
    const char * key, size_t key_len, const uint8_t * bytes, size_t len) {
  Bag * bag = (Bag *)ctx;
  int i;
  int free_slot = -1;
  uint8_t * copy;
  (void)tenant;
  (void)tenant_len;
  if (bag == NULL || key == NULL || key_len >= BAG_KEY ||
      (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; i < BAG_SLOTS; i++) {
    if (bag->slots[i].used && bag->slots[i].key_len == key_len &&
        memcmp(bag->slots[i].key, key, key_len) == 0) {
      copy = (uint8_t *)gcu_allocator_malloc(bag->alloc, len == 0 ? 1 : len);
      if (copy == NULL) {
        return GDEFSTOR_ERR_OOM;
      }
      if (len > 0) {
        memcpy(copy, bytes, len);
      }
      gcu_allocator_free(bag->alloc, bag->slots[i].bytes);
      bag->slots[i].bytes = copy;
      bag->slots[i].len = len;
      return GDEFSTOR_OK;
    }
    if (!bag->slots[i].used && free_slot < 0) {
      free_slot = i;
    }
  }
  if (free_slot < 0) {
    return GDEFSTOR_ERR_LIMIT;
  }
  copy = (uint8_t *)gcu_allocator_malloc(bag->alloc, len == 0 ? 1 : len);
  if (copy == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  if (len > 0) {
    memcpy(copy, bytes, len);
  }
  memcpy(bag->slots[free_slot].key, key, key_len);
  bag->slots[free_slot].key_len = key_len;
  bag->slots[free_slot].bytes = copy;
  bag->slots[free_slot].len = len;
  bag->slots[free_slot].used = 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result bag_get(void * ctx, const char * tenant, size_t tenant_len,
    const char * key, size_t key_len, uint8_t ** out_bytes, size_t * out_len) {
  Bag * bag = (Bag *)ctx;
  int i;
  (void)tenant;
  (void)tenant_len;
  if (bag == NULL || out_bytes == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; i < BAG_SLOTS; i++) {
    if (bag->slots[i].used && bag->slots[i].key_len == key_len &&
        memcmp(bag->slots[i].key, key, key_len) == 0) {
      uint8_t * copy = (uint8_t *)gcu_allocator_malloc(bag->alloc,
          bag->slots[i].len == 0 ? 1 : bag->slots[i].len);
      if (copy == NULL) {
        return GDEFSTOR_ERR_OOM;
      }
      if (bag->slots[i].len > 0) {
        memcpy(copy, bag->slots[i].bytes, bag->slots[i].len);
      }
      *out_bytes = copy;
      *out_len = bag->slots[i].len;
      return GDEFSTOR_OK;
    }
  }
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result bag_remove(void * ctx, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len) {
  Bag * bag = (Bag *)ctx;
  int i;
  (void)tenant;
  (void)tenant_len;
  if (bag == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; i < BAG_SLOTS; i++) {
    if (bag->slots[i].used && bag->slots[i].key_len == key_len &&
        memcmp(bag->slots[i].key, key, key_len) == 0) {
      gcu_allocator_free(bag->alloc, bag->slots[i].bytes);
      bag->slots[i].bytes = NULL;
      bag->slots[i].used = 0;
      return GDEFSTOR_OK;
    }
  }
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result bag_list(void * ctx, const char * tenant,
    size_t tenant_len, const char * prefix, size_t prefix_len, char *** out_keys,
    size_t ** out_lens, size_t * out_count) {
  Bag * bag = (Bag *)ctx;
  int i;
  size_t count = 0;
  size_t n = 0;
  char ** keys;
  size_t * lens;
  (void)tenant;
  (void)tenant_len;
  if (bag == NULL || out_keys == NULL || out_lens == NULL || out_count == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; i < BAG_SLOTS; i++) {
    if (bag->slots[i].used && bag->slots[i].key_len >= prefix_len &&
        (prefix_len == 0 ||
            memcmp(bag->slots[i].key, prefix, prefix_len) == 0)) {
      count += 1;
    }
  }
  if (count == 0) {
    *out_keys = NULL;
    *out_lens = NULL;
    *out_count = 0;
    return GDEFSTOR_OK;
  }
  keys = (char **)gcu_allocator_calloc(bag->alloc, count, sizeof *keys);
  lens = (size_t *)gcu_allocator_calloc(bag->alloc, count, sizeof *lens);
  if (keys == NULL || lens == NULL) {
    gcu_allocator_free(bag->alloc, keys);
    gcu_allocator_free(bag->alloc, lens);
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < BAG_SLOTS; i++) {
    if (bag->slots[i].used && bag->slots[i].key_len >= prefix_len &&
        (prefix_len == 0 ||
            memcmp(bag->slots[i].key, prefix, prefix_len) == 0)) {
      keys[n] = (char *)gcu_allocator_malloc(bag->alloc, bag->slots[i].key_len + 1);
      if (keys[n] == NULL) {
        size_t j;
        for (j = 0; j < n; j++) {
          gcu_allocator_free(bag->alloc, keys[j]);
        }
        gcu_allocator_free(bag->alloc, keys);
        gcu_allocator_free(bag->alloc, lens);
        return GDEFSTOR_ERR_OOM;
      }
      memcpy(keys[n], bag->slots[i].key, bag->slots[i].key_len);
      keys[n][bag->slots[i].key_len] = '\0';
      lens[n] = bag->slots[i].key_len;
      n += 1;
    }
  }
  *out_keys = keys;
  *out_lens = lens;
  *out_count = count;
  return GDEFSTOR_OK;
}

} /* extern "C" */

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Store * store = NULL;
  Bag * bag;
  GDEFSTOR_Backend backend;
  unsigned opt;
  const uint8_t * zip;
  size_t zip_len;
  if (data == NULL || size < 2) {
    return 0;
  }
  opt = data[0];
  zip = data + 1;
  zip_len = size - 1;
  gdefstor_limits_default(&limits);
  if (opt & 1u) {
    limits.max_member_bytes = 128;
  }
  if (opt & 2u) {
    limits.max_members = 4;
  }
  if (opt & 4u) {
    limits.max_zip_ratio = 2;
  }
  if (opt & 8u) {
    limits.max_snapshot_bytes = 256;
  }
  bag = (Bag *)gcu_allocator_calloc(gcu_allocator_default(), 1, sizeof *bag);
  if (bag == NULL) {
    return 0;
  }
  bag->alloc = gcu_allocator_default();
  memset(&backend, 0, sizeof backend);
  backend.ctx = bag;
  backend.destroy = bag_destroy;
  backend.put = bag_put;
  backend.get = bag_get;
  backend.remove = bag_remove;
  backend.list = bag_list;
  if (gdefstor_store_create(&backend, NULL, &limits, &store) == GDEFSTOR_OK) {
    gdefstor_snapshot_import(store, "t", 1, zip, zip_len, &limits);
    gdefstor_store_destroy(store);
  } else {
    bag_destroy(bag);
  }
  return 0;
}
