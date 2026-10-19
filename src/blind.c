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
 * The blind index's lookups and the subject's erasure step (model.h, "The
 * blind index and subjects"; AD-14, AD-19): the history lookup by MAC, the
 * subject lookup across every type, and the deletion of a subject's blind
 * rows. The backend finds the rows (model_backend.h); the port checks the
 * arguments, names fields by path and puts the answer in one order, so that
 * every backend reports the same thing.
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static GDEFSTOR_Result ready(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len) {
  GDEFSTOR_Result rc = gdefstor_model_ready(store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  return gdefstor_model_has_blind(store) ? GDEFSTOR_OK
                                         : GDEFSTOR_ERR_UNSUPPORTED;
}

static int cmp_u64(uint64_t a, uint64_t b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

static int cmp_bytes(const void * a, size_t an, const void * b, size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = n == 0 ? 0 : memcmp(a, b, n);
  if (c != 0) {
    return c < 0 ? -1 : 1;
  }
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

/* Reorder @p n elements of @p size bytes at @p base by @p idx. */
static GDEFSTOR_Result permute(const GDEFSTOR_Allocator * alloc, void * base,
    size_t n, size_t size, const size_t * idx) {
  unsigned char * tmp;
  size_t i;
  if (n < 2) {
    return GDEFSTOR_OK;
  }
  tmp = (unsigned char *)gdefstor_mem(alloc, n * size);
  if (tmp == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < n; i++) {
    memcpy(tmp + i * size, (unsigned char *)base + idx[i] * size, size);
  }
  memcpy(base, tmp, n * size);
  gcu_allocator_free(alloc, tmp);
  return GDEFSTOR_OK;
}

/* ---- the history lookup -------------------------------------------------- */

typedef struct BHit {
  uint64_t row;
  char * pid;
  size_t pid_len;
  uint64_t item;
  uint64_t from;
  uint64_t to;
  uint8_t * subject;
  size_t subject_len;
  uint32_t key_id;
  uint32_t version;
} BHit;

struct GDEFSTOR_BlindHits {
  const GDEFSTOR_Allocator * alloc;
  BHit * v;
  size_t n;
  size_t cap;
};

void gdefstor_blind_hits_free(GDEFSTOR_BlindHits * hits) {
  size_t i;
  if (hits == NULL) {
    return;
  }
  for (i = 0; i < hits->n; i++) {
    gcu_allocator_free(hits->alloc, hits->v[i].pid);
    gcu_allocator_free(hits->alloc, hits->v[i].subject);
  }
  gcu_allocator_free(hits->alloc, hits->v);
  gcu_allocator_free(hits->alloc, hits);
}

static GDEFSTOR_Result take_blind(void * ctx, const GDEFSTOR_ModelBlindHit * h) {
  GDEFSTOR_BlindHits * out = (GDEFSTOR_BlindHits *)ctx;
  BHit * o;
  if (h == NULL || (h->public_id == NULL && h->public_id_len != 0) ||
      (h->subject == NULL && h->subject_len != 0) || h->item == 0 ||
      h->valid_from == 0 || (h->valid_to != 0 && h->valid_to <= h->valid_from)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (out->n == out->cap) {
    size_t grown = out->cap == 0 ? 8 : out->cap * 2;
    BHit * next = (BHit *)gcu_allocator_realloc(out->alloc, out->v,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    out->v = next;
    out->cap = grown;
  }
  o = &out->v[out->n];
  memset(o, 0, sizeof *o);
  o->pid = gdefstor_dup(out->alloc, h->public_id, h->public_id_len);
  o->subject = (uint8_t *)gdefstor_dup(out->alloc, (const char *)h->subject,
      h->subject_len);
  if (o->pid == NULL || o->subject == NULL) {
    gcu_allocator_free(out->alloc, o->pid);
    gcu_allocator_free(out->alloc, o->subject);
    return GDEFSTOR_ERR_OOM;
  }
  o->row = h->row;
  o->pid_len = h->public_id_len;
  o->item = h->item;
  o->from = h->valid_from;
  o->to = h->valid_to;
  o->subject_len = h->subject_len;
  o->key_id = h->key_id;
  o->version = h->version;
  out->n += 1;
  return GDEFSTOR_OK;
}

static int bhit_cmp(const void * ctx, size_t a, size_t b) {
  const BHit * v = (const BHit *)ctx;
  int c = cmp_u64(v[a].row, v[b].row);
  if (c == 0) {
    c = cmp_u64(v[a].item, v[b].item);
  }
  if (c == 0) {
    c = cmp_u64(v[a].from, v[b].from);
  }
  return c;
}

GDEFSTOR_Result gdefstor_blind_history(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const char * field, size_t field_len, const uint8_t * mac, size_t mac_len,
    GDEFSTOR_BlindHits ** out_hits) {
  GDEFSTOR_Type * t = NULL;
  GDEFSTOR_BlindHits * hits;
  size_t fi = 0;
  size_t * idx = NULL;
  GDEFSTOR_Result rc;
  if (out_hits == NULL || mac == NULL || mac_len != GDEFSTOR_BLIND_MAC_BYTES ||
      field == NULL || field_len == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = ready(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_type_load(store, NULL, tenant, tenant_len, type,
      type_len, &t);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  /* A personal scalar: the only fields with blind rows. A retired one keeps
   * its rows, and its history is searched too. */
  if (gdefstor_type_find(t, field, field_len, &fi) != GDEFSTOR_OK ||
      t->f[fi].type == GDEFSTOR_FIELD_GROUP ||
      (t->f[fi].flags & GDEFSTOR_FIELD_PERSONAL) == 0) {
    gdefstor_type_free(t);
    return GDEFSTOR_ERR_INVALID;
  }
  hits = (GDEFSTOR_BlindHits *)gdefstor_mem0(store->alloc, sizeof *hits);
  if (hits == NULL) {
    gdefstor_type_free(t);
    return GDEFSTOR_ERR_OOM;
  }
  hits->alloc = store->alloc;
  rc = store->model.blind_history(store->model.blind_ctx, tenant, tenant_len,
      t, fi, mac, take_blind, hits);
  gdefstor_type_free(t);
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(hits->alloc, hits->n, bhit_cmp, hits->v, &idx);
  }
  if (rc == GDEFSTOR_OK) {
    rc = permute(hits->alloc, hits->v, hits->n, sizeof *hits->v, idx);
  }
  gcu_allocator_free(hits->alloc, idx);
  if (rc != GDEFSTOR_OK) {
    gdefstor_blind_hits_free(hits);
    return rc;
  }
  *out_hits = hits;
  return GDEFSTOR_OK;
}

size_t gdefstor_blind_hits_count(const GDEFSTOR_BlindHits * hits) {
  return hits == NULL ? 0 : hits->n;
}

GDEFSTOR_Result gdefstor_blind_hit(const GDEFSTOR_BlindHits * hits,
    size_t index, GDEFSTOR_BlindHit * out) {
  const BHit * h;
  if (hits == NULL || out == NULL || out->size != sizeof *out ||
      index >= hits->n) {
    return GDEFSTOR_ERR_INVALID;
  }
  h = &hits->v[index];
  out->row = h->row;
  out->public_id = h->pid;
  out->public_id_len = h->pid_len;
  out->item = h->item;
  out->valid_from = h->from;
  out->valid_to = h->to;
  out->subject = h->subject;
  out->subject_len = h->subject_len;
  out->key_id = h->key_id;
  out->version = h->version;
  return GDEFSTOR_OK;
}

/* ---- the subject lookup -------------------------------------------------- */

typedef struct SHit {
  char * type;
  size_t type_len;
  uint64_t row;
  char * pid;
  size_t pid_len;
  uint32_t group;
  uint32_t column;
  size_t field;     /* the field's index in its declaration, once resolved */
  char * path;      /* borrowed from a declaration the hits keep */
  size_t path_len;
  uint64_t item;
  uint64_t from;
  uint64_t to;
} SHit;

struct GDEFSTOR_SubjectHits {
  const GDEFSTOR_Allocator * alloc;
  SHit * v;
  size_t n;
  size_t cap;
  GDEFSTOR_Type ** types; /* the declarations the paths point into */
  size_t ntypes;
  size_t captypes;
};

void gdefstor_subject_hits_free(GDEFSTOR_SubjectHits * hits) {
  size_t i;
  if (hits == NULL) {
    return;
  }
  for (i = 0; i < hits->n; i++) {
    gcu_allocator_free(hits->alloc, hits->v[i].type);
    gcu_allocator_free(hits->alloc, hits->v[i].pid);
  }
  for (i = 0; i < hits->ntypes; i++) {
    gdefstor_type_free(hits->types[i]);
  }
  gcu_allocator_free(hits->alloc, hits->types);
  gcu_allocator_free(hits->alloc, hits->v);
  gcu_allocator_free(hits->alloc, hits);
}

static GDEFSTOR_Result take_subject(void * ctx,
    const GDEFSTOR_ModelSubjectHit * h) {
  GDEFSTOR_SubjectHits * out = (GDEFSTOR_SubjectHits *)ctx;
  SHit * o;
  if (h == NULL || h->type == NULL || h->type_len == 0 ||
      (h->public_id == NULL && h->public_id_len != 0) || h->item == 0 ||
      h->valid_from == 0 || (h->valid_to != 0 && h->valid_to <= h->valid_from)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (out->n == out->cap) {
    size_t grown = out->cap == 0 ? 8 : out->cap * 2;
    SHit * next = (SHit *)gcu_allocator_realloc(out->alloc, out->v,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    out->v = next;
    out->cap = grown;
  }
  o = &out->v[out->n];
  memset(o, 0, sizeof *o);
  o->type = gdefstor_dup(out->alloc, h->type, h->type_len);
  o->pid = gdefstor_dup(out->alloc, h->public_id, h->public_id_len);
  if (o->type == NULL || o->pid == NULL) {
    gcu_allocator_free(out->alloc, o->type);
    gcu_allocator_free(out->alloc, o->pid);
    return GDEFSTOR_ERR_OOM;
  }
  o->type_len = h->type_len;
  o->row = h->row;
  o->pid_len = h->public_id_len;
  o->group = h->group;
  o->column = h->column;
  o->item = h->item;
  o->from = h->valid_from;
  o->to = h->valid_to;
  out->n += 1;
  return GDEFSTOR_OK;
}

/* The personal scalar field whose values live in column @p column of group
 * @p group: a single field's column in the group holding it, or a multiple
 * field's one column in its own group. */
static size_t field_at(const GDEFSTOR_Type * t, uint32_t group,
    uint32_t column) {
  size_t i;
  for (i = 0; i < t->n; i++) {
    const GDEFSTOR_MField * f = &t->f[i];
    if (f->type == GDEFSTOR_FIELD_GROUP ||
        (f->flags & GDEFSTOR_FIELD_PERSONAL) == 0) {
      continue;
    }
    if ((f->flags & GDEFSTOR_FIELD_MULTIPLE) != 0
            ? (f->group == group && column == 0)
            : (f->in_group == group && f->column == column)) {
      return i;
    }
  }
  return GDEFSTOR_NONE;
}

/* Name every hit's field by path, reading each type's declaration once. */
static GDEFSTOR_Result name_fields(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, GDEFSTOR_SubjectHits * hits) {
  size_t i;
  for (i = 0; i < hits->n; i++) {
    SHit * h = &hits->v[i];
    const GDEFSTOR_Type * t = NULL;
    size_t k;
    for (k = 0; k < hits->ntypes; k++) {
      if (hits->types[k]->name_len == h->type_len &&
          memcmp(hits->types[k]->name, h->type, h->type_len) == 0) {
        t = hits->types[k];
        break;
      }
    }
    if (t == NULL) {
      GDEFSTOR_Type * loaded = NULL;
      GDEFSTOR_Result rc;
      if (hits->ntypes == hits->captypes) {
        size_t grown = hits->captypes == 0 ? 4 : hits->captypes * 2;
        GDEFSTOR_Type ** next = (GDEFSTOR_Type **)gcu_allocator_realloc(
            hits->alloc, hits->types, grown * sizeof *next);
        if (next == NULL) {
          return GDEFSTOR_ERR_OOM;
        }
        hits->types = next;
        hits->captypes = grown;
      }
      rc = gdefstor_model_type_load(store, NULL, tenant, tenant_len, h->type,
          h->type_len, &loaded);
      if (rc != GDEFSTOR_OK) {
        /* A value of a type with no declaration is damage. */
        return rc == GDEFSTOR_ERR_INVALID ? GDEFSTOR_ERR_CORRUPT : rc;
      }
      hits->types[hits->ntypes++] = loaded;
      t = loaded;
    }
    h->field = field_at(t, h->group, h->column);
    if (h->field == GDEFSTOR_NONE) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    h->path = t->f[h->field].path;
    h->path_len = t->f[h->field].path_len;
  }
  return GDEFSTOR_OK;
}

static int shit_cmp(const void * ctx, size_t a, size_t b) {
  const SHit * v = (const SHit *)ctx;
  int c = cmp_bytes(v[a].type, v[a].type_len, v[b].type, v[b].type_len);
  if (c == 0) {
    c = cmp_u64(v[a].row, v[b].row);
  }
  if (c == 0) {
    c = cmp_u64(v[a].field, v[b].field);
  }
  if (c == 0) {
    c = cmp_u64(v[a].item, v[b].item);
  }
  if (c == 0) {
    c = cmp_u64(v[a].from, v[b].from);
  }
  return c;
}

GDEFSTOR_Result gdefstor_subject_lookup(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, GDEFSTOR_SubjectHits ** out_hits) {
  GDEFSTOR_SubjectHits * hits;
  size_t * idx = NULL;
  GDEFSTOR_Result rc;
  if (out_hits == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = ready(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_model_id_ok(subject, subject_len,
          store->limits.max_public_id_bytes, 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  hits = (GDEFSTOR_SubjectHits *)gdefstor_mem0(store->alloc, sizeof *hits);
  if (hits == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  hits->alloc = store->alloc;
  rc = store->model.subject_lookup(store->model.blind_ctx, tenant, tenant_len,
      (const uint8_t *)subject, subject_len, take_subject, hits);
  if (rc == GDEFSTOR_OK) {
    rc = name_fields(store, tenant, tenant_len, hits);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(hits->alloc, hits->n, shit_cmp, hits->v, &idx);
  }
  if (rc == GDEFSTOR_OK) {
    rc = permute(hits->alloc, hits->v, hits->n, sizeof *hits->v, idx);
  }
  gcu_allocator_free(hits->alloc, idx);
  if (rc != GDEFSTOR_OK) {
    gdefstor_subject_hits_free(hits);
    return rc;
  }
  *out_hits = hits;
  return GDEFSTOR_OK;
}

size_t gdefstor_subject_hits_count(const GDEFSTOR_SubjectHits * hits) {
  return hits == NULL ? 0 : hits->n;
}

GDEFSTOR_Result gdefstor_subject_hit(const GDEFSTOR_SubjectHits * hits,
    size_t index, GDEFSTOR_SubjectHit * out) {
  const SHit * h;
  if (hits == NULL || out == NULL || out->size != sizeof *out ||
      index >= hits->n) {
    return GDEFSTOR_ERR_INVALID;
  }
  h = &hits->v[index];
  out->type = h->type;
  out->type_len = h->type_len;
  out->row = h->row;
  out->public_id = h->pid;
  out->public_id_len = h->pid_len;
  out->field = h->path;
  out->field_len = h->path_len;
  out->item = h->item;
  out->valid_from = h->from;
  out->valid_to = h->to;
  return GDEFSTOR_OK;
}

/* ---- erasure's data-store step ------------------------------------------- */

GDEFSTOR_Result gdefstor_blind_delete_subject(GDEFSTOR_Write * write,
    const char * subject, size_t subject_len) {
  GDEFSTOR_Store * store;
  void * txn = NULL;
  GDEFSTOR_Result rc;
  if (write == NULL || write->store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  store = write->store;
  rc = gdefstor_model_ready(store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_model_has_blind(store)) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  if (!gdefstor_model_id_ok(subject, subject_len,
          write->limits.max_public_id_bytes, 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_model_txn(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = store->model.blind_delete(store->model.blind_ctx, txn,
      (const uint8_t *)subject, subject_len);
  return rc == GDEFSTOR_OK ? rc : gdefstor_model_break(write, rc);
}
