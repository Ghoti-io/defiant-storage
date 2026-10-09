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
 * Scratch backend for the conformance suite. The memory library runs the
 * same suite against its own backend; this one exists so the port can run
 * the suite without depending on that library.
 */

#include <gtest/gtest.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/fiber.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>

#include <cstring>
#include <string>

extern "C" {

typedef struct ScratchNode {
  char * tenant;
  size_t tenant_len;
  char * key;
  size_t key_len;
  uint8_t * bytes;
  size_t len;
  struct ScratchNode * next;
} ScratchNode;

typedef struct Scratch {
  const GDEFSTOR_Allocator * alloc;
  ScratchNode * head;
} Scratch;

static int scratch_same(const char * a, size_t a_len, const char * b,
    size_t b_len) {
  return a_len == b_len && (a_len == 0 || memcmp(a, b, a_len) == 0);
}

static char * scratch_dup(const GDEFSTOR_Allocator * alloc, const char * p,
    size_t n) {
  char * out = (char *)gcu_allocator_malloc(alloc, n + 1);
  if (out == NULL) {
    return NULL;
  }
  if (n > 0 && p != NULL) {
    memcpy(out, p, n);
  }
  out[n] = '\0';
  return out;
}

static ScratchNode * scratch_find(Scratch * s, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len) {
  ScratchNode * n;
  for (n = s->head; n != NULL; n = n->next) {
    if (scratch_same(n->tenant, n->tenant_len, tenant, tenant_len) &&
        scratch_same(n->key, n->key_len, key, key_len)) {
      return n;
    }
  }
  return NULL;
}

static void scratch_destroy(void * ctx) {
  Scratch * s = (Scratch *)ctx;
  ScratchNode * n;
  if (s == NULL) {
    return;
  }
  n = s->head;
  while (n != NULL) {
    ScratchNode * next = n->next;
    gcu_allocator_free(s->alloc, n->tenant);
    gcu_allocator_free(s->alloc, n->key);
    gcu_allocator_free(s->alloc, n->bytes);
    gcu_allocator_free(s->alloc, n);
    n = next;
  }
  gcu_allocator_free(s->alloc, s);
}

static GDEFSTOR_Result scratch_put(void * ctx, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len, const uint8_t * bytes,
    size_t len) {
  Scratch * s = (Scratch *)ctx;
  ScratchNode * n;
  uint8_t * copy;
  if (s == NULL || tenant == NULL || key == NULL || (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  copy = (uint8_t *)gcu_allocator_malloc(s->alloc, len == 0 ? 1 : len);
  if (copy == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  if (len > 0) {
    memcpy(copy, bytes, len);
  }
  n = scratch_find(s, tenant, tenant_len, key, key_len);
  if (n != NULL) {
    gcu_allocator_free(s->alloc, n->bytes);
    n->bytes = copy;
    n->len = len;
    return GDEFSTOR_OK;
  }
  n = (ScratchNode *)gcu_allocator_calloc(s->alloc, 1, sizeof *n);
  if (n == NULL) {
    gcu_allocator_free(s->alloc, copy);
    return GDEFSTOR_ERR_OOM;
  }
  n->tenant = scratch_dup(s->alloc, tenant, tenant_len);
  n->key = scratch_dup(s->alloc, key, key_len);
  if (n->tenant == NULL || n->key == NULL) {
    gcu_allocator_free(s->alloc, n->tenant);
    gcu_allocator_free(s->alloc, n->key);
    gcu_allocator_free(s->alloc, n);
    gcu_allocator_free(s->alloc, copy);
    return GDEFSTOR_ERR_OOM;
  }
  n->tenant_len = tenant_len;
  n->key_len = key_len;
  n->bytes = copy;
  n->len = len;
  n->next = s->head;
  s->head = n;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result scratch_get(void * ctx, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len, uint8_t ** out_bytes,
    size_t * out_len) {
  Scratch * s = (Scratch *)ctx;
  ScratchNode * n;
  uint8_t * copy;
  if (s == NULL || out_bytes == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = scratch_find(s, tenant, tenant_len, key, key_len);
  if (n == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  copy = (uint8_t *)gcu_allocator_malloc(s->alloc, n->len == 0 ? 1 : n->len);
  if (copy == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  if (n->len > 0) {
    memcpy(copy, n->bytes, n->len);
  }
  *out_bytes = copy;
  *out_len = n->len;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result scratch_remove(void * ctx, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len) {
  Scratch * s = (Scratch *)ctx;
  ScratchNode ** pp;
  if (s == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (pp = &s->head; *pp != NULL; pp = &(*pp)->next) {
    ScratchNode * n = *pp;
    if (scratch_same(n->tenant, n->tenant_len, tenant, tenant_len) &&
        scratch_same(n->key, n->key_len, key, key_len)) {
      *pp = n->next;
      gcu_allocator_free(s->alloc, n->tenant);
      gcu_allocator_free(s->alloc, n->key);
      gcu_allocator_free(s->alloc, n->bytes);
      gcu_allocator_free(s->alloc, n);
      return GDEFSTOR_OK;
    }
  }
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result scratch_list(void * ctx, const char * tenant,
    size_t tenant_len, const char * prefix, size_t prefix_len, char *** out_keys,
    size_t ** out_lens, size_t * out_count) {
  Scratch * s = (Scratch *)ctx;
  ScratchNode * n;
  size_t count = 0;
  size_t i = 0;
  char ** keys;
  size_t * lens;
  if (s == NULL || out_keys == NULL || out_lens == NULL || out_count == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (n = s->head; n != NULL; n = n->next) {
    if (scratch_same(n->tenant, n->tenant_len, tenant, tenant_len) &&
        n->key_len >= prefix_len &&
        (prefix_len == 0 || memcmp(n->key, prefix, prefix_len) == 0)) {
      count += 1;
    }
  }
  if (count == 0) {
    *out_keys = NULL;
    *out_lens = NULL;
    *out_count = 0;
    return GDEFSTOR_OK;
  }
  keys = (char **)gcu_allocator_calloc(s->alloc, count, sizeof *keys);
  lens = (size_t *)gcu_allocator_calloc(s->alloc, count, sizeof *lens);
  if (keys == NULL || lens == NULL) {
    gcu_allocator_free(s->alloc, keys);
    gcu_allocator_free(s->alloc, lens);
    return GDEFSTOR_ERR_OOM;
  }
  for (n = s->head; n != NULL; n = n->next) {
    if (scratch_same(n->tenant, n->tenant_len, tenant, tenant_len) &&
        n->key_len >= prefix_len &&
        (prefix_len == 0 || memcmp(n->key, prefix, prefix_len) == 0)) {
      keys[i] = scratch_dup(s->alloc, n->key, n->key_len);
      if (keys[i] == NULL) {
        size_t j;
        for (j = 0; j < i; j++) {
          gcu_allocator_free(s->alloc, keys[j]);
        }
        gcu_allocator_free(s->alloc, keys);
        gcu_allocator_free(s->alloc, lens);
        return GDEFSTOR_ERR_OOM;
      }
      lens[i] = n->key_len;
      i += 1;
    }
  }
  *out_keys = keys;
  *out_lens = lens;
  *out_count = count;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result ConformanceOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * allocator) {
  const GDEFSTOR_Allocator * use =
      allocator != NULL ? allocator : gcu_allocator_default();
  Scratch * scratch;
  GDEFSTOR_Backend backend;
  GDEFSTOR_Result rc;
  if (out == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  scratch = (Scratch *)gcu_allocator_calloc(use, 1, sizeof *scratch);
  if (scratch == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  scratch->alloc = use;
  memset(&backend, 0, sizeof backend);
  backend.ctx = scratch;
  backend.destroy = scratch_destroy;
  backend.put = scratch_put;
  backend.get = scratch_get;
  backend.remove = scratch_remove;
  backend.list = scratch_list;
  rc = gdefstor_store_create(&backend, use, NULL, out);
  if (rc != GDEFSTOR_OK) {
    scratch_destroy(scratch);
  }
  return rc;
}

void ConformanceClose(GDEFSTOR_Store * store) {
  gdefstor_store_destroy(store);
}

} /* extern "C" */

#include "suite.inc"

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
