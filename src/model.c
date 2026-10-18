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
 * The entity port (model.h): entities, states, revisions and drafts over a
 * GDEFSTOR_ModelBackend. Everything that decides what a write means is here,
 * once, so that every backend stores the same rows: which items changed, their
 * ids and order keys, the base-revision check, retired fields, as-of trees and
 * differences. A backend only keeps what it is handed (model_backend.h).
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <stdio.h>
#include <string.h>

static int same_span(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

/* ---- the store and the write --------------------------------------------- */

GDEFSTOR_Result gdefstor_model_ready(const GDEFSTOR_Store * store) {
  if (store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  return store->has_model ? GDEFSTOR_OK : GDEFSTOR_ERR_UNSUPPORTED;
}

GDEFSTOR_Result gdefstor_store_create_model(const GDEFSTOR_Backend * backend,
    const GDEFSTOR_ModelBackend * model, const GDEFSTOR_Allocator * allocator,
    const GDEFSTOR_Limits * limits, GDEFSTOR_Store ** out_store) {
  GDEFSTOR_Store * store = NULL;
  GDEFSTOR_Result rc;
  if (model == NULL || out_store == NULL ||
      model->size != sizeof(GDEFSTOR_ModelBackend) || model->destroy == NULL ||
      model->begin == NULL || model->commit == NULL || model->abort == NULL ||
      model->type_load == NULL || model->type_store == NULL ||
      model->entity_find == NULL || model->entity_insert == NULL ||
      model->state_append == NULL || model->apply == NULL ||
      model->history == NULL || model->items == NULL ||
      model->draft_put == NULL || model->draft_get == NULL ||
      model->draft_delete == NULL || model->query == NULL ||
      model->rebuild == NULL || model->dump == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_store_create(backend, allocator, limits, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  store->model = *model;
  store->has_model = 1;
  *out_store = store;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_model_txn(GDEFSTOR_Write * write, void ** out_txn) {
  GDEFSTOR_Store * store;
  GDEFSTOR_Result rc;
  if (write == NULL || !write->alive || write->store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  store = write->store;
  if (!store->has_model) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  if (write->model_broken != GDEFSTOR_OK) {
    return write->model_broken;
  }
  /* Key/bytes rows commit through the other backend; one write is one
   * transaction, so it holds one kind or the other. */
  if (write->head != NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (write->model_txn == NULL) {
    void * txn = NULL;
    rc = store->model.begin(store->model.ctx, write->tenant, write->tenant_len,
        &txn);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    if (txn == NULL) {
      store->model.abort(store->model.ctx, txn);
      return GDEFSTOR_ERR_INTERNAL;
    }
    write->model_txn = txn;
  }
  *out_txn = write->model_txn;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_model_break(GDEFSTOR_Write * write,
    GDEFSTOR_Result rc) {
  if (write->model_broken == GDEFSTOR_OK && rc != GDEFSTOR_OK) {
    write->model_broken = rc;
  }
  return rc;
}

GDEFSTOR_Result gdefstor_model_finish(GDEFSTOR_Write * write, int commit) {
  GDEFSTOR_Store * store = write->store;
  void * txn = write->model_txn;
  GDEFSTOR_Result rc;
  write->model_txn = NULL;
  if (txn == NULL) {
    return GDEFSTOR_OK;
  }
  if (commit && write->model_broken == GDEFSTOR_OK) {
    return store->model.commit(store->model.ctx, txn);
  }
  rc = store->model.abort(store->model.ctx, txn);
  if (commit) {
    return write->model_broken;
  }
  return rc;
}

/* ---- reading rows through a visitor -------------------------------------- */

typedef struct Ent {
  uint64_t row;
  char * type;
  size_t type_len;
  char * pid;
  size_t pid_len;
  char * creator;
  size_t creator_len;
  int64_t created_ms;
  uint64_t revision;
  uint64_t last_item;
  int published;
  int deleted;
} Ent;

static void ent_release(const GDEFSTOR_Allocator * alloc, Ent * e) {
  gcu_allocator_free(alloc, e->type);
  gcu_allocator_free(alloc, e->pid);
  gcu_allocator_free(alloc, e->creator);
  memset(e, 0, sizeof *e);
}

typedef struct EntCatch {
  const GDEFSTOR_Allocator * alloc;
  Ent * out;
  int seen;
} EntCatch;

static GDEFSTOR_Result catch_entity(void * ctx, const GDEFSTOR_ModelEntity * e) {
  EntCatch * c = (EntCatch *)ctx;
  Ent * o = c->out;
  if (c->seen) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  o->type = gdefstor_dup(c->alloc, e->type, e->type_len);
  o->pid = gdefstor_dup(c->alloc, e->public_id, e->public_id_len);
  o->creator = gdefstor_dup(c->alloc, e->creator, e->creator_len);
  if (o->type == NULL || o->pid == NULL || o->creator == NULL) {
    ent_release(c->alloc, o);
    return GDEFSTOR_ERR_OOM;
  }
  o->type_len = e->type_len;
  o->pid_len = e->public_id_len;
  o->creator_len = e->creator_len;
  o->row = e->row;
  o->created_ms = e->created_ms;
  o->revision = e->revision;
  o->last_item = e->last_item;
  o->published = e->published != 0;
  o->deleted = e->deleted != 0;
  c->seen = 1;
  return GDEFSTOR_OK;
}

/* ERR_INVALID when there is no such entity. */
static GDEFSTOR_Result find_entity(GDEFSTOR_Store * store, void * txn,
    const char * tenant, size_t tenant_len, const char * pid, size_t pid_len,
    Ent * out) {
  EntCatch c;
  GDEFSTOR_ModelVisitor v;
  GDEFSTOR_Result rc;
  memset(out, 0, sizeof *out);
  if (!gdefstor_model_id_ok(pid, pid_len, store->limits.max_public_id_bytes,
          0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&c, 0, sizeof c);
  c.alloc = store->alloc;
  c.out = out;
  memset(&v, 0, sizeof v);
  v.ctx = &c;
  v.entity = catch_entity;
  rc = store->model.entity_find(store->model.ctx, txn, tenant, tenant_len, pid,
      pid_len, &v);
  if (rc != GDEFSTOR_OK) {
    ent_release(store->alloc, out);
    return rc;
  }
  if (!c.seen) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  return GDEFSTOR_OK;
}

/* An item version the port owns. */
typedef struct Item {
  uint32_t group;
  uint64_t item;
  uint64_t parent;
  double order;
  uint64_t from;
  uint64_t to;
  GDEFSTOR_Value * vals;
  size_t nvals;
} Item;

typedef struct Items {
  const GDEFSTOR_Allocator * alloc;
  Item * v;
  size_t n;
  size_t cap;
} Items;

static void items_release(Items * it) {
  size_t i;
  size_t k;
  for (i = 0; i < it->n; i++) {
    for (k = 0; k < it->v[i].nvals; k++) {
      gdefstor_model_value_clear(it->alloc, &it->v[i].vals[k]);
    }
    gcu_allocator_free(it->alloc, it->v[i].vals);
  }
  gcu_allocator_free(it->alloc, it->v);
  it->v = NULL;
  it->n = 0;
  it->cap = 0;
}

static GDEFSTOR_Result catch_item(void * ctx, uint64_t row,
    const GDEFSTOR_ModelItem * i) {
  Items * it = (Items *)ctx;
  Item * o;
  size_t k;
  (void)row;
  if (it->n >= GDEFSTOR_MODEL_MAX_ITEMS * 4u) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (it->n == it->cap) {
    size_t grown = it->cap == 0 ? 16 : it->cap * 2;
    Item * next = (Item *)gcu_allocator_realloc(it->alloc, it->v,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    it->v = next;
    it->cap = grown;
  }
  o = &it->v[it->n];
  memset(o, 0, sizeof *o);
  o->group = i->group;
  o->item = i->item;
  o->parent = i->parent;
  o->order = i->order;
  o->from = i->valid_from;
  o->to = i->valid_to;
  if (i->value_count > 0) {
    if (i->values == NULL || i->value_count > GDEFSTOR_MODEL_MAX_FIELDS) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    o->vals = (GDEFSTOR_Value *)gdefstor_mem0(it->alloc,
        i->value_count * sizeof *o->vals);
    if (o->vals == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    for (k = 0; k < i->value_count; k++) {
      GDEFSTOR_Result rc =
          gdefstor_model_value_copy(it->alloc, &o->vals[k], &i->values[k]);
      if (rc != GDEFSTOR_OK) {
        size_t j;
        for (j = 0; j < k; j++) {
          gdefstor_model_value_clear(it->alloc, &o->vals[j]);
        }
        gcu_allocator_free(it->alloc, o->vals);
        return rc;
      }
    }
    o->nvals = i->value_count;
  }
  it->n += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result load_items(GDEFSTOR_Store * store, void * txn,
    const char * tenant, size_t tenant_len, uint64_t row, uint64_t as_of,
    Items * out) {
  GDEFSTOR_ModelVisitor v;
  GDEFSTOR_Result rc;
  memset(out, 0, sizeof *out);
  out->alloc = store->alloc;
  memset(&v, 0, sizeof v);
  v.ctx = out;
  v.item = catch_item;
  rc = store->model.items(store->model.ctx, txn, tenant, tenant_len, row,
      as_of, &v);
  if (rc != GDEFSTOR_OK) {
    items_release(out);
  }
  return rc;
}

static int cmp_u64(uint64_t a, uint64_t b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* A stable merge sort of the indexes 0..n-1 by @p cmp, which C's qsort cannot
 * be given a context for. */
GDEFSTOR_Result gdefstor_model_sort(const GDEFSTOR_Allocator * alloc, size_t n,
    int (*cmp)(const void * ctx, size_t a, size_t b), const void * ctx,
    size_t ** out) {
  size_t * idx = (size_t *)gdefstor_mem(alloc, (n == 0 ? 1 : n) * sizeof *idx);
  size_t * tmp = (size_t *)gdefstor_mem(alloc, (n == 0 ? 1 : n) * sizeof *tmp);
  size_t width;
  size_t i;
  if (idx == NULL || tmp == NULL) {
    gcu_allocator_free(alloc, idx);
    gcu_allocator_free(alloc, tmp);
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < n; i++) {
    idx[i] = i;
  }
  for (width = 1; width < n; width *= 2) {
    size_t lo;
    for (lo = 0; lo < n; lo += 2 * width) {
      size_t mid = lo + width < n ? lo + width : n;
      size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t a = lo;
      size_t b = mid;
      size_t o = lo;
      while (a < mid && b < hi) {
        tmp[o++] = cmp(ctx, idx[b], idx[a]) < 0 ? idx[b++] : idx[a++];
      }
      while (a < mid) {
        tmp[o++] = idx[a++];
      }
      while (b < hi) {
        tmp[o++] = idx[b++];
      }
    }
    memcpy(idx, tmp, n * sizeof *idx);
  }
  gcu_allocator_free(alloc, tmp);
  *out = idx;
  return GDEFSTOR_OK;
}

static int by_item_id(const void * ctx, size_t a, size_t b) {
  const Item * v = (const Item *)ctx;
  return cmp_u64(v[a].item, v[b].item);
}

static int by_parent_id(const void * ctx, size_t a, size_t b) {
  const Item * v = (const Item *)ctx;
  return cmp_u64(v[a].parent, v[b].parent);
}


static GDEFSTOR_Result sort_by_id(const GDEFSTOR_Allocator * alloc,
    const Item * v, size_t n, size_t ** out) {
  return gdefstor_model_sort(alloc, n, by_item_id, v, out);
}

/* The position in @p idx (sorted by id) of item @p id, or GDEFSTOR_NONE. */
static size_t find_id(const Item * v, const size_t * idx, size_t n,
    uint64_t id) {
  size_t lo = 0;
  size_t hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (v[idx[mid]].item < id) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < n && v[idx[lo]].item == id) {
    return idx[lo];
  }
  return GDEFSTOR_NONE;
}

/* ---- checking arguments -------------------------------------------------- */

static GDEFSTOR_Result check_meta(const GDEFSTOR_Store * store,
    const GDEFSTOR_RevisionMeta * meta) {
  if (meta == NULL || meta->size != sizeof(GDEFSTOR_RevisionMeta) ||
      !gdefstor_model_id_ok(meta->who, meta->who_len,
          store->limits.max_public_id_bytes, 1) ||
      (meta->why == NULL && meta->why_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (meta->why_len > GDEFSTOR_MODEL_MAX_WHY_BYTES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  if (!gdefstor_model_utf8_ok((const uint8_t *)meta->why, meta->why_len)) {
    return GDEFSTOR_ERR_INVALID;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result write_store(GDEFSTOR_Write * write,
    GDEFSTOR_Store ** out) {
  if (write == NULL || write->store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out = write->store;
  return gdefstor_model_ready(write->store);
}

static GDEFSTOR_Result read_args(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len) {
  GDEFSTOR_Result rc = gdefstor_model_ready(store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  return GDEFSTOR_OK;
}

/* ---- entities ------------------------------------------------------------ */

static GDEFSTOR_Result draft_exists(GDEFSTOR_Store * store, void * txn,
    const char * tenant, size_t tenant_len, const char * pid, size_t pid_len,
    int * out);

GDEFSTOR_Result gdefstor_entity_create(GDEFSTOR_Write * write,
    const char * type, size_t type_len, const char * public_id,
    size_t public_id_len, const char * creator, size_t creator_len,
    int64_t created_ms, uint64_t * out_row) {
  GDEFSTOR_Store * store;
  GDEFSTOR_Type * t = NULL;
  GDEFSTOR_ModelEntity e;
  Ent existing;
  void * txn = NULL;
  uint64_t row = 0;
  int reserved = 0;
  GDEFSTOR_Result rc = write_store(write, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_model_id_ok(public_id, public_id_len,
          write->limits.max_public_id_bytes, 0) ||
      !gdefstor_model_id_ok(creator, creator_len,
          write->limits.max_public_id_bytes, 1)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_model_txn(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_type_load(store, txn, write->tenant, write->tenant_len,
      type, type_len, &t);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  gdefstor_type_free(t);
  rc = find_entity(store, txn, write->tenant, write->tenant_len, public_id,
      public_id_len, &existing);
  if (rc == GDEFSTOR_OK) {
    ent_release(store->alloc, &existing);
    return GDEFSTOR_ERR_INVALID;
  }
  if (rc != GDEFSTOR_ERR_INVALID) {
    return rc;
  }
  rc = draft_exists(store, txn, write->tenant, write->tenant_len, public_id,
      public_id_len, &reserved);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (reserved) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&e, 0, sizeof e);
  e.type = type;
  e.type_len = type_len;
  e.public_id = public_id;
  e.public_id_len = public_id_len;
  e.creator = creator_len == 0 ? "" : creator;
  e.creator_len = creator_len;
  e.created_ms = created_ms;
  rc = store->model.entity_insert(store->model.ctx, txn, &e, &row);
  if (rc != GDEFSTOR_OK) {
    return gdefstor_model_break(write, rc);
  }
  if (row == 0) {
    return gdefstor_model_break(write, GDEFSTOR_ERR_INTERNAL);
  }
  if (out_row != NULL) {
    *out_row = row;
  }
  return GDEFSTOR_OK;
}

struct GDEFSTOR_Entity {
  const GDEFSTOR_Allocator * alloc;
  Ent e;
};

GDEFSTOR_Result gdefstor_entity_get(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * public_id, size_t public_id_len,
    GDEFSTOR_Entity ** out_entity) {
  GDEFSTOR_Entity * h;
  GDEFSTOR_Result rc = read_args(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (out_entity == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  h = (GDEFSTOR_Entity *)gdefstor_mem0(store->alloc, sizeof *h);
  if (h == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  h->alloc = store->alloc;
  rc = find_entity(store, NULL, tenant, tenant_len, public_id, public_id_len,
      &h->e);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(store->alloc, h);
    return rc;
  }
  *out_entity = h;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_entity_info(const GDEFSTOR_Entity * entity,
    GDEFSTOR_EntityInfo * out) {
  if (entity == NULL || out == NULL || out->size != sizeof *out) {
    return GDEFSTOR_ERR_INVALID;
  }
  out->row = entity->e.row;
  out->type = entity->e.type;
  out->type_len = entity->e.type_len;
  out->public_id = entity->e.pid;
  out->public_id_len = entity->e.pid_len;
  out->creator = entity->e.creator;
  out->creator_len = entity->e.creator_len;
  out->created_ms = entity->e.created_ms;
  out->revision = entity->e.revision;
  out->published = entity->e.published;
  out->deleted = entity->e.deleted;
  return GDEFSTOR_OK;
}

void gdefstor_entity_free(GDEFSTOR_Entity * entity) {
  if (entity == NULL) {
    return;
  }
  ent_release(entity->alloc, &entity->e);
  gcu_allocator_free(entity->alloc, entity);
}

GDEFSTOR_Result gdefstor_entity_set_state(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, int published, int deleted,
    const char * actor, size_t actor_len, int64_t when_ms, int * out_changed) {
  GDEFSTOR_Store * store;
  GDEFSTOR_ModelState s;
  Ent e;
  void * txn = NULL;
  GDEFSTOR_Result rc = write_store(write, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_model_id_ok(actor, actor_len, write->limits.max_public_id_bytes,
          1)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_model_txn(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = find_entity(store, txn, write->tenant, write->tenant_len, public_id,
      public_id_len, &e);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  published = published != 0;
  deleted = deleted != 0;
  if (out_changed != NULL) {
    *out_changed = 0;
  }
  if (e.published == published && e.deleted == deleted) {
    ent_release(store->alloc, &e);
    return GDEFSTOR_OK;
  }
  memset(&s, 0, sizeof s);
  s.published = published;
  s.deleted = deleted;
  s.actor = actor_len == 0 ? "" : actor;
  s.actor_len = actor_len;
  s.when_ms = when_ms;
  rc = store->model.state_append(store->model.ctx, txn, e.row, &s);
  ent_release(store->alloc, &e);
  if (rc != GDEFSTOR_OK) {
    return gdefstor_model_break(write, rc);
  }
  if (out_changed != NULL) {
    *out_changed = 1;
  }
  return GDEFSTOR_OK;
}

/* ---- a save: from a tree to the rows that change -------------------------- */

/* One item of the tree a save will leave current. */
typedef struct Tgt {
  uint64_t id;            /* 0 until assigned */
  uint32_t group;
  size_t field;           /* the field it sits under; GDEFSTOR_NONE: the root */
  size_t parent;          /* a target index; GDEFSTOR_NONE for the root */
  double order;
  int has_order;          /* a key already fixed (revert, carried) */
  GDEFSTOR_Value * cols;  /* by column; bytes borrowed from the tree or base */
  uint32_t ncols;
  size_t base;            /* the current version it continues, or NONE */
} Tgt;

typedef struct Save {
  GDEFSTOR_Store * store;
  const GDEFSTOR_Allocator * alloc;
  const GDEFSTOR_Type * type;
  size_t id_cap;
  int revert;             /* ids from history may come back */
  Tgt * t;
  size_t n;
  size_t cap;
  Items base;             /* the current versions */
  size_t * base_idx;      /* base sorted by id */
  size_t * base_by_parent; /* base sorted by parent id */
  unsigned char * used;   /* base[i] is continued by a target */
  uint64_t base_root;     /* the current root item's id, or 0 */
  uint64_t next_item;     /* the last item id given out */
  /* The rows to write. */
  GDEFSTOR_ModelClose * closes;
  size_t nclose;
  GDEFSTOR_ModelItem * inserts;
  size_t ninsert;
} Save;

static void save_release(Save * s) {
  size_t i;
  for (i = 0; i < s->n; i++) {
    gcu_allocator_free(s->alloc, s->t[i].cols);
  }
  gcu_allocator_free(s->alloc, s->t);
  gcu_allocator_free(s->alloc, s->base_idx);
  gcu_allocator_free(s->alloc, s->base_by_parent);
  gcu_allocator_free(s->alloc, s->used);
  gcu_allocator_free(s->alloc, s->closes);
  gcu_allocator_free(s->alloc, s->inserts);
  items_release(&s->base);
}

static uint32_t group_of(const GDEFSTOR_Type * t, size_t field) {
  return field == GDEFSTOR_NONE ? 0 : t->f[field].group;
}

static GDEFSTOR_Result tgt_new(Save * s, uint64_t id, size_t field,
    size_t parent, size_t * out) {
  Tgt * x;
  uint32_t g = group_of(s->type, field);
  if (s->n >= GDEFSTOR_MODEL_MAX_ITEMS) {
    return GDEFSTOR_ERR_LIMIT;
  }
  if (s->n == s->cap) {
    size_t grown = s->cap == 0 ? 16 : s->cap * 2;
    Tgt * next = (Tgt *)gcu_allocator_realloc(s->alloc, s->t,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    s->t = next;
    s->cap = grown;
  }
  x = &s->t[s->n];
  memset(x, 0, sizeof *x);
  x->id = id;
  x->group = g;
  x->field = field;
  x->parent = parent;
  x->base = GDEFSTOR_NONE;
  x->ncols = s->type->group_ncols[g];
  if (x->ncols > 0) {
    x->cols = (GDEFSTOR_Value *)gdefstor_mem0(s->alloc,
        x->ncols * sizeof *x->cols);
    if (x->cols == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
  }
  *out = s->n;
  s->n += 1;
  return GDEFSTOR_OK;
}

/* A value as stored: only the members its type uses. */
static GDEFSTOR_Value canon(const GDEFSTOR_MField * f, const GDEFSTOR_Value * v) {
  GDEFSTOR_Value c;
  memset(&c, 0, sizeof c);
  c.type = v->type;
  if (v->type == 0) {
    return c;
  }
  if ((f->flags & GDEFSTOR_FIELD_PERSONAL) != 0) {
    c.bytes = v->bytes;
    c.len = v->len;
    c.subject = v->subject;
    c.subject_len = v->subject_len;
    return c;
  }
  switch (f->type) {
  case GDEFSTOR_FIELD_TEXT:
  case GDEFSTOR_FIELD_REFERENCE:
    c.bytes = v->bytes;
    c.len = v->len;
    break;
  case GDEFSTOR_FIELD_FLOAT:
    c.real = v->real == 0.0 ? 0.0 : v->real;
    break;
  default:
    c.integer = v->integer;
    break;
  }
  return c;
}

static const GDEFSTOR_Value * base_col(const Item * b, uint32_t c) {
  static const GDEFSTOR_Value none = {0, 0, 0.0, NULL, 0, NULL, 0};
  return c < b->nvals ? &b->vals[c] : &none;
}

/* The current version of item @p id, or NULL. */
static const Item * base_item(const Save * s, uint64_t id) {
  size_t i = find_id(s->base.v, s->base_idx, s->base.n, id);
  return i == GDEFSTOR_NONE ? NULL : &s->base.v[i];
}

/* An item the tree names under a retired field may only restate what is
 * stored: the same item, under the same parent, with the same values. */
static GDEFSTOR_Result check_restated(Save * s, const GDEFSTOR_Tree * tree,
    size_t node, uint64_t parent_id, size_t fi) {
  const GDEFSTOR_TreeNode * n = &tree->items[node];
  const GDEFSTOR_MField * owner = &s->type->f[fi];
  const Item * b = n->id == 0 ? NULL : base_item(s, n->id);
  size_t k;
  if (b == NULL || b->group != owner->group || b->parent != parent_id) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (k = 0; k < n->nvals; k++) {
    const GDEFSTOR_TreeVal * tv = &n->vals[k];
    size_t cf;
    uint32_t col;
    if (owner->type == GDEFSTOR_FIELD_GROUP) {
      cf = gdefstor_model_type_child(s->type, fi, tv->field, tv->field_len);
      if (cf == GDEFSTOR_NONE || s->type->f[cf].type == GDEFSTOR_FIELD_GROUP ||
          (s->type->f[cf].flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
        return GDEFSTOR_ERR_INVALID;
      }
      col = s->type->f[cf].column;
    } else {
      if (tv->field_len != 0) {
        return GDEFSTOR_ERR_INVALID;
      }
      cf = fi;
      col = 0;
    }
    {
      GDEFSTOR_Value c = canon(&s->type->f[cf], &tv->v);
      if (!gdefstor_model_value_equal(&c, base_col(b, col))) {
        return GDEFSTOR_ERR_INVALID;
      }
    }
  }
  for (k = 0; k < n->nkids; k++) {
    const GDEFSTOR_TreeNode * kid = &tree->items[n->kids[k]];
    size_t kf;
    GDEFSTOR_Result rc;
    if (owner->type != GDEFSTOR_FIELD_GROUP) {
      return GDEFSTOR_ERR_INVALID;
    }
    kf = gdefstor_model_type_child(s->type, fi, kid->field, kid->field_len);
    if (kf == GDEFSTOR_NONE ||
        (s->type->f[kf].type != GDEFSTOR_FIELD_GROUP &&
            (s->type->f[kf].flags & GDEFSTOR_FIELD_MULTIPLE) == 0)) {
      return GDEFSTOR_ERR_INVALID;
    }
    rc = check_restated(s, tree, n->kids[k], n->id, kf);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

/* Resolve tree node @p node, already target @p ti, sitting under @p fi. */
static GDEFSTOR_Result resolve_node(Save * s, const GDEFSTOR_Tree * tree,
    size_t node, size_t ti, size_t fi) {
  const GDEFSTOR_TreeNode * n = &tree->items[node];
  int scalar_item = fi != GDEFSTOR_NONE &&
                    s->type->f[fi].type != GDEFSTOR_FIELD_GROUP;
  uint64_t node_id = ti == 0 ? s->base_root : n->id;
  size_t k;
  GDEFSTOR_Result rc;
  for (k = 0; k < n->nvals; k++) {
    const GDEFSTOR_TreeVal * tv = &n->vals[k];
    size_t cf;
    uint32_t col;
    const GDEFSTOR_MField * f;
    if (scalar_item) {
      if (tv->field_len != 0) {
        return GDEFSTOR_ERR_INVALID;
      }
      cf = fi;
      col = 0;
    } else {
      cf = tv->field_len == 0 ? GDEFSTOR_NONE
                              : gdefstor_model_type_child(s->type, fi,
                                    tv->field, tv->field_len);
      if (cf == GDEFSTOR_NONE || s->type->f[cf].type == GDEFSTOR_FIELD_GROUP ||
          (s->type->f[cf].flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
        return GDEFSTOR_ERR_INVALID;
      }
      col = s->type->f[cf].column;
    }
    f = &s->type->f[cf];
    if (f->eff_retired) {
      /* Restating what is stored is allowed; changing it is not. */
      const Item * b = node_id == 0 ? NULL : base_item(s, node_id);
      GDEFSTOR_Value c = canon(f, &tv->v);
      if (b == NULL || b->group != s->t[ti].group ||
          !gdefstor_model_value_equal(&c, base_col(b, col))) {
        return GDEFSTOR_ERR_INVALID;
      }
      continue;
    }
    rc = gdefstor_model_value_check(f, &tv->v, s->id_cap);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    s->t[ti].cols[col] = canon(f, &tv->v);
  }
  for (k = 0; k < n->nkids; k++) {
    const GDEFSTOR_TreeNode * kid = &tree->items[n->kids[k]];
    size_t kf;
    size_t child;
    if (scalar_item) {
      return GDEFSTOR_ERR_INVALID;
    }
    kf = gdefstor_model_type_child(s->type, fi, kid->field, kid->field_len);
    if (kf == GDEFSTOR_NONE ||
        (s->type->f[kf].type != GDEFSTOR_FIELD_GROUP &&
            (s->type->f[kf].flags & GDEFSTOR_FIELD_MULTIPLE) == 0)) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (s->type->f[kf].eff_retired) {
      if (node_id == 0) {
        return GDEFSTOR_ERR_INVALID;
      }
      rc = check_restated(s, tree, n->kids[k], node_id, kf);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      continue;
    }
    rc = tgt_new(s, kid->id, kf, ti, &child);
    if (rc == GDEFSTOR_OK) {
      rc = resolve_node(s, tree, n->kids[k], child, kf);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

/* Targets by parent, then field, in target order within each: the runs of
 * siblings. */
static int by_parent_field(const void * ctx, size_t a, size_t b) {
  const Tgt * t = (const Tgt *)ctx;
  if (t[a].parent != t[b].parent) {
    return t[a].parent < t[b].parent ? -1 : 1;
  }
  if (t[a].field != t[b].field) {
    return t[a].field < t[b].field ? -1 : 1;
  }
  return 0;
}

static int by_target_id(const void * ctx, size_t a, size_t b) {
  const Tgt * t = (const Tgt *)ctx;
  return cmp_u64(t[a].id, t[b].id);
}

/* Every target that names an id continues that current version; a revert may
 * also bring back an id from history. Ids are unique. */
static GDEFSTOR_Result map_ids(Save * s) {
  size_t i;
  size_t * idx = NULL;
  GDEFSTOR_Result rc;
  for (i = 0; i < s->n; i++) {
    Tgt * x = &s->t[i];
    size_t b;
    if (i == 0) {
      x->id = s->base_root;
    }
    if (x->id == 0) {
      continue;
    }
    b = find_id(s->base.v, s->base_idx, s->base.n, x->id);
    if (b == GDEFSTOR_NONE) {
      if (!s->revert || x->id > s->next_item) {
        return GDEFSTOR_ERR_INVALID;
      }
      continue;
    }
    if (s->used[b] || s->base.v[b].group != x->group) {
      return GDEFSTOR_ERR_INVALID;
    }
    s->used[b] = 1;
    x->base = b;
  }
  /* A revived id must not appear twice either. */
  rc = gdefstor_model_sort(s->alloc, s->n, by_target_id, s->t, &idx);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  for (i = 1; i < s->n; i++) {
    if (s->t[idx[i]].id != 0 && s->t[idx[i]].id == s->t[idx[i - 1]].id) {
      rc = GDEFSTOR_ERR_INVALID;
      break;
    }
  }
  gcu_allocator_free(s->alloc, idx);
  return rc;
}

/* The first position in base_by_parent whose parent is @p id. */
static size_t first_child(const Save * s, uint64_t id) {
  size_t lo = 0;
  size_t hi = s->base.n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (s->base.v[s->base_by_parent[mid]].parent < id) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/* Retired columns keep their stored values, and the items of retired fields
 * under a continued item stay as they are. */
static GDEFSTOR_Result carry_retired(Save * s) {
  size_t i;
  for (i = 0; i < s->n; i++) {
    uint32_t c;
    size_t at;
    uint64_t item;
    if (s->t[i].base == GDEFSTOR_NONE) {
      continue;
    }
    {
      Tgt * x = &s->t[i];
      const Item * b = &s->base.v[x->base];
      for (c = 0; c < x->ncols; c++) {
        size_t cf = s->type->col_field[s->type->col_base[x->group] + c];
        if (s->type->f[cf].eff_retired) {
          x->cols[c] = *base_col(b, c);
        }
      }
      item = b->item;
    }
    for (at = first_child(s, item);
         at < s->base.n && s->base.v[s->base_by_parent[at]].parent == item;
         at++) {
      size_t j = s->base_by_parent[at];
      const Item * k = &s->base.v[j];
      size_t owner;
      size_t nt;
      GDEFSTOR_Result rc;
      if (s->used[j] || k->group == 0 || k->group >= s->type->next_group) {
        continue;
      }
      owner = s->type->group_owner[k->group];
      if (!s->type->f[owner].eff_retired) {
        continue;
      }
      rc = tgt_new(s, k->item, owner, i, &nt);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      s->t[nt].base = j;
      s->t[nt].order = k->order;
      s->t[nt].has_order = 1;
      for (c = 0; c < s->t[nt].ncols; c++) {
        s->t[nt].cols[c] = *base_col(k, c);
      }
      s->used[j] = 1;
      /* Its own items are retired too, and carried when the outer loop
       * reaches it. */
    }
  }
  return GDEFSTOR_OK;
}

/* The subject of a personal value is the named sibling reference's value. */
static int subject_matches(const Save * s, const Tgt * holder,
    size_t group_field, const GDEFSTOR_MField * d, const GDEFSTOR_Value * v) {
  const GDEFSTOR_Type * t = s->type;
  size_t sf = gdefstor_model_type_child(t, group_field, d->subject,
      d->subject_len);
  const GDEFSTOR_Value * ref;
  if (sf == GDEFSTOR_NONE) {
    return 0;
  }
  ref = &holder->cols[t->f[sf].column];
  return ref->type == GDEFSTOR_FIELD_REFERENCE &&
         same_span((const char *)ref->bytes, ref->len,
             (const char *)v->subject, v->subject_len);
}

/* What the declaration requires of each item: required values and items, at
 * most one item of a single field, and a personal value's subject. */
static GDEFSTOR_Result check_required(Save * s) {
  const GDEFSTOR_Type * t = s->type;
  size_t * idx = NULL;
  size_t * counts = NULL;
  size_t i;
  size_t at = 0;
  GDEFSTOR_Result rc;
  rc = gdefstor_model_sort(s->alloc, s->n, by_parent_field, s->t, &idx);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  counts = (size_t *)gdefstor_mem0(s->alloc,
      (t->n == 0 ? 1 : t->n) * sizeof *counts);
  if (counts == NULL) {
    gcu_allocator_free(s->alloc, idx);
    return GDEFSTOR_ERR_OOM;
  }
  /* idx lists the root (parent NONE) last; children of i are a block. */
  for (i = 0; i < s->n && rc == GDEFSTOR_OK; i++) {
    const Tgt * x = &s->t[i];
    size_t f;
    while (at < s->n && s->t[idx[at]].parent < i) {
      at += 1;
    }
    if (x->field != GDEFSTOR_NONE &&
        t->f[x->field].type != GDEFSTOR_FIELD_GROUP) {
      /* An item of a multiple scalar is its value. */
      const GDEFSTOR_MField * d = &t->f[x->field];
      if (x->cols[0].type == 0) {
        rc = GDEFSTOR_ERR_INVALID;
      } else if (!d->eff_retired &&
                 (d->flags & GDEFSTOR_FIELD_PERSONAL) != 0 &&
                 d->subject != NULL &&
                 !subject_matches(s, &s->t[x->parent], d->parent, d,
                     &x->cols[0])) {
        rc = GDEFSTOR_ERR_INVALID;
      }
      continue;
    }
    for (f = 0; f < t->n; f++) {
      counts[f] = 0;
    }
    while (at < s->n && s->t[idx[at]].parent == i) {
      counts[s->t[idx[at]].field] += 1;
      at += 1;
    }
    for (f = 0; f < t->n && rc == GDEFSTOR_OK; f++) {
      const GDEFSTOR_MField * d = &t->f[f];
      if (d->parent != x->field || d->eff_retired) {
        continue;
      }
      if (d->type == GDEFSTOR_FIELD_GROUP ||
          (d->flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
        if (((d->flags & GDEFSTOR_FIELD_MULTIPLE) == 0 && counts[f] > 1) ||
            ((d->flags & GDEFSTOR_FIELD_REQUIRED) != 0 && counts[f] == 0)) {
          rc = GDEFSTOR_ERR_INVALID;
        }
        continue;
      }
      if ((d->flags & GDEFSTOR_FIELD_REQUIRED) != 0 &&
          x->cols[d->column].type == 0) {
        rc = GDEFSTOR_ERR_INVALID;
      }
      if ((d->flags & GDEFSTOR_FIELD_PERSONAL) != 0 && d->subject != NULL &&
          x->cols[d->column].type != 0 &&
          !subject_matches(s, x, x->field, d, &x->cols[d->column])) {
        rc = GDEFSTOR_ERR_INVALID;
      }
    }
  }
  gcu_allocator_free(s->alloc, counts);
  gcu_allocator_free(s->alloc, idx);
  return rc;
}

static void assign_ids(Save * s) {
  size_t i;
  for (i = 0; i < s->n; i++) {
    if (s->t[i].id == 0) {
      s->next_item += 1;
      s->t[i].id = s->next_item;
    }
  }
}

static uint64_t parent_id(const Save * s, const Tgt * x) {
  return x->parent == GDEFSTOR_NONE ? 0 : s->t[x->parent].id;
}

/* Order keys for one run of siblings (targets @p run[0..m), in order). An
 * item that stays under the same parent keeps its key when it is on the
 * longest run of keys already in order; every other item gets a key between
 * its neighbours'. When a gap is too small, the run is renumbered. */
static GDEFSTOR_Result order_run(Save * s, const size_t * run, size_t m) {
  size_t * tail = NULL;   /* LIS: index into run of the tail of each length */
  size_t * prev = NULL;
  unsigned char * keep = NULL;
  size_t len = 0;
  size_t i;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  tail = (size_t *)gdefstor_mem(s->alloc, m * sizeof *tail);
  prev = (size_t *)gdefstor_mem(s->alloc, m * sizeof *prev);
  keep = (unsigned char *)gdefstor_mem0(s->alloc, m);
  if (tail == NULL || prev == NULL || keep == NULL) {
    rc = GDEFSTOR_ERR_OOM;
    goto done;
  }
  for (i = 0; i < m; i++) {
    const Tgt * x = &s->t[run[i]];
    const Item * b;
    double key;
    size_t lo = 0;
    size_t hi = len;
    prev[i] = GDEFSTOR_NONE;
    if (x->base == GDEFSTOR_NONE) {
      continue;
    }
    b = &s->base.v[x->base];
    if (b->parent != parent_id(s, x)) {
      continue;
    }
    key = b->order;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (s->base.v[s->t[run[tail[mid]]].base].order < key) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    prev[i] = lo > 0 ? tail[lo - 1] : GDEFSTOR_NONE;
    tail[lo] = i;
    if (lo == len) {
      len += 1;
    }
  }
  if (len > 0) {
    size_t at = tail[len - 1];
    while (at != GDEFSTOR_NONE) {
      keep[at] = 1;
      at = prev[at];
    }
  }
  for (i = 0; i < m;) {
    size_t j = i;
    double lo_key;
    double hi_key;
    int has_lo;
    int has_hi;
    size_t gap;
    size_t k;
    if (keep[i]) {
      s->t[run[i]].order = s->base.v[s->t[run[i]].base].order;
      i += 1;
      continue;
    }
    while (j < m && !keep[j]) {
      j += 1;
    }
    gap = j - i;
    has_lo = i > 0;
    has_hi = j < m;
    lo_key = has_lo ? s->t[run[i - 1]].order : 0.0;
    hi_key = has_hi ? s->base.v[s->t[run[j]].base].order : 0.0;
    for (k = 0; k < gap; k++) {
      double key;
      if (has_lo && has_hi) {
        key = lo_key + (hi_key - lo_key) * (double)(k + 1) / (double)(gap + 1);
      } else if (has_lo) {
        key = lo_key + (double)(k + 1);
      } else if (has_hi) {
        key = hi_key - (double)(gap - k);
      } else {
        key = (double)(k + 1);
      }
      s->t[run[i + k]].order = key;
    }
    i = j;
  }
  /* Too close, or out of order through rounding: renumber the run. */
  for (i = 1; i < m; i++) {
    double gap = s->t[run[i]].order - s->t[run[i - 1]].order;
    if (!(gap >= GDEFSTOR_MODEL_ORDER_EPSILON)) {
      size_t k;
      for (k = 0; k < m; k++) {
        s->t[run[k]].order = (double)(k + 1);
      }
      break;
    }
  }
done:
  gcu_allocator_free(s->alloc, tail);
  gcu_allocator_free(s->alloc, prev);
  gcu_allocator_free(s->alloc, keep);
  return rc;
}

/* Runs are the targets with one parent and one field, in target order, which
 * is the tree's order. */
static GDEFSTOR_Result assign_orders(Save * s) {
  size_t * idx = NULL;
  size_t * run = NULL;
  size_t i = 0;
  GDEFSTOR_Result rc;
  if (s->n == 0) {
    return GDEFSTOR_OK;
  }
  s->t[0].order = 0.0;
  rc = gdefstor_model_sort(s->alloc, s->n, by_parent_field, s->t, &idx);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  run = (size_t *)gdefstor_mem(s->alloc, s->n * sizeof *run);
  if (run == NULL) {
    gcu_allocator_free(s->alloc, idx);
    return GDEFSTOR_ERR_OOM;
  }
  while (rc == GDEFSTOR_OK && i < s->n) {
    size_t j = i;
    size_t m = 0;
    while (j < s->n && s->t[idx[j]].parent == s->t[idx[i]].parent &&
           s->t[idx[j]].field == s->t[idx[i]].field) {
      if (!s->t[idx[j]].has_order && idx[j] != 0) {
        run[m++] = idx[j];
      }
      j += 1;
    }
    if (m > 0) {
      rc = order_run(s, run, m);
    }
    i = j;
  }
  gcu_allocator_free(s->alloc, run);
  gcu_allocator_free(s->alloc, idx);
  return rc;
}

static int item_same(const Save * s, const Tgt * x, const Item * b) {
  uint32_t c;
  if (b->parent != parent_id(s, x) || b->order != x->order) {
    return 0;
  }
  for (c = 0; c < x->ncols; c++) {
    if (!gdefstor_model_value_equal(&x->cols[c], base_col(b, c))) {
      return 0;
    }
  }
  /* A stored row with more columns than the declaration has is damage. */
  return b->nvals <= x->ncols;
}

/* The closes and inserts that take the current versions to the targets. */
static GDEFSTOR_Result diff_rows(Save * s) {
  size_t i;
  s->closes = (GDEFSTOR_ModelClose *)gdefstor_mem0(s->alloc,
      (s->base.n + 1) * sizeof *s->closes);
  s->inserts = (GDEFSTOR_ModelItem *)gdefstor_mem0(s->alloc,
      (s->n + 1) * sizeof *s->inserts);
  if (s->closes == NULL || s->inserts == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < s->n; i++) {
    const Tgt * x = &s->t[i];
    GDEFSTOR_ModelItem * ins;
    if (x->base != GDEFSTOR_NONE) {
      const Item * b = &s->base.v[x->base];
      if (item_same(s, x, b)) {
        continue;
      }
      s->closes[s->nclose].group = b->group;
      s->closes[s->nclose].item = b->item;
      s->closes[s->nclose].valid_from = b->from;
      s->nclose += 1;
    }
    ins = &s->inserts[s->ninsert];
    ins->group = x->group;
    ins->item = x->id;
    ins->parent = parent_id(s, x);
    ins->order = x->order;
    ins->values = x->cols;
    ins->value_count = x->ncols;
    s->ninsert += 1;
  }
  for (i = 0; i < s->base.n; i++) {
    if (!s->used[i]) {
      s->closes[s->nclose].group = s->base.v[i].group;
      s->closes[s->nclose].item = s->base.v[i].item;
      s->closes[s->nclose].valid_from = s->base.v[i].from;
      s->nclose += 1;
    }
  }
  return GDEFSTOR_OK;
}

/* Load the current versions of an entity (none for a new one) and index them. */
static GDEFSTOR_Result save_begin(Save * s, GDEFSTOR_Store * store,
    const GDEFSTOR_Type * type, void * txn, const char * tenant,
    size_t tenant_len, const Ent * e, size_t id_cap) {
  size_t i;
  GDEFSTOR_Result rc;
  memset(s, 0, sizeof *s);
  s->store = store;
  s->alloc = store->alloc;
  s->type = type;
  s->id_cap = id_cap;
  s->base.alloc = store->alloc;
  s->next_item = e->last_item;
  if (e->row != 0 && e->revision != 0) {
    rc = load_items(store, txn, tenant, tenant_len, e->row, 0, &s->base);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  rc = sort_by_id(s->alloc, s->base.v, s->base.n, &s->base_idx);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_sort(s->alloc, s->base.n, by_parent_id, s->base.v,
      &s->base_by_parent);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  s->used = (unsigned char *)gdefstor_mem0(s->alloc, s->base.n + 1);
  if (s->used == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < s->base.n; i++) {
    const Item * b = &s->base.v[i];
    if (b->item == 0 || b->item > e->last_item || b->to != 0 ||
        b->group >= type->next_group || b->nvals > type->group_ncols[b->group]) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (b->group == 0) {
      if (s->base_root != 0 || b->parent != 0) {
        return GDEFSTOR_ERR_CORRUPT;
      }
      s->base_root = b->item;
    }
    if (i > 0 && s->base.v[s->base_idx[i]].item ==
                     s->base.v[s->base_idx[i - 1]].item) {
      return GDEFSTOR_ERR_CORRUPT;
    }
  }
  if (e->revision != 0 && s->base_root == 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  return GDEFSTOR_OK;
}

/* Everything after the targets exist: ids, retired data, requirements, keys,
 * and the rows. */
static GDEFSTOR_Result save_plan(Save * s) {
  GDEFSTOR_Result rc = map_ids(s);
  if (rc == GDEFSTOR_OK) {
    rc = carry_retired(s);
  }
  if (rc == GDEFSTOR_OK) {
    rc = check_required(s);
  }
  if (rc == GDEFSTOR_OK) {
    assign_ids(s);
    rc = assign_orders(s);
  }
  if (rc == GDEFSTOR_OK) {
    rc = diff_rows(s);
  }
  return rc;
}

/* Write the planned rows as revision @p e->revision + 1. Nothing changed:
 * nothing is written and *out_revision is 0. */
static GDEFSTOR_Result save_apply(Save * s, GDEFSTOR_Write * write,
    void * txn, uint64_t row, uint64_t head, const GDEFSTOR_RevisionMeta * meta,
    uint64_t * out_revision) {
  GDEFSTOR_ModelRevision r;
  GDEFSTOR_ModelApply a;
  GDEFSTOR_Result rc;
  if (out_revision != NULL) {
    *out_revision = 0;
  }
  if (s->nclose == 0 && s->ninsert == 0) {
    return GDEFSTOR_OK;
  }
  if (head == UINT64_MAX) {
    return GDEFSTOR_ERR_LIMIT;
  }
  memset(&r, 0, sizeof r);
  r.revision = head + 1;
  r.who = meta->who_len == 0 ? "" : meta->who;
  r.who_len = meta->who_len;
  r.when_ms = meta->when_ms;
  r.why = meta->why_len == 0 ? "" : meta->why;
  r.why_len = meta->why_len;
  memset(&a, 0, sizeof a);
  a.row = row;
  a.revision = &r;
  a.last_item = s->next_item;
  a.closes = s->closes;
  a.close_count = s->nclose;
  a.inserts = s->inserts;
  a.insert_count = s->ninsert;
  {
    size_t i;
    for (i = 0; i < s->ninsert; i++) {
      s->inserts[i].valid_from = r.revision;
      s->inserts[i].valid_to = 0;
    }
  }
  rc = write->store->model.apply(write->store->model.ctx, txn, &a);
  if (rc != GDEFSTOR_OK) {
    return gdefstor_model_break(write, rc);
  }
  if (out_revision != NULL) {
    *out_revision = r.revision;
  }
  return GDEFSTOR_OK;
}

/* The entity, its type and its write, for a save, a revert or an apply.
 * ERR_CONFLICT when the head is not @p base_revision. */
static GDEFSTOR_Result open_save(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    const GDEFSTOR_RevisionMeta * meta, void ** txn, Ent * e,
    GDEFSTOR_Type ** type) {
  GDEFSTOR_Store * store;
  GDEFSTOR_Result rc = write_store(write, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = check_meta(store, meta);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_txn(write, txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = find_entity(store, *txn, write->tenant, write->tenant_len, public_id,
      public_id_len, e);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  /* Linear history: every write builds on the newest revision. */
  if (e->revision != base_revision) {
    ent_release(store->alloc, e);
    return GDEFSTOR_ERR_CONFLICT;
  }
  rc = gdefstor_model_type_load(store, *txn, write->tenant, write->tenant_len,
      e->type, e->type_len, type);
  if (rc != GDEFSTOR_OK) {
    ent_release(store->alloc, e);
    return rc == GDEFSTOR_ERR_INVALID ? GDEFSTOR_ERR_CORRUPT : rc;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result save_tree(GDEFSTOR_Write * write, void * txn,
    const Ent * e, const GDEFSTOR_Type * type, const GDEFSTOR_Tree * tree,
    const GDEFSTOR_RevisionMeta * meta, uint64_t * out_revision) {
  GDEFSTOR_Store * store = write->store;
  Save s;
  size_t root;
  GDEFSTOR_Result rc;
  rc = save_begin(&s, store, type, txn, write->tenant, write->tenant_len, e,
      write->limits.max_public_id_bytes);
  if (rc == GDEFSTOR_OK) {
    rc = tgt_new(&s, 0, GDEFSTOR_NONE, GDEFSTOR_NONE, &root);
  }
  if (rc == GDEFSTOR_OK) {
    rc = resolve_node(&s, tree, 0, root, GDEFSTOR_NONE);
  }
  if (rc == GDEFSTOR_OK) {
    rc = save_plan(&s);
  }
  if (rc == GDEFSTOR_OK) {
    rc = save_apply(&s, write, txn, e->row, e->revision, meta, out_revision);
  }
  save_release(&s);
  return rc;
}

GDEFSTOR_Result gdefstor_entity_save(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    const GDEFSTOR_RevisionMeta * meta, const GDEFSTOR_Tree * tree,
    uint64_t * out_revision) {
  void * txn = NULL;
  Ent e;
  GDEFSTOR_Type * type = NULL;
  GDEFSTOR_Result rc;
  if (tree == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = open_save(write, public_id, public_id_len, base_revision, meta, &txn,
      &e, &type);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = save_tree(write, txn, &e, type, tree, meta, out_revision);
  gdefstor_type_free(type);
  ent_release(write->store->alloc, &e);
  return rc;
}

/* ---- reverting ----------------------------------------------------------- */

GDEFSTOR_Result gdefstor_entity_revert(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    uint64_t to_revision, const GDEFSTOR_RevisionMeta * meta,
    uint64_t * out_revision) {
  void * txn = NULL;
  Ent e;
  GDEFSTOR_Type * type = NULL;
  Items old;
  size_t * by_parent = NULL;
  size_t * stack_items = NULL;
  size_t * stack_tgts = NULL;
  Save s;
  GDEFSTOR_Result rc;
  rc = open_save(write, public_id, public_id_len, base_revision, meta, &txn,
      &e, &type);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&old, 0, sizeof old);
  old.alloc = write->store->alloc;
  rc = save_begin(&s, write->store, type, txn, write->tenant,
      write->tenant_len, &e, write->limits.max_public_id_bytes);
  s.revert = 1;
  if (rc == GDEFSTOR_OK && (to_revision == 0 || to_revision > e.revision)) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  if (rc == GDEFSTOR_OK) {
    rc = load_items(write->store, txn, write->tenant, write->tenant_len, e.row,
        to_revision, &old);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(s.alloc, old.n, by_parent_id, old.v, &by_parent);
  }
  if (rc == GDEFSTOR_OK) {
    stack_items = (size_t *)gdefstor_mem(s.alloc, (old.n + 1) * sizeof(size_t));
    stack_tgts = (size_t *)gdefstor_mem(s.alloc, (old.n + 1) * sizeof(size_t));
    if (stack_items == NULL || stack_tgts == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  /* The past tree, parents first, as targets with their old ids and keys.
   * Retired fields are left out here: carry_retired() keeps the current ones. */
  if (rc == GDEFSTOR_OK) {
    size_t top = 0;
    size_t i;
    size_t root = GDEFSTOR_NONE;
    for (i = 0; i < old.n; i++) {
      if (old.v[i].group == 0) {
        root = root == GDEFSTOR_NONE ? i : old.n;
      }
    }
    if (root == GDEFSTOR_NONE || root == old.n) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc == GDEFSTOR_OK) {
      stack_items[top] = root;
      stack_tgts[top] = GDEFSTOR_NONE;
      top = 1;
    }
    while (rc == GDEFSTOR_OK && top > 0) {
      size_t oi;
      size_t parent_t;
      size_t field;
      size_t ti;
      size_t lo;
      size_t hi;
      uint32_t c;
      const Item * x;
      top -= 1;
      oi = stack_items[top];
      parent_t = stack_tgts[top];
      x = &old.v[oi];
      if (x->group >= type->next_group) {
        rc = GDEFSTOR_ERR_CORRUPT;
        break;
      }
      field = type->group_owner[x->group];
      if (field != GDEFSTOR_NONE && type->f[field].eff_retired) {
        continue;
      }
      rc = tgt_new(&s, x->item, field, parent_t, &ti);
      if (rc != GDEFSTOR_OK) {
        break;
      }
      s.t[ti].has_order = 1;
      s.t[ti].order = x->order;
      for (c = 0; c < s.t[ti].ncols; c++) {
        s.t[ti].cols[c] = *base_col(x, c);
      }
      if (find_id(s.base.v, s.base_idx, s.base.n, x->item) == GDEFSTOR_NONE) {
        /* Brought back: its retired columns are not restored either. */
        for (c = 0; c < s.t[ti].ncols; c++) {
          size_t cf = type->col_field[type->col_base[x->group] + c];
          if (type->f[cf].eff_retired) {
            memset(&s.t[ti].cols[c], 0, sizeof s.t[ti].cols[c]);
          }
        }
      }
      lo = 0;
      hi = old.n;
      while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (old.v[by_parent[mid]].parent < x->item) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      for (; lo < old.n && old.v[by_parent[lo]].parent == x->item; lo++) {
        if (top >= old.n) {
          rc = GDEFSTOR_ERR_CORRUPT;
          break;
        }
        stack_items[top] = by_parent[lo];
        stack_tgts[top] = ti;
        top += 1;
      }
    }
    if (rc == GDEFSTOR_OK && s.n > 0 && s.t[0].id != s.base_root) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = save_plan(&s);
  }
  if (rc == GDEFSTOR_OK) {
    rc = save_apply(&s, write, txn, e.row, e.revision, meta, out_revision);
  }
  gcu_allocator_free(s.alloc, by_parent);
  gcu_allocator_free(s.alloc, stack_items);
  gcu_allocator_free(s.alloc, stack_tgts);
  items_release(&old);
  save_release(&s);
  gdefstor_type_free(type);
  ent_release(write->store->alloc, &e);
  return rc;
}

/* ---- reading trees ------------------------------------------------------- */

typedef struct Build {
  const GDEFSTOR_Type * type;
  const Items * it;
  size_t * by_parent;
  GDEFSTOR_Tree * tree;
  const GDEFSTOR_Allocator * alloc;
} Build;

typedef struct Kid {
  size_t item;
  size_t field;
} Kid;

static int kid_before(const Build * b, const Kid * x, const Kid * y) {
  const Item * a = &b->it->v[x->item];
  const Item * c = &b->it->v[y->item];
  if (x->field != y->field) {
    return x->field < y->field;
  }
  if (a->order != c->order) {
    return a->order < c->order;
  }
  return a->item < c->item;
}

static GDEFSTOR_Result build_node(Build * b, size_t oi, size_t node,
    size_t field, size_t depth) {
  const Item * x = &b->it->v[oi];
  const GDEFSTOR_Type * t = b->type;
  uint32_t c;
  size_t lo = 0;
  size_t hi = b->it->n;
  size_t first;
  size_t count = 0;
  Kid * kids;
  size_t k;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (depth > GDEFSTOR_MODEL_MAX_DEPTH + 2) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  for (c = 0; c < x->nvals; c++) {
    size_t cf;
    const GDEFSTOR_MField * f;
    if (x->vals[c].type == 0) {
      continue;
    }
    cf = t->col_field[t->col_base[x->group] + c];
    f = &t->f[cf];
    if (x->vals[c].type != f->type) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = (f->type != GDEFSTOR_FIELD_GROUP && field == cf)
             ? gdefstor_model_tree_put(b->tree, node, NULL, 0, &x->vals[c])
             : gdefstor_model_tree_put(b->tree, node, f->name, f->name_len,
                   &x->vals[c]);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (b->it->v[b->by_parent[mid]].parent < x->item) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  first = lo;
  while (lo < b->it->n && b->it->v[b->by_parent[lo]].parent == x->item) {
    lo += 1;
    count += 1;
  }
  if (count == 0) {
    return GDEFSTOR_OK;
  }
  kids = (Kid *)gdefstor_mem(b->alloc, count * sizeof *kids);
  if (kids == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (k = 0; k < count; k++) {
    const Item * y = &b->it->v[b->by_parent[first + k]];
    size_t owner;
    if (y->group == 0 || y->group >= t->next_group) {
      gcu_allocator_free(b->alloc, kids);
      return GDEFSTOR_ERR_CORRUPT;
    }
    owner = t->group_owner[y->group];
    if (t->f[owner].parent != field ||
        (field != GDEFSTOR_NONE && t->f[field].type != GDEFSTOR_FIELD_GROUP)) {
      gcu_allocator_free(b->alloc, kids);
      return GDEFSTOR_ERR_CORRUPT;
    }
    kids[k].item = b->by_parent[first + k];
    kids[k].field = owner;
  }
  /* Insertion sort: siblings are few next to the cost of reading them. */
  for (k = 1; k < count; k++) {
    Kid v = kids[k];
    size_t j = k;
    while (j > 0 && kid_before(b, &v, &kids[j - 1])) {
      kids[j] = kids[j - 1];
      j -= 1;
    }
    kids[j] = v;
  }
  for (k = 0; k < count && rc == GDEFSTOR_OK; k++) {
    const Item * y = &b->it->v[kids[k].item];
    const GDEFSTOR_MField * f = &t->f[kids[k].field];
    size_t at;
    rc = gdefstor_model_tree_push(b->tree, node, f->name, f->name_len, y->item,
        y->order, &at);
    if (rc == GDEFSTOR_OK) {
      rc = build_node(b, kids[k].item, at, kids[k].field, depth + 1);
    }
  }
  gcu_allocator_free(b->alloc, kids);
  return rc;
}

static GDEFSTOR_Result build_tree(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Type * type, const Items * it, GDEFSTOR_Tree ** out) {
  Build b;
  size_t i;
  size_t root = GDEFSTOR_NONE;
  GDEFSTOR_Result rc;
  for (i = 0; i < it->n; i++) {
    if (it->v[i].group == 0) {
      if (root != GDEFSTOR_NONE || it->v[i].parent != 0) {
        return GDEFSTOR_ERR_CORRUPT;
      }
      root = i;
    }
  }
  if (root == GDEFSTOR_NONE) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  memset(&b, 0, sizeof b);
  b.type = type;
  b.it = it;
  b.alloc = alloc;
  rc = gdefstor_model_sort(alloc, it->n, by_parent_id, it->v, &b.by_parent);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_tree_create(alloc, &b.tree);
  if (rc == GDEFSTOR_OK) {
    b.tree->items[0].id = it->v[root].item;
    rc = build_node(&b, root, 0, GDEFSTOR_NONE, 0);
  }
  /* Every item must hang from the root: anything else is damage. */
  if (rc == GDEFSTOR_OK && b.tree->n != it->n) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  gcu_allocator_free(alloc, b.by_parent);
  if (rc != GDEFSTOR_OK) {
    gdefstor_tree_free(b.tree);
    return rc;
  }
  *out = b.tree;
  return GDEFSTOR_OK;
}

/* The entity and its type, as of the last commit. */
static GDEFSTOR_Result open_read(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * public_id, size_t public_id_len, Ent * e,
    GDEFSTOR_Type ** type) {
  GDEFSTOR_Result rc = read_args(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = find_entity(store, NULL, tenant, tenant_len, public_id, public_id_len,
      e);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (type == NULL) {
    return GDEFSTOR_OK;
  }
  rc = gdefstor_model_type_load(store, NULL, tenant, tenant_len, e->type,
      e->type_len, type);
  if (rc != GDEFSTOR_OK) {
    ent_release(store->alloc, e);
    return rc == GDEFSTOR_ERR_INVALID ? GDEFSTOR_ERR_CORRUPT : rc;
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_entity_tree(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, uint64_t revision, GDEFSTOR_Tree ** out_tree) {
  Ent e;
  GDEFSTOR_Type * type = NULL;
  Items it;
  GDEFSTOR_Result rc;
  if (out_tree == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = open_read(store, tenant, tenant_len, public_id, public_id_len, &e,
      &type);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (e.revision == 0 || revision > e.revision) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  if (rc == GDEFSTOR_OK) {
    /* 0 reads the current tables; a number reads history. */
    rc = load_items(store, NULL, tenant, tenant_len, e.row, revision, &it);
  }
  if (rc == GDEFSTOR_OK) {
    rc = build_tree(store->alloc, type, &it, out_tree);
    items_release(&it);
  }
  gdefstor_type_free(type);
  ent_release(store->alloc, &e);
  return rc;
}

/* ---- history ------------------------------------------------------------- */

struct GDEFSTOR_History {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_ModelRevision * revs;
  size_t nrev;
  size_t caprev;
  GDEFSTOR_ModelState * states;
  size_t nstate;
  size_t capstate;
};

void gdefstor_history_free(GDEFSTOR_History * h) {
  size_t i;
  if (h == NULL) {
    return;
  }
  for (i = 0; i < h->nrev; i++) {
    gcu_allocator_free(h->alloc, (void *)h->revs[i].who);
    gcu_allocator_free(h->alloc, (void *)h->revs[i].why);
  }
  for (i = 0; i < h->nstate; i++) {
    gcu_allocator_free(h->alloc, (void *)h->states[i].actor);
  }
  gcu_allocator_free(h->alloc, h->revs);
  gcu_allocator_free(h->alloc, h->states);
  gcu_allocator_free(h->alloc, h);
}

static GDEFSTOR_Result catch_revision(void * ctx, uint64_t row,
    const GDEFSTOR_ModelRevision * r) {
  GDEFSTOR_History * h = (GDEFSTOR_History *)ctx;
  GDEFSTOR_ModelRevision * o;
  (void)row;
  if (h->nrev == h->caprev) {
    size_t grown = h->caprev == 0 ? 8 : h->caprev * 2;
    GDEFSTOR_ModelRevision * next = (GDEFSTOR_ModelRevision *)
        gcu_allocator_realloc(h->alloc, h->revs, grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    h->revs = next;
    h->caprev = grown;
  }
  o = &h->revs[h->nrev];
  *o = *r;
  o->who = gdefstor_dup(h->alloc, r->who, r->who_len);
  o->why = gdefstor_dup(h->alloc, r->why, r->why_len);
  if (o->who == NULL || o->why == NULL) {
    gcu_allocator_free(h->alloc, (void *)o->who);
    gcu_allocator_free(h->alloc, (void *)o->why);
    return GDEFSTOR_ERR_OOM;
  }
  h->nrev += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result catch_state(void * ctx, uint64_t row,
    const GDEFSTOR_ModelState * s) {
  GDEFSTOR_History * h = (GDEFSTOR_History *)ctx;
  GDEFSTOR_ModelState * o;
  (void)row;
  if (h->nstate == h->capstate) {
    size_t grown = h->capstate == 0 ? 8 : h->capstate * 2;
    GDEFSTOR_ModelState * next = (GDEFSTOR_ModelState *)gcu_allocator_realloc(
        h->alloc, h->states, grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    h->states = next;
    h->capstate = grown;
  }
  o = &h->states[h->nstate];
  *o = *s;
  o->actor = gdefstor_dup(h->alloc, s->actor, s->actor_len);
  if (o->actor == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  h->nstate += 1;
  return GDEFSTOR_OK;
}

static int rev_cmp(const void * ctx, size_t a, size_t b) {
  const GDEFSTOR_ModelRevision * r = (const GDEFSTOR_ModelRevision *)ctx;
  return cmp_u64(r[a].revision, r[b].revision);
}

static int state_cmp(const void * ctx, size_t a, size_t b) {
  const GDEFSTOR_ModelState * s = (const GDEFSTOR_ModelState *)ctx;
  return cmp_u64(s[a].seq, s[b].seq);
}

/* Put @p n elements of @p size bytes at @p base into the order of @p idx. */
static GDEFSTOR_Result permute(const GDEFSTOR_Allocator * alloc, void * base,
    size_t n, size_t size, const size_t * idx) {
  unsigned char * tmp = (unsigned char *)gdefstor_mem(alloc, n * size + 1);
  size_t i;
  if (tmp == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < n; i++) {
    memcpy(tmp + i * size, (unsigned char *)base + idx[i] * size, size);
  }
  if (n > 0) {
    memcpy(base, tmp, n * size);
  }
  gcu_allocator_free(alloc, tmp);
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_entity_history(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, GDEFSTOR_History ** out_history) {
  Ent e;
  GDEFSTOR_History * h;
  GDEFSTOR_ModelVisitor v;
  size_t * idx = NULL;
  size_t i;
  GDEFSTOR_Result rc;
  if (out_history == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = open_read(store, tenant, tenant_len, public_id, public_id_len, &e,
      NULL);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  h = (GDEFSTOR_History *)gdefstor_mem0(store->alloc, sizeof *h);
  if (h == NULL) {
    ent_release(store->alloc, &e);
    return GDEFSTOR_ERR_OOM;
  }
  h->alloc = store->alloc;
  memset(&v, 0, sizeof v);
  v.ctx = h;
  v.revision = catch_revision;
  v.state = catch_state;
  rc = store->model.history(store->model.ctx, NULL, tenant, tenant_len, e.row,
      &v);
  ent_release(store->alloc, &e);
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(h->alloc, h->nrev, rev_cmp, h->revs, &idx);
  }
  if (rc == GDEFSTOR_OK) {
    rc = permute(h->alloc, h->revs, h->nrev, sizeof *h->revs, idx);
    gcu_allocator_free(h->alloc, idx);
    idx = NULL;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(h->alloc, h->nstate, state_cmp, h->states, &idx);
  }
  if (rc == GDEFSTOR_OK) {
    rc = permute(h->alloc, h->states, h->nstate, sizeof *h->states, idx);
    gcu_allocator_free(h->alloc, idx);
  }
  /* Numbers are 1, 2, 3 ... with none missing: anything else is damage. */
  for (i = 0; rc == GDEFSTOR_OK && i < h->nrev; i++) {
    if (h->revs[i].revision != i + 1) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  for (i = 0; rc == GDEFSTOR_OK && i < h->nstate; i++) {
    if (h->states[i].seq != i + 1) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  if (rc != GDEFSTOR_OK) {
    gdefstor_history_free(h);
    return rc;
  }
  *out_history = h;
  return GDEFSTOR_OK;
}

size_t gdefstor_history_revision_count(const GDEFSTOR_History * h) {
  return h == NULL ? 0 : h->nrev;
}

GDEFSTOR_Result gdefstor_history_revision(const GDEFSTOR_History * h,
    size_t index, GDEFSTOR_RevisionInfo * out) {
  const GDEFSTOR_ModelRevision * r;
  if (h == NULL || out == NULL || out->size != sizeof *out ||
      index >= h->nrev) {
    return GDEFSTOR_ERR_INVALID;
  }
  r = &h->revs[index];
  out->revision = r->revision;
  out->commit_seq = r->commit_seq;
  out->who = r->who;
  out->who_len = r->who_len;
  out->when_ms = r->when_ms;
  out->why = r->why;
  out->why_len = r->why_len;
  return GDEFSTOR_OK;
}

size_t gdefstor_history_state_count(const GDEFSTOR_History * h) {
  return h == NULL ? 0 : h->nstate;
}

GDEFSTOR_Result gdefstor_history_state(const GDEFSTOR_History * h,
    size_t index, GDEFSTOR_StateInfo * out) {
  const GDEFSTOR_ModelState * s;
  if (h == NULL || out == NULL || out->size != sizeof *out ||
      index >= h->nstate) {
    return GDEFSTOR_ERR_INVALID;
  }
  s = &h->states[index];
  out->published = s->published;
  out->deleted = s->deleted;
  out->actor = s->actor;
  out->actor_len = s->actor_len;
  out->when_ms = s->when_ms;
  return GDEFSTOR_OK;
}

/* ---- differences --------------------------------------------------------- */

typedef struct DItem {
  int kind;
  unsigned what;
  uint64_t item;
  size_t field; /* GDEFSTOR_NONE: the root */
} DItem;

struct GDEFSTOR_Diff {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_Type * type;
  DItem * v;
  size_t n;
};

void gdefstor_diff_free(GDEFSTOR_Diff * diff) {
  if (diff == NULL) {
    return;
  }
  gdefstor_type_free(diff->type);
  gcu_allocator_free(diff->alloc, diff->v);
  gcu_allocator_free(diff->alloc, diff);
}

static unsigned item_delta(const Item * a, const Item * b) {
  unsigned what = 0;
  size_t c;
  size_t n = a->nvals > b->nvals ? a->nvals : b->nvals;
  if (a->parent != b->parent) {
    what |= GDEFSTOR_DIFF_PARENT;
  }
  if (a->order != b->order) {
    what |= GDEFSTOR_DIFF_ORDER;
  }
  for (c = 0; c < n; c++) {
    if (!gdefstor_model_value_equal(base_col(a, (uint32_t)c),
            base_col(b, (uint32_t)c))) {
      what |= GDEFSTOR_DIFF_VALUES;
    }
  }
  return what;
}

GDEFSTOR_Result gdefstor_entity_diff(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, uint64_t from, uint64_t to,
    GDEFSTOR_Diff ** out_diff) {
  Ent e;
  GDEFSTOR_Type * type = NULL;
  Items a;
  Items b;
  size_t * ia = NULL;
  size_t * ib = NULL;
  GDEFSTOR_Diff * d = NULL;
  size_t i;
  size_t j;
  GDEFSTOR_Result rc;
  if (out_diff == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = open_read(store, tenant, tenant_len, public_id, public_id_len, &e,
      &type);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  a.alloc = store->alloc;
  b.alloc = store->alloc;
  if (from > e.revision || to > e.revision) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  /* Revision 0 is the empty tree; for the backend 0 would mean "current". */
  if (rc == GDEFSTOR_OK && from > 0) {
    rc = load_items(store, NULL, tenant, tenant_len, e.row, from, &a);
  }
  if (rc == GDEFSTOR_OK && to > 0) {
    rc = load_items(store, NULL, tenant, tenant_len, e.row, to, &b);
  }
  if (rc == GDEFSTOR_OK) {
    rc = sort_by_id(store->alloc, a.v, a.n, &ia);
  }
  if (rc == GDEFSTOR_OK) {
    rc = sort_by_id(store->alloc, b.v, b.n, &ib);
  }
  if (rc == GDEFSTOR_OK) {
    d = (GDEFSTOR_Diff *)gdefstor_mem0(store->alloc, sizeof *d);
    if (d != NULL) {
      d->alloc = store->alloc;
      d->v = (DItem *)gdefstor_mem0(store->alloc,
          (a.n + b.n + 1) * sizeof *d->v);
    }
    if (d == NULL || d->v == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  /* A merge of the two id-sorted lists. */
  i = 0;
  j = 0;
  while (rc == GDEFSTOR_OK && (i < a.n || j < b.n)) {
    const Item * x = i < a.n ? &a.v[ia[i]] : NULL;
    const Item * y = j < b.n ? &b.v[ib[j]] : NULL;
    DItem * o = &d->v[d->n];
    const Item * which;
    if (y == NULL || (x != NULL && x->item < y->item)) {
      o->kind = GDEFSTOR_DIFF_REMOVED;
      which = x;
      i += 1;
    } else if (x == NULL || y->item < x->item) {
      o->kind = GDEFSTOR_DIFF_ADDED;
      which = y;
      j += 1;
    } else {
      unsigned what = item_delta(x, y);
      i += 1;
      j += 1;
      if (what == 0) {
        continue;
      }
      o->kind = GDEFSTOR_DIFF_CHANGED;
      o->what = what;
      which = y;
    }
    if (which->group >= type->next_group) {
      rc = GDEFSTOR_ERR_CORRUPT;
      break;
    }
    o->item = which->item;
    o->field = type->group_owner[which->group];
    d->n += 1;
  }
  gcu_allocator_free(store->alloc, ia);
  gcu_allocator_free(store->alloc, ib);
  items_release(&a);
  items_release(&b);
  ent_release(store->alloc, &e);
  if (rc != GDEFSTOR_OK) {
    gdefstor_type_free(type);
    if (d != NULL) {
      d->type = NULL;
      gdefstor_diff_free(d);
    }
    return rc;
  }
  d->type = type;
  *out_diff = d;
  return GDEFSTOR_OK;
}

size_t gdefstor_diff_count(const GDEFSTOR_Diff * diff) {
  return diff == NULL ? 0 : diff->n;
}

GDEFSTOR_Result gdefstor_diff_item(const GDEFSTOR_Diff * diff, size_t index,
    GDEFSTOR_DiffItem * out) {
  const DItem * x;
  if (diff == NULL || out == NULL || out->size != sizeof *out ||
      index >= diff->n) {
    return GDEFSTOR_ERR_INVALID;
  }
  x = &diff->v[index];
  out->kind = x->kind;
  out->what = x->what;
  out->item = x->item;
  if (x->field == GDEFSTOR_NONE) {
    out->field = "";
    out->field_len = 0;
  } else {
    out->field = diff->type->f[x->field].path;
    out->field_len = diff->type->f[x->field].path_len;
  }
  return GDEFSTOR_OK;
}

/* ---- drafts -------------------------------------------------------------- */

typedef struct DraftCatch {
  const GDEFSTOR_Allocator * alloc;
  int seen;
  char * type;
  size_t type_len;
  uint64_t base;
  uint8_t * json;
  size_t json_len;
} DraftCatch;

static void draft_catch_release(DraftCatch * c) {
  gcu_allocator_free(c->alloc, c->type);
  gcu_allocator_free(c->alloc, c->json);
  c->type = NULL;
  c->json = NULL;
}

static GDEFSTOR_Result catch_draft(void * ctx, const GDEFSTOR_ModelDraft * d) {
  DraftCatch * c = (DraftCatch *)ctx;
  if (c->seen) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  c->type = gdefstor_dup(c->alloc, d->type, d->type_len);
  c->json = (uint8_t *)gdefstor_mem(c->alloc, d->json_len + 1);
  if (c->type == NULL || c->json == NULL) {
    draft_catch_release(c);
    return GDEFSTOR_ERR_OOM;
  }
  if (d->json_len > 0) {
    memcpy(c->json, d->json, d->json_len);
  }
  c->type_len = d->type_len;
  c->json_len = d->json_len;
  c->base = d->base_revision;
  c->seen = 1;
  return GDEFSTOR_OK;
}

/* ERR_INVALID when there is none. */
static GDEFSTOR_Result load_draft(GDEFSTOR_Store * store, void * txn,
    const char * tenant, size_t tenant_len, const char * pid, size_t pid_len,
    DraftCatch * out) {
  GDEFSTOR_ModelVisitor v;
  GDEFSTOR_Result rc;
  memset(out, 0, sizeof *out);
  out->alloc = store->alloc;
  if (!gdefstor_model_id_ok(pid, pid_len, store->limits.max_public_id_bytes,
          0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&v, 0, sizeof v);
  v.ctx = out;
  v.draft = catch_draft;
  rc = store->model.draft_get(store->model.ctx, txn, tenant, tenant_len, pid,
      pid_len, &v);
  if (rc != GDEFSTOR_OK) {
    draft_catch_release(out);
    return rc;
  }
  return out->seen ? GDEFSTOR_OK : GDEFSTOR_ERR_INTERNAL;
}

static GDEFSTOR_Result draft_exists(GDEFSTOR_Store * store, void * txn,
    const char * tenant, size_t tenant_len, const char * pid, size_t pid_len,
    int * out) {
  DraftCatch c;
  GDEFSTOR_Result rc = load_draft(store, txn, tenant, tenant_len, pid, pid_len,
      &c);
  *out = 0;
  if (rc == GDEFSTOR_ERR_INVALID) {
    return GDEFSTOR_OK;
  }
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  draft_catch_release(&c);
  *out = 1;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_draft_put(GDEFSTOR_Write * write, const char * type,
    size_t type_len, const char * public_id, size_t public_id_len,
    uint64_t base_revision, const uint8_t * json, size_t json_len) {
  GDEFSTOR_Store * store;
  GDEFSTOR_ModelDraft d;
  GDEFSTOR_Type * t = NULL;
  Ent e;
  void * txn = NULL;
  int exists;
  GDEFSTOR_Result rc = write_store(write, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (json == NULL ||
      !gdefstor_model_id_ok(public_id, public_id_len,
          write->limits.max_public_id_bytes, 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_model_json_ok(store->alloc, json, json_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_txn(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = find_entity(store, txn, write->tenant, write->tenant_len, public_id,
      public_id_len, &e);
  if (rc != GDEFSTOR_OK && rc != GDEFSTOR_ERR_INVALID) {
    return rc;
  }
  exists = rc == GDEFSTOR_OK;
  memset(&d, 0, sizeof d);
  if (exists) {
    if ((type != NULL && !same_span(type, type_len, e.type, e.type_len)) ||
        base_revision > e.revision) {
      ent_release(store->alloc, &e);
      return GDEFSTOR_ERR_INVALID;
    }
  } else {
    /* A draft of an entity never saved reserves its public id. */
    if (type == NULL || base_revision != 0) {
      return GDEFSTOR_ERR_INVALID;
    }
    rc = gdefstor_model_type_load(store, txn, write->tenant,
        write->tenant_len, type, type_len, &t);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    gdefstor_type_free(t);
  }
  d.public_id = public_id;
  d.public_id_len = public_id_len;
  d.type = exists ? e.type : type;
  d.type_len = exists ? e.type_len : type_len;
  d.base_revision = base_revision;
  d.json = json;
  d.json_len = json_len;
  rc = store->model.draft_put(store->model.ctx, txn, &d);
  if (exists) {
    ent_release(store->alloc, &e);
  }
  return rc == GDEFSTOR_OK ? rc : gdefstor_model_break(write, rc);
}

struct GDEFSTOR_Draft {
  DraftCatch c;
};

GDEFSTOR_Result gdefstor_draft_get(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * public_id, size_t public_id_len,
    GDEFSTOR_Draft ** out_draft) {
  GDEFSTOR_Draft * d;
  GDEFSTOR_Result rc = read_args(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (out_draft == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  d = (GDEFSTOR_Draft *)gdefstor_mem0(store->alloc, sizeof *d);
  if (d == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  rc = load_draft(store, NULL, tenant, tenant_len, public_id, public_id_len,
      &d->c);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(store->alloc, d);
    return rc;
  }
  *out_draft = d;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_draft_info(const GDEFSTOR_Draft * draft,
    GDEFSTOR_DraftInfo * out) {
  if (draft == NULL || out == NULL || out->size != sizeof *out) {
    return GDEFSTOR_ERR_INVALID;
  }
  out->type = draft->c.type;
  out->type_len = draft->c.type_len;
  out->base_revision = draft->c.base;
  out->json = draft->c.json;
  out->json_len = draft->c.json_len;
  return GDEFSTOR_OK;
}

void gdefstor_draft_free(GDEFSTOR_Draft * draft) {
  const GDEFSTOR_Allocator * alloc;
  if (draft == NULL) {
    return;
  }
  alloc = draft->c.alloc;
  draft_catch_release(&draft->c);
  gcu_allocator_free(alloc, draft);
}

GDEFSTOR_Result gdefstor_draft_delete(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len) {
  GDEFSTOR_Store * store;
  void * txn = NULL;
  int exists = 0;
  GDEFSTOR_Result rc = write_store(write, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_txn(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = draft_exists(store, txn, write->tenant, write->tenant_len, public_id,
      public_id_len, &exists);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!exists) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = store->model.draft_delete(store->model.ctx, txn, public_id,
      public_id_len);
  return rc == GDEFSTOR_OK ? rc : gdefstor_model_break(write, rc);
}

GDEFSTOR_Result gdefstor_draft_apply(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len,
    const GDEFSTOR_RevisionMeta * meta, uint64_t * out_revision) {
  GDEFSTOR_Store * store;
  DraftCatch c;
  GDEFSTOR_Type * type = NULL;
  GDEFSTOR_Tree * tree = NULL;
  Ent e;
  void * txn = NULL;
  int exists;
  GDEFSTOR_Result rc = write_store(write, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = check_meta(store, meta);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_txn(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = load_draft(store, txn, write->tenant, write->tenant_len, public_id,
      public_id_len, &c);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = find_entity(store, txn, write->tenant, write->tenant_len, public_id,
      public_id_len, &e);
  exists = rc == GDEFSTOR_OK;
  if (rc == GDEFSTOR_ERR_INVALID) {
    memset(&e, 0, sizeof e);
    rc = GDEFSTOR_OK;
  }
  /* The draft was made from its base; the head may not have moved since. */
  if (rc == GDEFSTOR_OK && exists && e.revision != c.base) {
    rc = GDEFSTOR_ERR_CONFLICT;
  }
  if (rc == GDEFSTOR_OK && !exists && c.base != 0) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_type_load(store, txn, write->tenant,
        write->tenant_len, c.type, c.type_len, &type);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_from_json(type, c.json, c.json_len, store->alloc,
        &tree);
    if (rc == GDEFSTOR_ERR_FORMAT || rc == GDEFSTOR_ERR_CORRUPT) {
      rc = GDEFSTOR_ERR_INVALID;
    }
  }
  if (rc == GDEFSTOR_OK && exists) {
    rc = save_tree(write, txn, &e, type, tree, meta, out_revision);
  } else if (rc == GDEFSTOR_OK) {
    /* A reserved id: check everything, then create the entity and write
     * revision 1, so a draft that does not validate creates nothing. */
    Save s;
    size_t root;
    rc = save_begin(&s, store, type, txn, write->tenant, write->tenant_len,
        &e, write->limits.max_public_id_bytes);
    if (rc == GDEFSTOR_OK) {
      rc = tgt_new(&s, 0, GDEFSTOR_NONE, GDEFSTOR_NONE, &root);
    }
    if (rc == GDEFSTOR_OK) {
      rc = resolve_node(&s, tree, 0, root, GDEFSTOR_NONE);
    }
    if (rc == GDEFSTOR_OK) {
      rc = save_plan(&s);
    }
    if (rc == GDEFSTOR_OK) {
      GDEFSTOR_ModelEntity me;
      uint64_t row = 0;
      memset(&me, 0, sizeof me);
      me.type = c.type;
      me.type_len = c.type_len;
      me.public_id = public_id;
      me.public_id_len = public_id_len;
      me.creator = meta->who_len == 0 ? "" : meta->who;
      me.creator_len = meta->who_len;
      me.created_ms = meta->when_ms;
      rc = store->model.entity_insert(store->model.ctx, txn, &me, &row);
      if (rc != GDEFSTOR_OK) {
        rc = gdefstor_model_break(write, rc);
      } else {
        rc = save_apply(&s, write, txn, row, 0, meta, out_revision);
      }
    }
    save_release(&s);
  }
  if (rc == GDEFSTOR_OK) {
    rc = store->model.draft_delete(store->model.ctx, txn, public_id,
        public_id_len);
    if (rc != GDEFSTOR_OK) {
      rc = gdefstor_model_break(write, rc);
    }
  }
  gdefstor_tree_free(tree);
  gdefstor_type_free(type);
  draft_catch_release(&c);
  if (exists) {
    ent_release(store->alloc, &e);
  }
  return rc;
}

/* ---- derived data -------------------------------------------------------- */

GDEFSTOR_Result gdefstor_model_rebuild(GDEFSTOR_Write * write) {
  GDEFSTOR_Store * store;
  void * txn = NULL;
  GDEFSTOR_Result rc = write_store(write, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_txn(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = store->model.rebuild(store->model.ctx, txn);
  return rc == GDEFSTOR_OK ? rc : gdefstor_model_break(write, rc);
}

typedef struct Lines {
  const GDEFSTOR_Allocator * alloc;
  char ** v;
  size_t * lens;
  size_t n;
  size_t cap;
  /* the line being written */
  char * buf;
  size_t len;
  size_t bcap;
  GDEFSTOR_Result rc;
} Lines;

static void l_put(Lines * l, const char * s, size_t n) {
  if (l->rc == GDEFSTOR_OK) {
    l->rc = gdefstor_buf_append(&l->buf, &l->len, &l->bcap, l->alloc, s, n);
  }
}

static void l_str(Lines * l, const char * s) {
  l_put(l, s, strlen(s));
}

static void l_hex(Lines * l, const void * p, size_t n) {
  static const char hex[] = "0123456789abcdef";
  const unsigned char * b = (const unsigned char *)p;
  size_t i;
  l_put(l, " ", 1);
  if (n == 0) {
    l_put(l, "-", 1);
  }
  for (i = 0; i < n; i++) {
    char pair[2];
    pair[0] = hex[b[i] >> 4];
    pair[1] = hex[b[i] & 0xF];
    l_put(l, pair, 2);
  }
}

static void l_u64(Lines * l, uint64_t v) {
  char tmp[32];
  snprintf(tmp, sizeof tmp, " %llu", (unsigned long long)v);
  l_str(l, tmp);
}

static void l_i64(Lines * l, int64_t v) {
  char tmp[32];
  snprintf(tmp, sizeof tmp, " %lld", (long long)v);
  l_str(l, tmp);
}

/* A double's bits: exact, and no locale in it. */
static void l_bits(Lines * l, double d) {
  uint64_t bits;
  char tmp[32];
  memcpy(&bits, &d, sizeof bits);
  snprintf(tmp, sizeof tmp, " %016llx", (unsigned long long)bits);
  l_str(l, tmp);
}

static void l_value(Lines * l, const GDEFSTOR_Value * v) {
  char tmp[32];
  snprintf(tmp, sizeof tmp, " v%d", v->type);
  l_str(l, tmp);
  if (v->type == 0) {
    return;
  }
  l_i64(l, v->integer);
  l_bits(l, v->real);
  l_hex(l, v->bytes, v->len);
  l_hex(l, v->subject, v->subject_len);
}

static void l_end(Lines * l) {
  if (l->rc != GDEFSTOR_OK) {
    return;
  }
  if (l->n == l->cap) {
    size_t grown = l->cap == 0 ? 64 : l->cap * 2;
    char ** v = (char **)gcu_allocator_realloc(l->alloc, l->v,
        grown * sizeof *v);
    size_t * lens;
    if (v == NULL) {
      l->rc = GDEFSTOR_ERR_OOM;
      return;
    }
    l->v = v;
    lens = (size_t *)gcu_allocator_realloc(l->alloc, l->lens,
        grown * sizeof *lens);
    if (lens == NULL) {
      l->rc = GDEFSTOR_ERR_OOM;
      return;
    }
    l->lens = lens;
    l->cap = grown;
  }
  l->v[l->n] = l->buf == NULL ? gdefstor_dup(l->alloc, "", 0) : l->buf;
  l->lens[l->n] = l->len;
  if (l->v[l->n] == NULL) {
    l->rc = GDEFSTOR_ERR_OOM;
    return;
  }
  l->n += 1;
  l->buf = NULL;
  l->len = 0;
  l->bcap = 0;
}

static GDEFSTOR_Result dump_type(void * ctx, const char * name,
    size_t name_len, const uint8_t * text, size_t text_len) {
  Lines * l = (Lines *)ctx;
  l_str(l, "T");
  l_hex(l, name, name_len);
  l_hex(l, text, text_len);
  l_end(l);
  return l->rc;
}

static GDEFSTOR_Result dump_entity(void * ctx, const GDEFSTOR_ModelEntity * e) {
  Lines * l = (Lines *)ctx;
  l_str(l, "E");
  l_u64(l, e->row);
  l_hex(l, e->type, e->type_len);
  l_hex(l, e->public_id, e->public_id_len);
  l_hex(l, e->creator, e->creator_len);
  l_i64(l, e->created_ms);
  l_u64(l, e->revision);
  l_u64(l, e->last_item);
  l_u64(l, (uint64_t)(e->published != 0));
  l_u64(l, (uint64_t)(e->deleted != 0));
  l_end(l);
  return l->rc;
}

static GDEFSTOR_Result dump_state(void * ctx, uint64_t row,
    const GDEFSTOR_ModelState * s) {
  Lines * l = (Lines *)ctx;
  l_str(l, "S");
  l_u64(l, row);
  l_u64(l, s->seq);
  l_u64(l, (uint64_t)(s->published != 0));
  l_u64(l, (uint64_t)(s->deleted != 0));
  l_hex(l, s->actor, s->actor_len);
  l_i64(l, s->when_ms);
  l_end(l);
  return l->rc;
}

static GDEFSTOR_Result dump_revision(void * ctx, uint64_t row,
    const GDEFSTOR_ModelRevision * r) {
  Lines * l = (Lines *)ctx;
  l_str(l, "R");
  l_u64(l, row);
  l_u64(l, r->revision);
  l_u64(l, r->commit_seq);
  l_hex(l, r->who, r->who_len);
  l_i64(l, r->when_ms);
  l_hex(l, r->why, r->why_len);
  l_end(l);
  return l->rc;
}

static GDEFSTOR_Result dump_item(void * ctx, uint64_t row,
    const GDEFSTOR_ModelItem * i) {
  Lines * l = (Lines *)ctx;
  size_t k;
  l_str(l, "I");
  l_u64(l, row);
  l_u64(l, i->group);
  l_u64(l, i->item);
  l_u64(l, i->valid_from);
  l_u64(l, i->valid_to);
  l_u64(l, i->parent);
  l_bits(l, i->order);
  /* Trailing no-values are not stored differences: a row written before a
   * column existed and one written after with that column empty read alike. */
  k = i->value_count;
  while (k > 0 && i->values[k - 1].type == 0) {
    k -= 1;
  }
  l_u64(l, k);
  {
    size_t j;
    for (j = 0; j < k; j++) {
      l_value(l, &i->values[j]);
    }
  }
  l_end(l);
  return l->rc;
}

static GDEFSTOR_Result dump_draft(void * ctx, const GDEFSTOR_ModelDraft * d) {
  Lines * l = (Lines *)ctx;
  l_str(l, "D");
  l_hex(l, d->public_id, d->public_id_len);
  l_hex(l, d->type, d->type_len);
  l_u64(l, d->base_revision);
  l_hex(l, d->json, d->json_len);
  l_end(l);
  return l->rc;
}

static int line_cmp(const void * ctx, size_t a, size_t b) {
  const Lines * l = (const Lines *)ctx;
  size_t n = l->lens[a] < l->lens[b] ? l->lens[a] : l->lens[b];
  int c = memcmp(l->v[a], l->v[b], n);
  if (c != 0) {
    return c;
  }
  return l->lens[a] < l->lens[b] ? -1 : (l->lens[a] > l->lens[b] ? 1 : 0);
}

GDEFSTOR_Result gdefstor_model_dump(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, int which, uint8_t ** out_text,
    size_t * out_len) {
  Lines l;
  GDEFSTOR_ModelVisitor v;
  size_t * idx = NULL;
  char * text = NULL;
  size_t len = 0;
  size_t cap = 0;
  size_t i;
  GDEFSTOR_Result rc = read_args(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (out_text == NULL || out_len == NULL ||
      (which != GDEFSTOR_MODEL_TRUTH && which != GDEFSTOR_MODEL_CURRENT)) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&l, 0, sizeof l);
  l.alloc = store->alloc;
  memset(&v, 0, sizeof v);
  v.ctx = &l;
  v.type = dump_type;
  v.entity = dump_entity;
  v.state = dump_state;
  v.revision = dump_revision;
  v.item = dump_item;
  v.draft = dump_draft;
  rc = store->model.dump(store->model.ctx, tenant, tenant_len, which, &v);
  if (rc == GDEFSTOR_OK) {
    rc = l.rc;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(l.alloc, l.n, line_cmp, &l, &idx);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < l.n; i++) {
    rc = gdefstor_buf_append(&text, &len, &cap, l.alloc, l.v[idx[i]],
        l.lens[idx[i]]);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&text, &len, &cap, l.alloc, "\n", 1);
    }
  }
  for (i = 0; i < l.n; i++) {
    gcu_allocator_free(l.alloc, l.v[i]);
  }
  gcu_allocator_free(l.alloc, l.v);
  gcu_allocator_free(l.alloc, l.lens);
  gcu_allocator_free(l.alloc, l.buf);
  gcu_allocator_free(l.alloc, idx);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(l.alloc, text);
    return rc;
  }
  if (text == NULL) {
    text = gdefstor_dup(l.alloc, "", 0);
    if (text == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
  }
  *out_text = (uint8_t *)text;
  *out_len = len;
  return GDEFSTOR_OK;
}
