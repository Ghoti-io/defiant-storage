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
 * The key store port (keystore.h): argument checks over a backend, and the
 * key set's text (`defiant-keyset/1`). The port holds no key and does no
 * cryptography.
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A public id, as model.h spells one, of at most this many bytes. */
#define KEYSTORE_ID_BYTES 256u
#define KEYSTORE_TENANT_BYTES 256u

struct GDEFSTOR_KeyStore {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_KeyStoreBackend backend; /* the optional members NULL for V1 */
};

/* Whether the backend has the optional members (all are checked at create). */
static int has_v2(const GDEFSTOR_KeyStore * ks) {
  return ks->backend.sealed_put != NULL;
}

GDEFSTOR_Result gdefstor_keystore_create(const GDEFSTOR_KeyStoreBackend * backend,
    const GDEFSTOR_Allocator * allocator, GDEFSTOR_KeyStore ** out_keystore) {
  GDEFSTOR_KeyStore * ks;
  int full;
  if (backend == NULL || out_keystore == NULL ||
      (backend->size != sizeof(GDEFSTOR_KeyStoreBackend) &&
          backend->size != GDEFSTOR_KEYSTORE_BACKEND_SIZE_V1) ||
      backend->destroy == NULL || backend->create == NULL ||
      backend->wrap == NULL || backend->unwrap == NULL ||
      backend->erase == NULL || backend->ledger == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  full = backend->size == sizeof(GDEFSTOR_KeyStoreBackend);
  if (full && !((backend->sealed_put != NULL && backend->sealed_get != NULL &&
                    backend->secret_put != NULL &&
                    backend->secret_get != NULL && backend->dump != NULL &&
                    backend->restore != NULL) ||
                   (backend->sealed_put == NULL && backend->sealed_get == NULL &&
                       backend->secret_put == NULL &&
                       backend->secret_get == NULL && backend->dump == NULL &&
                       backend->restore == NULL))) {
    return GDEFSTOR_ERR_INVALID; /* all of them or none */
  }
  ks = (GDEFSTOR_KeyStore *)gdefstor_mem0(allocator, sizeof *ks);
  if (ks == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  ks->alloc = gdefstor_alloc(allocator);
  /* A V1 backend is copied as far as it goes; the rest stays NULL. */
  memcpy(&ks->backend, backend, full ? sizeof *backend
                                     : GDEFSTOR_KEYSTORE_BACKEND_SIZE_V1);
  *out_keystore = ks;
  return GDEFSTOR_OK;
}

void gdefstor_keystore_destroy(GDEFSTOR_KeyStore * keystore) {
  if (keystore == NULL) {
    return;
  }
  keystore->backend.destroy(keystore->backend.ctx);
  gcu_allocator_free(keystore->alloc, keystore);
}

static GDEFSTOR_Result args(const GDEFSTOR_KeyStore * ks, const char * tenant,
    size_t tenant_len, const char * subject, size_t subject_len) {
  if (ks == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, KEYSTORE_TENANT_BYTES) ||
      tenant_len == 0 ||
      !gdefstor_model_id_ok(subject, subject_len, KEYSTORE_ID_BYTES, 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_keystore_subject_create(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len) {
  GDEFSTOR_Result rc = args(keystore, tenant, tenant_len, subject, subject_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  return keystore->backend.create(keystore->backend.ctx, tenant, tenant_len,
      subject, subject_len);
}

static GDEFSTOR_Result seal_or_open(GDEFSTOR_KeyStore * ks, int seal,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, const uint8_t * bytes, size_t len, int * out_erased,
    uint8_t ** out_bytes, size_t * out_len) {
  uint8_t * got = NULL;
  size_t got_len = 0;
  int erased = 0;
  GDEFSTOR_Result rc = args(ks, tenant, tenant_len, subject, subject_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (out_erased == NULL || out_bytes == NULL || out_len == NULL ||
      (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (len > GDEFSTOR_KEYSTORE_MAX_BYTES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  rc = (seal ? ks->backend.wrap : ks->backend.unwrap)(ks->backend.ctx, tenant,
      tenant_len, subject, subject_len, bytes, len, &erased, &got, &got_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (erased) {
    /* An erased subject's bytes are gone; nothing comes back. */
    gcu_allocator_free(ks->alloc, got);
    *out_erased = 1;
    *out_bytes = NULL;
    *out_len = 0;
    return GDEFSTOR_OK;
  }
  if (got == NULL) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  *out_erased = 0;
  *out_bytes = got;
  *out_len = got_len;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_keystore_wrap(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, const uint8_t * bytes, size_t len, int * out_erased,
    uint8_t ** out_bytes, size_t * out_len) {
  return seal_or_open(keystore, 1, tenant, tenant_len, subject, subject_len,
      bytes, len, out_erased, out_bytes, out_len);
}

GDEFSTOR_Result gdefstor_keystore_unwrap(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, const uint8_t * bytes, size_t len, int * out_erased,
    uint8_t ** out_bytes, size_t * out_len) {
  return seal_or_open(keystore, 0, tenant, tenant_len, subject, subject_len,
      bytes, len, out_erased, out_bytes, out_len);
}

GDEFSTOR_Result gdefstor_keystore_erase(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, int64_t when_ms) {
  GDEFSTOR_Result rc = args(keystore, tenant, tenant_len, subject, subject_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  return keystore->backend.erase(keystore->backend.ctx, tenant, tenant_len,
      subject, subject_len, when_ms);
}

GDEFSTOR_Result gdefstor_keystore_ledger_append(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, int64_t when_ms) {
  /* A replayed erasure is an erasure: the key goes too, so the ledger and
   * the keys never disagree. */
  return gdefstor_keystore_erase(keystore, tenant, tenant_len, subject,
      subject_len, when_ms);
}

typedef struct LRow {
  char * subject;
  size_t subject_len;
  int64_t when_ms;
} LRow;

struct GDEFSTOR_Ledger {
  const GDEFSTOR_Allocator * alloc;
  LRow * v;
  size_t n;
  size_t cap;
};

void gdefstor_ledger_free(GDEFSTOR_Ledger * ledger) {
  size_t i;
  if (ledger == NULL) {
    return;
  }
  for (i = 0; i < ledger->n; i++) {
    gcu_allocator_free(ledger->alloc, ledger->v[i].subject);
  }
  gcu_allocator_free(ledger->alloc, ledger->v);
  gcu_allocator_free(ledger->alloc, ledger);
}

static GDEFSTOR_Result ledger_visit(void * vctx, const char * subject,
    size_t subject_len, int64_t when_ms) {
  GDEFSTOR_Ledger * l = (GDEFSTOR_Ledger *)vctx;
  if (l->n == l->cap) {
    size_t grown = l->cap == 0 ? 8 : l->cap * 2;
    LRow * next = (LRow *)gcu_allocator_realloc(l->alloc, l->v,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    l->v = next;
    l->cap = grown;
  }
  l->v[l->n].subject = gdefstor_dup(l->alloc, subject, subject_len);
  if (l->v[l->n].subject == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  l->v[l->n].subject_len = subject_len;
  l->v[l->n].when_ms = when_ms;
  l->n += 1;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_keystore_ledger(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, GDEFSTOR_Ledger ** out_ledger) {
  GDEFSTOR_Ledger * l;
  GDEFSTOR_Result rc;
  if (keystore == NULL || out_ledger == NULL || tenant_len == 0 ||
      !gdefstor_span_ok(tenant, tenant_len, KEYSTORE_TENANT_BYTES)) {
    return GDEFSTOR_ERR_INVALID;
  }
  l = (GDEFSTOR_Ledger *)gdefstor_mem0(keystore->alloc, sizeof *l);
  if (l == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  l->alloc = keystore->alloc;
  rc = keystore->backend.ledger(keystore->backend.ctx, tenant, tenant_len,
      ledger_visit, l);
  if (rc != GDEFSTOR_OK) {
    gdefstor_ledger_free(l);
    return rc;
  }
  *out_ledger = l;
  return GDEFSTOR_OK;
}

size_t gdefstor_ledger_count(const GDEFSTOR_Ledger * ledger) {
  return ledger == NULL ? 0 : ledger->n;
}

GDEFSTOR_Result gdefstor_ledger_row(const GDEFSTOR_Ledger * ledger,
    size_t index, const char ** out_subject, size_t * out_subject_len,
    int64_t * out_when_ms) {
  if (ledger == NULL || index >= ledger->n || out_subject == NULL ||
      out_subject_len == NULL || out_when_ms == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_subject = ledger->v[index].subject;
  *out_subject_len = ledger->v[index].subject_len;
  *out_when_ms = ledger->v[index].when_ms;
  return GDEFSTOR_OK;
}

/* ---- sealed material and tenant secrets ----------------------------------- */

static GDEFSTOR_Result material_ok(const uint8_t * bytes, size_t len) {
  if (bytes == NULL || len == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  return len > GDEFSTOR_KEYSTORE_MAX_MATERIAL_BYTES ? GDEFSTOR_ERR_LIMIT
                                                    : GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_keystore_subject_put(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, const uint8_t * bytes, size_t len) {
  GDEFSTOR_Result rc = args(keystore, tenant, tenant_len, subject, subject_len);
  if (rc == GDEFSTOR_OK) {
    rc = material_ok(bytes, len);
  }
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!has_v2(keystore)) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  return keystore->backend.sealed_put(keystore->backend.ctx, tenant, tenant_len,
      subject, subject_len, bytes, len);
}

/* A read that hands back bytes: the erased flag and the bytes are consistent
 * whatever the backend did. */
static GDEFSTOR_Result read_back(GDEFSTOR_KeyStore * ks, GDEFSTOR_Result rc,
    int flag, int flag_means_bytes, uint8_t * got, size_t got_len,
    int * out_flag, uint8_t ** out_bytes, size_t * out_len) {
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(ks->alloc, got);
    return rc;
  }
  if ((flag != 0) != (flag_means_bytes != 0)) {
    /* No bytes come back: erased, or not found. */
    gcu_allocator_free(ks->alloc, got);
    *out_flag = flag;
    *out_bytes = NULL;
    *out_len = 0;
    return GDEFSTOR_OK;
  }
  if (got == NULL || got_len == 0) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  *out_flag = flag;
  *out_bytes = got;
  *out_len = got_len;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_keystore_subject_sealed(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, int * out_erased, uint8_t ** out_bytes,
    size_t * out_len) {
  uint8_t * got = NULL;
  size_t got_len = 0;
  int erased = 0;
  GDEFSTOR_Result rc = args(keystore, tenant, tenant_len, subject, subject_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (out_erased == NULL || out_bytes == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (!has_v2(keystore)) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  rc = keystore->backend.sealed_get(keystore->backend.ctx, tenant, tenant_len,
      subject, subject_len, &erased, &got, &got_len);
  return read_back(keystore, rc, erased, 0, got, got_len, out_erased,
      out_bytes, out_len);
}

GDEFSTOR_Result gdefstor_keystore_secret_put(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * name, size_t name_len,
    const uint8_t * bytes, size_t len) {
  GDEFSTOR_Result rc = args(keystore, tenant, tenant_len, name, name_len);
  if (rc == GDEFSTOR_OK) {
    rc = material_ok(bytes, len);
  }
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!has_v2(keystore)) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  return keystore->backend.secret_put(keystore->backend.ctx, tenant, tenant_len,
      name, name_len, bytes, len);
}

GDEFSTOR_Result gdefstor_keystore_secret_get(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * name, size_t name_len,
    int * out_found, uint8_t ** out_bytes, size_t * out_len) {
  uint8_t * got = NULL;
  size_t got_len = 0;
  int found = 0;
  GDEFSTOR_Result rc = args(keystore, tenant, tenant_len, name, name_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (out_found == NULL || out_bytes == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (!has_v2(keystore)) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  rc = keystore->backend.secret_get(keystore->backend.ctx, tenant, tenant_len,
      name, name_len, &found, &got, &got_len);
  return read_back(keystore, rc, found, 1, got, got_len, out_found, out_bytes,
      out_len);
}

/* ---- the key set ----------------------------------------------------------- */

static const char KEYSET_NAME[] = "defiant-keyset/";
#define KEYSET_VERSION 1

static const char * kind_word(int kind) {
  switch (kind) {
  case GDEFSTOR_KEYROW_SECRET:
    return "secret";
  case GDEFSTOR_KEYROW_KEY:
    return "key";
  case GDEFSTOR_KEYROW_SEALED:
    return "sealed";
  case GDEFSTOR_KEYROW_LEDGER:
    return "ledger";
  default:
    return NULL;
  }
}

typedef struct KRow {
  int kind;
  char * name;
  size_t name_len;
  uint8_t * bytes;
  size_t len;
  int64_t when_ms;
} KRow;

struct GDEFSTOR_KeySet {
  const GDEFSTOR_Allocator * alloc;
  int64_t taken_ms;
  KRow * v;
  size_t n;
  size_t cap;
};

static void wipe_bytes(uint8_t * p, size_t n) {
  volatile uint8_t * v = p;
  size_t i;
  for (i = 0; i < n; i++) {
    v[i] = 0;
  }
}

void gdefstor_keyset_free(GDEFSTOR_KeySet * set) {
  size_t i;
  if (set == NULL) {
    return;
  }
  for (i = 0; i < set->n; i++) {
    gcu_allocator_free(set->alloc, set->v[i].name);
    if (set->v[i].bytes != NULL) {
      wipe_bytes(set->v[i].bytes, set->v[i].len);
    }
    gcu_allocator_free(set->alloc, set->v[i].bytes);
  }
  gcu_allocator_free(set->alloc, set->v);
  gcu_allocator_free(set->alloc, set);
}

int64_t gdefstor_keyset_taken(const GDEFSTOR_KeySet * set) {
  return set == NULL ? 0 : set->taken_ms;
}

size_t gdefstor_keyset_count(const GDEFSTOR_KeySet * set) {
  return set == NULL ? 0 : set->n;
}

GDEFSTOR_Result gdefstor_keyset_row(const GDEFSTOR_KeySet * set, size_t index,
    GDEFSTOR_KeyRow * out_row) {
  if (set == NULL || index >= set->n || out_row == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(out_row, 0, sizeof *out_row);
  out_row->kind = set->v[index].kind;
  out_row->name = set->v[index].name;
  out_row->name_len = set->v[index].name_len;
  out_row->bytes = set->v[index].bytes;
  out_row->len = set->v[index].len;
  out_row->when_ms = set->v[index].when_ms;
  return GDEFSTOR_OK;
}

static int same_name(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && memcmp(a, b, an) == 0;
}

/* The rules gdefstor_keyset_parse() states, over rows in memory: kinds in
 * order, public-id names, bytes in bounds (none for the ledger), a name once
 * per kind, and a subject with a key or sealed material, not both. Quadratic
 * in the rows of one kind; a key set is one tenant's. */
static GDEFSTOR_Result rows_ok(const GDEFSTOR_KeyRow * rows, size_t count) {
  size_t i;
  size_t j;
  int last = 0;
  for (i = 0; i < count; i++) {
    const GDEFSTOR_KeyRow * r = &rows[i];
    if (kind_word(r->kind) == NULL || r->kind < last ||
        !gdefstor_model_id_ok(r->name, r->name_len, KEYSTORE_ID_BYTES, 0)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    last = r->kind;
    if (r->kind == GDEFSTOR_KEYROW_LEDGER) {
      if (r->bytes != NULL || r->len != 0) {
        return GDEFSTOR_ERR_CORRUPT;
      }
    } else if (r->bytes == NULL || r->len == 0 ||
               r->len > GDEFSTOR_KEYSTORE_MAX_MATERIAL_BYTES || r->when_ms != 0) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    for (j = 0; j < i; j++) {
      const GDEFSTOR_KeyRow * o = &rows[j];
      int clash = o->kind == r->kind ||
                  (o->kind == GDEFSTOR_KEYROW_KEY &&
                      r->kind == GDEFSTOR_KEYROW_SEALED);
      if (clash && same_name(o->name, o->name_len, r->name, r->name_len)) {
        return GDEFSTOR_ERR_CORRUPT;
      }
    }
  }
  return GDEFSTOR_OK;
}

static const char HEXD[] = "0123456789abcdef";

GDEFSTOR_Result gdefstor_keyset_encode(const GDEFSTOR_Allocator * allocator,
    int64_t taken_ms, const GDEFSTOR_KeyRow * rows, size_t count,
    uint8_t ** out_bytes, size_t * out_len) {
  const GDEFSTOR_Allocator * alloc = gdefstor_alloc(allocator);
  char * buf = NULL;
  size_t len = 0;
  size_t cap = 0;
  char line[64];
  int kn;
  size_t i;
  GDEFSTOR_Result rc;
  if ((rows == NULL && count != 0) || out_bytes == NULL || out_len == NULL ||
      taken_ms < 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = rows_ok(rows, count);
  if (rc != GDEFSTOR_OK) {
    return GDEFSTOR_ERR_INVALID;
  }
  kn = snprintf(line, sizeof line, "%s%d\ntaken %lld\n", KEYSET_NAME,
      KEYSET_VERSION, (long long)taken_ms);
  if (kn <= 0 || (size_t)kn >= sizeof line) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  rc = gdefstor_buf_append(&buf, &len, &cap, alloc, line, (size_t)kn);
  for (i = 0; rc == GDEFSTOR_OK && i < count; i++) {
    const GDEFSTOR_KeyRow * r = &rows[i];
    const char * w = kind_word(r->kind);
    rc = gdefstor_buf_append(&buf, &len, &cap, alloc, w, strlen(w));
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&buf, &len, &cap, alloc, " ", 1);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&buf, &len, &cap, alloc, r->name, r->name_len);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&buf, &len, &cap, alloc, " ", 1);
    }
    if (rc == GDEFSTOR_OK && r->kind == GDEFSTOR_KEYROW_LEDGER) {
      kn = snprintf(line, sizeof line, "%lld", (long long)r->when_ms);
      rc = kn > 0 && (size_t)kn < sizeof line
               ? gdefstor_buf_append(&buf, &len, &cap, alloc, line, (size_t)kn)
               : GDEFSTOR_ERR_INTERNAL;
    } else {
      size_t b;
      for (b = 0; rc == GDEFSTOR_OK && b < r->len; b++) {
        char h[2];
        h[0] = HEXD[r->bytes[b] >> 4];
        h[1] = HEXD[r->bytes[b] & 0x0f];
        rc = gdefstor_buf_append(&buf, &len, &cap, alloc, h, 2);
      }
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&buf, &len, &cap, alloc, "\n", 1);
    }
  }
  if (rc != GDEFSTOR_OK) {
    if (buf != NULL) {
      wipe_bytes((uint8_t *)buf, len);
    }
    gcu_allocator_free(alloc, buf);
    return rc;
  }
  *out_bytes = (uint8_t *)buf;
  *out_len = len;
  return GDEFSTOR_OK;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

/* A decimal int64 of at most 19 digits, an optional leading '-'. */
static int dec64(const char * p, size_t n, int64_t * out) {
  size_t i = 0;
  int neg = 0;
  uint64_t v = 0;
  if (n > 0 && p[0] == '-') {
    neg = 1;
    i = 1;
  }
  if (i == n || n - i > 19 || (n - i > 1 && p[i] == '0')) {
    return 0;
  }
  for (; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') {
      return 0;
    }
    v = v * 10u + (uint64_t)(p[i] - '0');
  }
  if (v > (uint64_t)INT64_MAX) {
    return 0;
  }
  *out = neg ? -(int64_t)v : (int64_t)v;
  return 1;
}

static GDEFSTOR_Result set_push(GDEFSTOR_KeySet * set, KRow * row) {
  if (set->n == set->cap) {
    size_t grown = set->cap == 0 ? 16 : set->cap * 2;
    KRow * next;
    if (grown > SIZE_MAX / sizeof *next) {
      return GDEFSTOR_ERR_LIMIT;
    }
    next = (KRow *)gcu_allocator_realloc(set->alloc, set->v,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    set->v = next;
    set->cap = grown;
  }
  set->v[set->n] = *row;
  set->n += 1;
  return GDEFSTOR_OK;
}

/* One line, without its newline: "<word> <name> <hex or when>". */
static GDEFSTOR_Result parse_row(GDEFSTOR_KeySet * set, const char * p,
    size_t n) {
  KRow row;
  size_t a = 0;
  size_t b;
  size_t i;
  int kind = 0;
  GDEFSTOR_Result rc;
  memset(&row, 0, sizeof row);
  while (a < n && p[a] != ' ') {
    a += 1;
  }
  for (i = GDEFSTOR_KEYROW_SECRET; i <= GDEFSTOR_KEYROW_LEDGER; i++) {
    const char * w = kind_word((int)i);
    if (same_name(p, a, w, strlen(w))) {
      kind = (int)i;
    }
  }
  if (kind == 0 || a == n) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  b = a + 1;
  while (b < n && p[b] != ' ') {
    b += 1;
  }
  if (b == n || b == a + 1) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  row.kind = kind;
  row.name_len = b - a - 1;
  row.name = gdefstor_dup(set->alloc, p + a + 1, row.name_len);
  if (row.name == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  b += 1;
  if (kind == GDEFSTOR_KEYROW_LEDGER) {
    if (!dec64(p + b, n - b, &row.when_ms)) {
      gcu_allocator_free(set->alloc, row.name);
      return GDEFSTOR_ERR_CORRUPT;
    }
  } else {
    size_t hex = n - b;
    if (hex == 0 || hex % 2 != 0 ||
        hex / 2 > GDEFSTOR_KEYSTORE_MAX_MATERIAL_BYTES) {
      gcu_allocator_free(set->alloc, row.name);
      return GDEFSTOR_ERR_CORRUPT;
    }
    row.len = hex / 2;
    row.bytes = (uint8_t *)gdefstor_mem(set->alloc, row.len);
    if (row.bytes == NULL) {
      gcu_allocator_free(set->alloc, row.name);
      return GDEFSTOR_ERR_OOM;
    }
    for (i = 0; i < row.len; i++) {
      int hi = hexv(p[b + i * 2]);
      int lo = hexv(p[b + i * 2 + 1]);
      if (hi < 0 || lo < 0) {
        wipe_bytes(row.bytes, row.len);
        gcu_allocator_free(set->alloc, row.bytes);
        gcu_allocator_free(set->alloc, row.name);
        return GDEFSTOR_ERR_CORRUPT;
      }
      row.bytes[i] = (uint8_t)((hi << 4) | lo);
    }
  }
  rc = set_push(set, &row);
  if (rc != GDEFSTOR_OK) {
    if (row.bytes != NULL) {
      wipe_bytes(row.bytes, row.len);
    }
    gcu_allocator_free(set->alloc, row.bytes);
    gcu_allocator_free(set->alloc, row.name);
  }
  return rc;
}

/* The rows of a parsed set as the borrowed form rows_ok() and restore take. */
static GDEFSTOR_Result set_rows(const GDEFSTOR_KeySet * set,
    GDEFSTOR_KeyRow ** out) {
  GDEFSTOR_KeyRow * rows;
  size_t i;
  rows = (GDEFSTOR_KeyRow *)gdefstor_mem0(set->alloc,
      (set->n == 0 ? 1 : set->n) * sizeof *rows);
  if (rows == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < set->n; i++) {
    (void)gdefstor_keyset_row(set, i, &rows[i]);
  }
  *out = rows;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_keyset_parse(const GDEFSTOR_Allocator * allocator,
    const uint8_t * bytes, size_t len, GDEFSTOR_KeySet ** out_set) {
  const char * p = (const char *)bytes;
  size_t name_len = strlen(KEYSET_NAME);
  size_t at;
  size_t end;
  int64_t version = 0;
  GDEFSTOR_KeySet * set;
  GDEFSTOR_KeyRow * rows = NULL;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (out_set == NULL || (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (len < name_len || memcmp(p, KEYSET_NAME, name_len) != 0) {
    return GDEFSTOR_ERR_FORMAT;
  }
  end = name_len;
  while (end < len && p[end] != '\n') {
    end += 1;
  }
  if (end == len || !dec64(p + name_len, end - name_len, &version) ||
      version <= 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (version > KEYSET_VERSION) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  set = (GDEFSTOR_KeySet *)gdefstor_mem0(allocator, sizeof *set);
  if (set == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  set->alloc = gdefstor_alloc(allocator);
  at = end + 1;
  end = at;
  while (end < len && p[end] != '\n') {
    end += 1;
  }
  if (end == len || end - at < 7 || memcmp(p + at, "taken ", 6) != 0 ||
      !dec64(p + at + 6, end - at - 6, &set->taken_ms) || set->taken_ms < 0) {
    gdefstor_keyset_free(set);
    return GDEFSTOR_ERR_CORRUPT;
  }
  at = end + 1;
  while (rc == GDEFSTOR_OK && at < len) {
    end = at;
    while (end < len && p[end] != '\n') {
      end += 1;
    }
    if (end == len) {
      rc = GDEFSTOR_ERR_CORRUPT; /* every line ends in a newline */
      break;
    }
    rc = parse_row(set, p + at, end - at);
    at = end + 1;
  }
  if (rc == GDEFSTOR_OK) {
    rc = set_rows(set, &rows);
  }
  if (rc == GDEFSTOR_OK) {
    rc = rows_ok(rows, set->n);
  }
  gcu_allocator_free(set->alloc, rows);
  if (rc != GDEFSTOR_OK) {
    gdefstor_keyset_free(set);
    return rc;
  }
  *out_set = set;
  return GDEFSTOR_OK;
}

/* Secrets, keys and sealed material by kind and then name, so that a key set
 * reads the same whatever order a backend keeps its rows in; the ledger keeps
 * its own order, oldest first. */
static int row_order(const void * a, const void * b) {
  const GDEFSTOR_KeyRow * x = (const GDEFSTOR_KeyRow *)a;
  const GDEFSTOR_KeyRow * y = (const GDEFSTOR_KeyRow *)b;
  size_t n;
  int c;
  if (x->kind != y->kind) {
    return x->kind < y->kind ? -1 : 1;
  }
  n = x->name_len < y->name_len ? x->name_len : y->name_len;
  c = memcmp(x->name, y->name, n);
  if (c != 0) {
    return c;
  }
  return x->name_len < y->name_len ? -1 : (x->name_len > y->name_len ? 1 : 0);
}

static void canonical_order(GDEFSTOR_KeyRow * rows, size_t n) {
  size_t keep = 0;
  while (keep < n && rows[keep].kind != GDEFSTOR_KEYROW_LEDGER) {
    keep += 1;
  }
  if (keep > 1) {
    qsort(rows, keep, sizeof *rows, row_order);
  }
}

typedef struct DumpCtx {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_KeySet set;
  GDEFSTOR_Result failed;
} DumpCtx;

static GDEFSTOR_Result dump_visit(void * vctx, const GDEFSTOR_KeyRow * row) {
  DumpCtx * d = (DumpCtx *)vctx;
  KRow k;
  GDEFSTOR_Result rc;
  if (row == NULL || row->name == NULL ||
      (row->bytes == NULL && row->len != 0)) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  memset(&k, 0, sizeof k);
  k.kind = row->kind;
  k.name_len = row->name_len;
  k.when_ms = row->when_ms;
  k.len = row->len;
  k.name = gdefstor_dup(d->alloc, row->name, row->name_len);
  if (k.name == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  if (row->len > 0) {
    k.bytes = (uint8_t *)gdefstor_mem(d->alloc, row->len);
    if (k.bytes == NULL) {
      gcu_allocator_free(d->alloc, k.name);
      return GDEFSTOR_ERR_OOM;
    }
    memcpy(k.bytes, row->bytes, row->len);
  }
  rc = set_push(&d->set, &k);
  if (rc != GDEFSTOR_OK) {
    if (k.bytes != NULL) {
      wipe_bytes(k.bytes, k.len);
    }
    gcu_allocator_free(d->alloc, k.bytes);
    gcu_allocator_free(d->alloc, k.name);
  }
  return rc;
}

static void dump_clear(DumpCtx * d) {
  size_t i;
  for (i = 0; i < d->set.n; i++) {
    gcu_allocator_free(d->alloc, d->set.v[i].name);
    if (d->set.v[i].bytes != NULL) {
      wipe_bytes(d->set.v[i].bytes, d->set.v[i].len);
    }
    gcu_allocator_free(d->alloc, d->set.v[i].bytes);
  }
  gcu_allocator_free(d->alloc, d->set.v);
}

GDEFSTOR_Result gdefstor_keystore_export(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, int64_t taken_ms,
    uint8_t ** out_bytes, size_t * out_len) {
  DumpCtx d;
  GDEFSTOR_KeyRow * rows = NULL;
  GDEFSTOR_Result rc;
  if (keystore == NULL || out_bytes == NULL || out_len == NULL ||
      taken_ms < 0 || tenant_len == 0 ||
      !gdefstor_span_ok(tenant, tenant_len, KEYSTORE_TENANT_BYTES)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (!has_v2(keystore)) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  memset(&d, 0, sizeof d);
  d.alloc = keystore->alloc;
  d.set.alloc = keystore->alloc;
  rc = keystore->backend.dump(keystore->backend.ctx, tenant, tenant_len,
      dump_visit, &d);
  if (rc == GDEFSTOR_OK) {
    rc = set_rows(&d.set, &rows);
  }
  if (rc == GDEFSTOR_OK) {
    canonical_order(rows, d.set.n);
  }
  if (rc == GDEFSTOR_OK) {
    /* A backend that dumps out of order, or a row the port would refuse, is
     * the backend's defect: not a key set anyone could import. */
    rc = rows_ok(rows, d.set.n) == GDEFSTOR_OK ? GDEFSTOR_OK
                                                : GDEFSTOR_ERR_INTERNAL;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_keyset_encode(keystore->alloc, taken_ms, rows, d.set.n,
        out_bytes, out_len);
  }
  gcu_allocator_free(keystore->alloc, rows);
  dump_clear(&d);
  return rc;
}

GDEFSTOR_Result gdefstor_keystore_import(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const uint8_t * bytes, size_t len) {
  GDEFSTOR_KeySet * set = NULL;
  GDEFSTOR_KeyRow * rows = NULL;
  GDEFSTOR_Result rc;
  if (keystore == NULL || tenant_len == 0 ||
      !gdefstor_span_ok(tenant, tenant_len, KEYSTORE_TENANT_BYTES) ||
      (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (!has_v2(keystore)) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  rc = gdefstor_keyset_parse(keystore->alloc, bytes, len, &set);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = set_rows(set, &rows);
  if (rc == GDEFSTOR_OK) {
    rc = keystore->backend.restore(keystore->backend.ctx, tenant, tenant_len,
        rows, set->n);
  }
  gcu_allocator_free(keystore->alloc, rows);
  gdefstor_keyset_free(set);
  return rc;
}
