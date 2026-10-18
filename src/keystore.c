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
 * The key store port (keystore.h): argument checks over a backend. The port
 * holds no key and does no cryptography.
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/* A public id, as model.h spells one, of at most this many bytes. */
#define KEYSTORE_ID_BYTES 256u
#define KEYSTORE_TENANT_BYTES 256u

struct GDEFSTOR_KeyStore {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_KeyStoreBackend backend;
};

GDEFSTOR_Result gdefstor_keystore_create(const GDEFSTOR_KeyStoreBackend * backend,
    const GDEFSTOR_Allocator * allocator, GDEFSTOR_KeyStore ** out_keystore) {
  GDEFSTOR_KeyStore * ks;
  if (backend == NULL || out_keystore == NULL ||
      backend->size != sizeof(GDEFSTOR_KeyStoreBackend) ||
      backend->destroy == NULL || backend->create == NULL ||
      backend->wrap == NULL || backend->unwrap == NULL ||
      backend->erase == NULL || backend->ledger == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  ks = (GDEFSTOR_KeyStore *)gdefstor_mem0(allocator, sizeof *ks);
  if (ks == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  ks->alloc = gdefstor_alloc(allocator);
  ks->backend = *backend;
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
