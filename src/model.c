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
#include <stdlib.h>
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
  GDEFSTOR_ModelBackend copy;
  GDEFSTOR_Result rc;
  if (model == NULL || out_store == NULL ||
      (model->size != sizeof(GDEFSTOR_ModelBackend) &&
          model->size != GDEFSTOR_MODEL_BACKEND_SIZE_V1 &&
          model->size != GDEFSTOR_MODEL_BACKEND_SIZE_V2 &&
          model->size != GDEFSTOR_MODEL_BACKEND_SIZE_V3 &&
          model->size != GDEFSTOR_MODEL_BACKEND_SIZE_V4) ||
      model->destroy == NULL ||
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
  /* A backend of an earlier version's size has fewer optional members: they
   * read as NULL, never as bytes past what it filled. */
  memset(&copy, 0, sizeof copy);
  memcpy(&copy, model, model->size);
  copy.size = sizeof copy;
  /* The blind index is the three members together or not at all: a backend
   * that could write blind rows and not delete them would make erasure lie. */
  {
    int n = (copy.blind_history != NULL) + (copy.subject_lookup != NULL) +
            (copy.blind_delete != NULL);
    if (n != 0 && n != 3) {
      return GDEFSTOR_ERR_INVALID;
    }
  }
  /* Records, counters and the outbox together or not at all: a backend that
   * kept rows it could not list, or claimed rows it could not delete, would
   * answer half a contract. */
  {
    int n = (copy.record_put != NULL) + (copy.record_delete != NULL) +
            (copy.record_get != NULL) + (copy.record_list != NULL) +
            (copy.counter_next != NULL) + (copy.counter_get != NULL) +
            (copy.outbox_insert != NULL) + (copy.outbox_claim != NULL) +
            (copy.outbox_delete != NULL) + (copy.outbox_fail != NULL) +
            (copy.outbox_parked != NULL);
    if (n != 0 && n != 11) {
      return GDEFSTOR_ERR_INVALID;
    }
  }
  rc = gdefstor_store_create(backend, allocator, limits, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  store->model = copy;
  store->has_model = 1;
  *out_store = store;
  return GDEFSTOR_OK;
}

int gdefstor_model_has_blind(const GDEFSTOR_Store * store) {
  return store != NULL && store->has_model &&
         store->model.blind_history != NULL;
}

int gdefstor_model_has_sys(const GDEFSTOR_Store * store) {
  return store != NULL && store->has_model && store->model.record_put != NULL;
}

int gdefstor_model_has_personal(const GDEFSTOR_Type * t) {
  size_t i;
  for (i = 0; t != NULL && i < t->n; i++) {
    if ((t->f[i].flags & GDEFSTOR_FIELD_PERSONAL) != 0) {
      return 1;
    }
  }
  return 0;
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
  char lang[GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES];
  size_t lang_len;
} Item;

/* ---- languages ----------------------------------------------------------- */

#define lang_cmp gdefstor_model_lang_cmp

/* A set of languages, sorted by bytes. */
typedef struct Lang {
  char tag[GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES];
  size_t len;
} Lang;

typedef struct Langs {
  const GDEFSTOR_Allocator * alloc;
  Lang * v;
  size_t n;
  size_t cap;
} Langs;

static void langs_release(Langs * l) {
  gcu_allocator_free(l->alloc, l->v);
  l->v = NULL;
  l->n = 0;
  l->cap = 0;
}

/* Where @p tag is or would go. */
static size_t langs_at(const Langs * l, const char * tag, size_t len,
    int * found) {
  size_t lo = 0;
  size_t hi = l->n;
  *found = 0;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = lang_cmp(l->v[mid].tag, l->v[mid].len, tag, len);
    if (c == 0) {
      *found = 1;
      return mid;
    }
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

static int langs_has(const Langs * l, const char * tag, size_t len) {
  int found;
  (void)langs_at(l, tag, len, &found);
  return found;
}

/* Add a valid tag; *out_added 0 when it was there already. */
static GDEFSTOR_Result langs_add(Langs * l, const char * tag, size_t len,
    int * out_added) {
  int found;
  size_t at = langs_at(l, tag, len, &found);
  if (out_added != NULL) {
    *out_added = !found;
  }
  if (found) {
    return GDEFSTOR_OK;
  }
  if (l->n == l->cap) {
    size_t grown = l->cap == 0 ? 4 : l->cap * 2;
    Lang * next = (Lang *)gcu_allocator_realloc(l->alloc, l->v,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    l->v = next;
    l->cap = grown;
  }
  memmove(&l->v[at + 1], &l->v[at], (l->n - at) * sizeof *l->v);
  memcpy(l->v[at].tag, tag, len);
  l->v[at].len = len;
  l->n += 1;
  return GDEFSTOR_OK;
}


/* A blind row the port read back: which version and field it indexes, and
 * its MAC (the subject is the value's own, so it is not kept). */
typedef struct IBlind {
  uint64_t item;
  uint64_t from;
  uint32_t field;
  GDEFSTOR_BlindVal b;
} IBlind;

typedef struct Items {
  const GDEFSTOR_Allocator * alloc;
  Item * v;
  size_t n;
  size_t cap;
  IBlind * bl;  /* read only when asked: sorted by item, from, field */
  size_t nbl;
  size_t capbl;
  /* The language asked for, or none (every language): a row of another is
   * the backend's damage. */
  const char * lang;
  size_t lang_len;
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
  gcu_allocator_free(it->alloc, it->bl);
  it->v = NULL;
  it->n = 0;
  it->cap = 0;
  it->bl = NULL;
  it->nbl = 0;
  it->capbl = 0;
}

static GDEFSTOR_Result catch_blind(void * ctx, uint64_t row,
    const GDEFSTOR_ModelBlind * b) {
  Items * it = (Items *)ctx;
  IBlind * o;
  (void)row;
  if (b->mac == NULL || it->nbl >= GDEFSTOR_MODEL_MAX_ITEMS * 4u ||
      !gdefstor_model_lang_ok(b->language, b->language_len) ||
      (it->lang_len != 0 &&
          !same_span(b->language, b->language_len, it->lang, it->lang_len))) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (it->nbl == it->capbl) {
    size_t grown = it->capbl == 0 ? 8 : it->capbl * 2;
    IBlind * next = (IBlind *)gcu_allocator_realloc(it->alloc, it->bl,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    it->bl = next;
    it->capbl = grown;
  }
  o = &it->bl[it->nbl];
  memset(o, 0, sizeof *o);
  o->item = b->item;
  o->from = b->valid_from;
  o->field = b->field;
  o->b.set = 1;
  memcpy(o->b.mac, b->mac, GDEFSTOR_BLIND_MAC_BYTES);
  o->b.key_id = b->key_id;
  o->b.version = b->version;
  it->nbl += 1;
  return GDEFSTOR_OK;
}

static int blind_cmp(const IBlind * a, uint64_t item, uint64_t from,
    uint32_t field) {
  if (a->item != item) {
    return a->item < item ? -1 : 1;
  }
  if (a->from != from) {
    return a->from < from ? -1 : 1;
  }
  return a->field < field ? -1 : (a->field > field ? 1 : 0);
}

static int blind_qcmp(const void * a, const void * b) {
  const IBlind * y = (const IBlind *)b;
  return blind_cmp((const IBlind *)a, y->item, y->from, y->field);
}

/* Insertion order is the backend's; lookups want it sorted. No two rows
 * share a key, so an unstable sort is enough. */
static void blinds_sort(Items * it) {
  if (it->nbl > 1) {
    qsort(it->bl, it->nbl, sizeof *it->bl, blind_qcmp);
  }
}

/* The blind row of version (item, from) for field id @p field, or NULL. */
static const GDEFSTOR_BlindVal * blind_of(const Items * it, uint64_t item,
    uint64_t from, uint32_t field) {
  size_t lo = 0;
  size_t hi = it->nbl;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = blind_cmp(&it->bl[mid], item, from, field);
    if (c == 0) {
      return &it->bl[mid].b;
    }
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return NULL;
}

static GDEFSTOR_Result catch_item(void * ctx, uint64_t row,
    const GDEFSTOR_ModelItem * i) {
  Items * it = (Items *)ctx;
  Item * o;
  size_t k;
  (void)row;
  if (it->n >= GDEFSTOR_MODEL_MAX_ITEMS * 4u * GDEFSTOR_MODEL_MAX_LANGUAGES ||
      !gdefstor_model_lang_ok(i->language, i->language_len) ||
      (it->lang_len != 0 &&
          !same_span(i->language, i->language_len, it->lang, it->lang_len))) {
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
  memcpy(o->lang, i->language, i->language_len);
  o->lang_len = i->language_len;
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

/* The items of language @p lang (every language when @p lang_len is 0).
 * @p with_blind also reads the entity's blind rows (a backend that serves the
 * blind index only). */
static GDEFSTOR_Result load_items(GDEFSTOR_Store * store, void * txn,
    const char * tenant, size_t tenant_len, uint64_t row, uint64_t as_of,
    const char * lang, size_t lang_len, int with_blind, Items * out) {
  GDEFSTOR_ModelVisitor v;
  GDEFSTOR_Result rc;
  memset(out, 0, sizeof *out);
  out->alloc = store->alloc;
  out->lang = lang;
  out->lang_len = lang_len;
  memset(&v, 0, sizeof v);
  v.ctx = out;
  v.item = catch_item;
  if (with_blind && gdefstor_model_has_blind(store)) {
    v.blind = catch_blind;
  }
  rc = store->model.items(store->model.ctx, txn, tenant, tenant_len, row,
      as_of, lang_len == 0 ? NULL : lang, lang_len, &v);
  if (rc != GDEFSTOR_OK) {
    items_release(out);
    return rc;
  }
  blinds_sort(out);
  return GDEFSTOR_OK;
}

static int cmp_u64(uint64_t a, uint64_t b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

static GDEFSTOR_Result catch_lang(void * ctx, uint64_t row,
    const char * language, size_t language_len) {
  Langs * l = (Langs *)ctx;
  int added = 0;
  GDEFSTOR_Result rc;
  (void)row;
  if (!gdefstor_model_lang_ok(language, language_len) ||
      l->n >= GDEFSTOR_MODEL_MAX_LANGUAGES) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = langs_add(l, language, language_len, &added);
  if (rc == GDEFSTOR_OK && !added) {
    /* One root, so one visit, per language. */
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  return rc;
}

/* The languages entity @p row has a tree in (a root item), now or as of a
 * revision. */
static GDEFSTOR_Result load_langs(GDEFSTOR_Store * store, void * txn,
    const char * tenant, size_t tenant_len, uint64_t row, uint64_t as_of,
    Langs * out) {
  GDEFSTOR_Result rc;
  memset(out, 0, sizeof *out);
  out->alloc = store->alloc;
  if (store->model.languages != NULL) {
    GDEFSTOR_ModelVisitor v;
    memset(&v, 0, sizeof v);
    v.ctx = out;
    v.language = catch_lang;
    rc = store->model.languages(store->model.lang_ctx, txn, tenant,
        tenant_len, row, as_of, &v);
  } else {
    /* No call of its own: the root items, from every language's items. */
    Items it;
    size_t i;
    rc = load_items(store, txn, tenant, tenant_len, row, as_of, NULL, 0, 0,
        &it);
    for (i = 0; rc == GDEFSTOR_OK && i < it.n; i++) {
      if (it.v[i].group == 0) {
        rc = catch_lang(out, row, it.v[i].lang, it.v[i].lang_len);
      }
    }
    items_release(&it);
  }
  if (rc != GDEFSTOR_OK) {
    langs_release(out);
  }
  return rc;
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
  GDEFSTOR_BlindVal * bl; /* by column, when the save keeps blind rows */
  uint32_t ncols;
  size_t base;            /* the current version it continues, or NONE */
} Tgt;

typedef struct Save {
  GDEFSTOR_Store * store;
  const GDEFSTOR_Allocator * alloc;
  const GDEFSTOR_Type * type;
  /* The language whose tree this plans: every base item, close, insert and
   * blind row is of it. */
  char lang[GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES];
  size_t lang_len;
  size_t id_cap;
  int revert;             /* ids from history may come back */
  /* The type has a personal field and the backend serves the blind index:
   * the save reads the current versions' blind rows and writes new ones. */
  int personal;
  Tgt * t;
  size_t n;
  size_t cap;
  Items base;             /* the current versions */
  Items past;             /* a revert's past versions: targets borrow them */
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
  GDEFSTOR_ModelBlind * blinds;
  size_t nblind;
} Save;

static void save_release(Save * s) {
  size_t i;
  for (i = 0; i < s->n; i++) {
    gcu_allocator_free(s->alloc, s->t[i].cols);
    gcu_allocator_free(s->alloc, s->t[i].bl);
  }
  gcu_allocator_free(s->alloc, s->t);
  gcu_allocator_free(s->alloc, s->blinds);
  gcu_allocator_free(s->alloc, s->base_idx);
  gcu_allocator_free(s->alloc, s->base_by_parent);
  gcu_allocator_free(s->alloc, s->used);
  gcu_allocator_free(s->alloc, s->closes);
  gcu_allocator_free(s->alloc, s->inserts);
  items_release(&s->base);
  items_release(&s->past);
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
    if (s->personal) {
      x->bl = (GDEFSTOR_BlindVal *)gdefstor_mem0(s->alloc,
          x->ncols * sizeof *x->bl);
      if (x->bl == NULL) {
        gcu_allocator_free(s->alloc, x->cols);
        x->cols = NULL;
        return GDEFSTOR_ERR_OOM;
      }
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
    if (tv->blind.set && (f->flags & GDEFSTOR_FIELD_PERSONAL) == 0) {
      /* A MAC indexes a personal value; on any other it would be a guess
       * kept beside a plaintext. */
      return GDEFSTOR_ERR_INVALID;
    }
    if (tv->blind.set && !gdefstor_model_has_blind(s->store)) {
      return GDEFSTOR_ERR_UNSUPPORTED;
    }
    if (f->eff_retired) {
      /* Restating what is stored is allowed; changing it is not. A retired
       * value writes no new version of its own, so a MAC on it is unused. */
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
    if (tv->blind.set && s->t[ti].bl != NULL) {
      s->t[ti].bl[col] = tv->blind;
    }
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
    if (i == 0 && !s->revert) {
      /* The root continues the language's root; a revert names the old one,
       * which a language removed and saved again since no longer has. */
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
          if (x->bl != NULL) {
            /* Whatever MAC came with the tree or a revert was of another
             * value; carry_blind() gives this one its own. */
            memset(&x->bl[c], 0, sizeof x->bl[c]);
          }
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

/* The field of column @p c of group @p g. */
static const GDEFSTOR_MField * col_field(const GDEFSTOR_Type * t, uint32_t g,
    uint32_t c) {
  return &t->f[t->col_field[t->col_base[g] + c]];
}

/* A personal value an item carries into its next version unchanged keeps its
 * blind row: the current version's MAC goes with it, unless the tree gave
 * one. The value is the same ciphertext and subject, so the same plaintext,
 * and its MAC is the same MAC. */
static void carry_blind(Save * s) {
  size_t i;
  for (i = 0; i < s->n; i++) {
    Tgt * x = &s->t[i];
    const Item * b;
    uint32_t c;
    if (x->bl == NULL || x->base == GDEFSTOR_NONE) {
      continue;
    }
    b = &s->base.v[x->base];
    for (c = 0; c < x->ncols; c++) {
      const GDEFSTOR_MField * f = col_field(s->type, x->group, c);
      const GDEFSTOR_BlindVal * was;
      if (x->bl[c].set || x->cols[c].type == 0 ||
          (f->flags & GDEFSTOR_FIELD_PERSONAL) == 0 ||
          !gdefstor_model_value_equal(&x->cols[c], base_col(b, c))) {
        continue;
      }
      was = blind_of(&s->base, b->item, b->from, f->id);
      if (was != NULL) {
        x->bl[c] = *was;
      }
    }
  }
}

/* The blind rows of one new version: one per personal value with a MAC. */
static void blinds_of(Save * s, const Tgt * x, int count_only, size_t * n) {
  uint32_t c;
  for (c = 0; x->bl != NULL && c < x->ncols; c++) {
    const GDEFSTOR_MField * f = col_field(s->type, x->group, c);
    GDEFSTOR_ModelBlind * o;
    if (!x->bl[c].set || x->cols[c].type == 0 ||
        (f->flags & GDEFSTOR_FIELD_PERSONAL) == 0) {
      continue;
    }
    if (count_only) {
      *n += 1;
      continue;
    }
    o = &s->blinds[s->nblind];
    memset(o, 0, sizeof *o);
    o->item = x->id;
    o->field = f->id;
    o->subject = x->cols[c].subject;
    o->subject_len = x->cols[c].subject_len;
    o->mac = x->bl[c].mac;
    o->key_id = x->bl[c].key_id;
    o->version = x->bl[c].version;
    o->language = s->lang;
    o->language_len = s->lang_len;
    s->nblind += 1;
  }
}

/* The closes and inserts that take the current versions to the targets, and
 * the blind rows of the inserts. */
static GDEFSTOR_Result diff_rows(Save * s) {
  size_t i;
  size_t nbl = 0;
  if (s->personal) {
    carry_blind(s);
    for (i = 0; i < s->n; i++) {
      blinds_of(s, &s->t[i], 1, &nbl);
    }
  }
  s->closes = (GDEFSTOR_ModelClose *)gdefstor_mem0(s->alloc,
      (s->base.n + 1) * sizeof *s->closes);
  s->inserts = (GDEFSTOR_ModelItem *)gdefstor_mem0(s->alloc,
      (s->n + 1) * sizeof *s->inserts);
  s->blinds = (GDEFSTOR_ModelBlind *)gdefstor_mem0(s->alloc,
      (nbl + 1) * sizeof *s->blinds);
  if (s->closes == NULL || s->inserts == NULL || s->blinds == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < s->n; i++) {
    const Tgt * x = &s->t[i];
    GDEFSTOR_ModelItem * ins;
    if (x->base != GDEFSTOR_NONE) {
      const Item * b = &s->base.v[x->base];
      if (item_same(s, x, b)) {
        /* Unchanged: no new version, so no blind row either. */
        continue;
      }
      s->closes[s->nclose].group = b->group;
      s->closes[s->nclose].item = b->item;
      s->closes[s->nclose].valid_from = b->from;
      s->closes[s->nclose].language = s->lang;
      s->closes[s->nclose].language_len = s->lang_len;
      s->nclose += 1;
    }
    ins = &s->inserts[s->ninsert];
    ins->group = x->group;
    ins->item = x->id;
    ins->parent = parent_id(s, x);
    ins->order = x->order;
    ins->values = x->cols;
    ins->value_count = x->ncols;
    ins->language = s->lang;
    ins->language_len = s->lang_len;
    s->ninsert += 1;
    blinds_of(s, x, 0, NULL);
  }
  for (i = 0; i < s->base.n; i++) {
    if (!s->used[i]) {
      s->closes[s->nclose].group = s->base.v[i].group;
      s->closes[s->nclose].item = s->base.v[i].item;
      s->closes[s->nclose].valid_from = s->base.v[i].from;
      s->closes[s->nclose].language = s->lang;
      s->closes[s->nclose].language_len = s->lang_len;
      s->nclose += 1;
    }
  }
  return GDEFSTOR_OK;
}

/* Load the current versions of one language of an entity (none for a new
 * entity or language) and index them. */
static GDEFSTOR_Result save_begin(Save * s, GDEFSTOR_Store * store,
    const GDEFSTOR_Type * type, void * txn, const char * tenant,
    size_t tenant_len, const Ent * e, size_t id_cap, const char * lang,
    size_t lang_len) {
  size_t i;
  GDEFSTOR_Result rc;
  memset(s, 0, sizeof *s);
  s->store = store;
  s->alloc = store->alloc;
  s->type = type;
  memcpy(s->lang, lang, lang_len);
  s->lang_len = lang_len;
  s->id_cap = id_cap;
  s->personal = gdefstor_model_has_personal(type) &&
                gdefstor_model_has_blind(store);
  s->base.alloc = store->alloc;
  s->next_item = e->last_item;
  if (e->row != 0 && e->revision != 0) {
    rc = load_items(store, txn, tenant, tenant_len, e->row, 0, s->lang,
        s->lang_len, s->personal, &s->base);
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
  /* A language with items has its root; one with none has no tree yet. */
  if (s->base.n != 0 && s->base_root == 0) {
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

/* ---- one revision over several languages ---------------------------------- */

/* The languages a save, a revert or an apply plans, one Save each, and what
 * they share: the entity's item ids are given out across its languages, so
 * one language's new items never take another's ids, and every language's
 * rows are one revision. */
typedef struct Multi {
  GDEFSTOR_Store * store;
  const GDEFSTOR_Type * type;
  const Ent * e;
  void * txn;
  const char * tenant;
  size_t tenant_len;
  size_t id_cap;
  int revert;
  /* Allocated once, at its full size: the rows a part plans point into its
   * Save (the language), so a part never moves. */
  Save * v;
  size_t n;
  size_t cap;
  uint64_t next_item;  /* the last item id given out, across languages */
  Langs now;           /* the languages the entity has a tree in now */
} Multi;

static void multi_release(Multi * m) {
  size_t i;
  for (i = 0; i < m->n; i++) {
    save_release(&m->v[i]);
  }
  gcu_allocator_free(m->store->alloc, m->v);
  langs_release(&m->now);
  m->v = NULL;
  m->n = 0;
}

/* The entity's languages now; parts are reserved by multi_reserve(). */
static GDEFSTOR_Result multi_begin(Multi * m, GDEFSTOR_Write * write,
    void * txn, const Ent * e, const GDEFSTOR_Type * type) {
  memset(m, 0, sizeof *m);
  m->store = write->store;
  m->type = type;
  m->e = e;
  m->txn = txn;
  m->tenant = write->tenant;
  m->tenant_len = write->tenant_len;
  m->id_cap = write->limits.max_public_id_bytes;
  m->next_item = e->last_item;
  m->now.alloc = write->store->alloc;
  if (e->row != 0 && e->revision != 0) {
    return load_langs(m->store, txn, m->tenant, m->tenant_len, e->row, 0,
        &m->now);
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result multi_reserve(Multi * m, size_t parts) {
  m->v = (Save *)gdefstor_mem0(m->store->alloc,
      (parts == 0 ? 1 : parts) * sizeof *m->v);
  if (m->v == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  m->cap = parts;
  return GDEFSTOR_OK;
}

/* A new part for language @p lang, its current versions loaded. */
static GDEFSTOR_Result multi_part(Multi * m, const char * lang, size_t lang_len,
    Save ** out) {
  Save * s;
  size_t i;
  GDEFSTOR_Result rc;
  if (!gdefstor_model_lang_ok(lang, lang_len)) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; i < m->n; i++) {
    if (same_span(m->v[i].lang, m->v[i].lang_len, lang, lang_len)) {
      /* A language named twice in one save. */
      return GDEFSTOR_ERR_INVALID;
    }
  }
  if (m->n >= m->cap) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  s = &m->v[m->n];
  rc = save_begin(s, m->store, m->type, m->txn, m->tenant, m->tenant_len,
      m->e, m->id_cap, lang, lang_len);
  /* Counted at once, so that whatever it holds is released. */
  m->n += 1;
  s->base.lang = NULL;
  s->revert = m->revert;
  s->next_item = m->next_item;
  if (rc == GDEFSTOR_OK &&
      (s->base_root != 0) != langs_has(&m->now, lang, lang_len)) {
    /* The language list and the items disagree: damage. */
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  *out = s;
  return rc;
}

/* Plan language @p lang's whole new tree. */
static GDEFSTOR_Result multi_tree(Multi * m, const char * lang,
    size_t lang_len, const GDEFSTOR_Tree * tree) {
  Save * s = NULL;
  size_t root;
  GDEFSTOR_Result rc = multi_part(m, lang, lang_len, &s);
  if (rc == GDEFSTOR_OK) {
    rc = tgt_new(s, 0, GDEFSTOR_NONE, GDEFSTOR_NONE, &root);
  }
  if (rc == GDEFSTOR_OK) {
    rc = resolve_node(s, tree, 0, root, GDEFSTOR_NONE);
  }
  if (rc == GDEFSTOR_OK) {
    rc = save_plan(s);
  }
  if (rc == GDEFSTOR_OK) {
    m->next_item = s->next_item;
  }
  return rc;
}

/* Plan language @p lang's removal: no targets, so every current item of it
 * is closed (none, for a language it has no tree in). */
static GDEFSTOR_Result multi_remove(Multi * m, const char * lang,
    size_t lang_len) {
  Save * s = NULL;
  GDEFSTOR_Result rc = multi_part(m, lang, lang_len, &s);
  if (rc == GDEFSTOR_OK) {
    rc = save_plan(s);
  }
  return rc;
}

/* Plan language @p lang back to its tree as of @p to_revision, which it has:
 * the past tree, parents first, as targets with their old ids and keys.
 * Retired fields are left out here: carry_retired() keeps the current ones. */
static GDEFSTOR_Result multi_revert(Multi * m, const char * lang,
    size_t lang_len, uint64_t to_revision) {
  const GDEFSTOR_Type * type = m->type;
  Save * s = NULL;
  Items * old;
  size_t * by_parent = NULL;
  size_t * stack_items = NULL;
  size_t * stack_tgts = NULL;
  GDEFSTOR_Result rc;
  rc = multi_part(m, lang, lang_len, &s);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  /* Kept in the part: its targets borrow these values until the apply. */
  old = &s->past;
  rc = load_items(m->store, m->txn, m->tenant, m->tenant_len, m->e->row,
      to_revision, lang, lang_len, s->personal, old);
  s->past.lang = NULL;
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(s->alloc, old->n, by_parent_id, old->v,
        &by_parent);
  }
  if (rc == GDEFSTOR_OK) {
    stack_items = (size_t *)gdefstor_mem(s->alloc,
        (old->n + 1) * sizeof(size_t));
    stack_tgts = (size_t *)gdefstor_mem(s->alloc,
        (old->n + 1) * sizeof(size_t));
    if (stack_items == NULL || stack_tgts == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  if (rc == GDEFSTOR_OK) {
    size_t top = 0;
    size_t i;
    size_t root = GDEFSTOR_NONE;
    for (i = 0; i < old->n; i++) {
      if (old->v[i].group == 0) {
        root = root == GDEFSTOR_NONE ? i : old->n;
      }
    }
    if (root == GDEFSTOR_NONE || root == old->n) {
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
      x = &old->v[oi];
      if (x->group >= type->next_group) {
        rc = GDEFSTOR_ERR_CORRUPT;
        break;
      }
      field = type->group_owner[x->group];
      if (field != GDEFSTOR_NONE && type->f[field].eff_retired) {
        continue;
      }
      rc = tgt_new(s, x->item, field, parent_t, &ti);
      if (rc != GDEFSTOR_OK) {
        break;
      }
      s->t[ti].has_order = 1;
      s->t[ti].order = x->order;
      for (c = 0; c < s->t[ti].ncols; c++) {
        s->t[ti].cols[c] = *base_col(x, c);
      }
      /* The old values come back with the blind rows they had (those of a
       * subject erased since are gone, and are not brought back). */
      for (c = 0; s->t[ti].bl != NULL && c < s->t[ti].ncols; c++) {
        const GDEFSTOR_MField * f = col_field(type, x->group, c);
        const GDEFSTOR_BlindVal * was;
        if ((f->flags & GDEFSTOR_FIELD_PERSONAL) == 0 ||
            s->t[ti].cols[c].type == 0) {
          continue;
        }
        was = blind_of(old, x->item, x->from, f->id);
        if (was != NULL) {
          s->t[ti].bl[c] = *was;
        }
      }
      if (find_id(s->base.v, s->base_idx, s->base.n, x->item) ==
          GDEFSTOR_NONE) {
        /* Brought back: its retired columns are not restored either. */
        for (c = 0; c < s->t[ti].ncols; c++) {
          size_t cf = type->col_field[type->col_base[x->group] + c];
          if (type->f[cf].eff_retired) {
            memset(&s->t[ti].cols[c], 0, sizeof s->t[ti].cols[c]);
          }
        }
      }
      lo = 0;
      hi = old->n;
      while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (old->v[by_parent[mid]].parent < x->item) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      for (; lo < old->n && old->v[by_parent[lo]].parent == x->item; lo++) {
        if (top >= old->n) {
          rc = GDEFSTOR_ERR_CORRUPT;
          break;
        }
        stack_items[top] = by_parent[lo];
        stack_tgts[top] = ti;
        top += 1;
      }
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = save_plan(s);
  }
  if (rc == GDEFSTOR_OK) {
    m->next_item = s->next_item;
  }
  gcu_allocator_free(m->store->alloc, by_parent);
  gcu_allocator_free(m->store->alloc, stack_items);
  gcu_allocator_free(m->store->alloc, stack_tgts);
  return rc;
}

/* Whether a planned part writes anything: an unchanged language writes no
 * row, and is not one of the revision's languages. */
static int part_writes(const Save * s) {
  return s->nclose != 0 || s->ninsert != 0;
}

/* Write every part's planned rows as revision @p head + 1, one apply. No part
 * changed: nothing is written and *out_revision is 0. */
static GDEFSTOR_Result multi_apply(Multi * m, GDEFSTOR_Write * write,
    uint64_t row, uint64_t head, const GDEFSTOR_RevisionMeta * meta,
    uint64_t * out_revision) {
  const GDEFSTOR_Allocator * alloc = m->store->alloc;
  GDEFSTOR_ModelRevision r;
  GDEFSTOR_ModelApply a;
  GDEFSTOR_ModelClose * closes = NULL;
  GDEFSTOR_ModelItem * inserts = NULL;
  GDEFSTOR_ModelBlind * blinds = NULL;
  Langs touched;
  char * text = NULL;
  size_t text_len = 0;
  size_t text_cap = 0;
  size_t nclose = 0;
  size_t ninsert = 0;
  size_t nblind = 0;
  size_t after = 0;
  size_t i;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (out_revision != NULL) {
    *out_revision = 0;
  }
  memset(&touched, 0, sizeof touched);
  touched.alloc = alloc;
  /* The languages it holds afterwards: those it does not plan, and those it
   * plans a tree for. */
  for (i = 0; i < m->now.n; i++) {
    size_t k;
    int planned = 0;
    for (k = 0; k < m->n && !planned; k++) {
      planned = same_span(m->v[k].lang, m->v[k].lang_len, m->now.v[i].tag,
          m->now.v[i].len);
    }
    after += planned ? 0 : 1;
  }
  for (i = 0; i < m->n; i++) {
    after += m->v[i].n > 0 ? 1 : 0;
    if (part_writes(&m->v[i])) {
      nclose += m->v[i].nclose;
      ninsert += m->v[i].ninsert;
      nblind += m->v[i].nblind;
      if (rc == GDEFSTOR_OK) {
        rc = langs_add(&touched, m->v[i].lang, m->v[i].lang_len, NULL);
      }
    }
  }
  if (rc == GDEFSTOR_OK && after > GDEFSTOR_MODEL_MAX_LANGUAGES) {
    rc = GDEFSTOR_ERR_LIMIT;
  }
  if (rc == GDEFSTOR_OK && touched.n == 0) {
    langs_release(&touched);
    return GDEFSTOR_OK;
  }
  if (rc == GDEFSTOR_OK && head == UINT64_MAX) {
    rc = GDEFSTOR_ERR_LIMIT;
  }
  if (rc == GDEFSTOR_OK) {
    closes = (GDEFSTOR_ModelClose *)gdefstor_mem0(alloc,
        (nclose + 1) * sizeof *closes);
    inserts = (GDEFSTOR_ModelItem *)gdefstor_mem0(alloc,
        (ninsert + 1) * sizeof *inserts);
    blinds = (GDEFSTOR_ModelBlind *)gdefstor_mem0(alloc,
        (nblind + 1) * sizeof *blinds);
    if (closes == NULL || inserts == NULL || blinds == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  /* The revision's languages: sorted, joined by ','. */
  for (i = 0; rc == GDEFSTOR_OK && i < touched.n; i++) {
    if (i > 0) {
      rc = gdefstor_buf_append(&text, &text_len, &text_cap, alloc, ",", 1);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&text, &text_len, &text_cap, alloc,
          touched.v[i].tag, touched.v[i].len);
    }
  }
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(alloc, closes);
    gcu_allocator_free(alloc, inserts);
    gcu_allocator_free(alloc, blinds);
    gcu_allocator_free(alloc, text);
    langs_release(&touched);
    return rc;
  }
  memset(&r, 0, sizeof r);
  r.revision = head + 1;
  r.who = meta->who_len == 0 ? "" : meta->who;
  r.who_len = meta->who_len;
  r.when_ms = meta->when_ms;
  r.why = meta->why_len == 0 ? "" : meta->why;
  r.why_len = meta->why_len;
  r.languages = text;
  r.languages_len = text_len;
  nclose = 0;
  ninsert = 0;
  nblind = 0;
  for (i = 0; i < m->n; i++) {
    const Save * s = &m->v[i];
    size_t k;
    if (!part_writes(s)) {
      continue;
    }
    for (k = 0; k < s->nclose; k++) {
      closes[nclose++] = s->closes[k];
    }
    for (k = 0; k < s->ninsert; k++) {
      inserts[ninsert] = s->inserts[k];
      inserts[ninsert].valid_from = r.revision;
      inserts[ninsert].valid_to = 0;
      ninsert += 1;
    }
    /* A blind row indexes the version written in this same apply. */
    for (k = 0; k < s->nblind; k++) {
      blinds[nblind] = s->blinds[k];
      blinds[nblind].valid_from = r.revision;
      nblind += 1;
    }
  }
  memset(&a, 0, sizeof a);
  a.row = row;
  a.revision = &r;
  a.last_item = m->next_item;
  a.closes = closes;
  a.close_count = nclose;
  a.inserts = inserts;
  a.insert_count = ninsert;
  a.blinds = nblind == 0 ? NULL : blinds;
  a.blind_count = nblind;
  rc = write->store->model.apply(write->store->model.ctx, m->txn, &a);
  gcu_allocator_free(alloc, closes);
  gcu_allocator_free(alloc, inserts);
  gcu_allocator_free(alloc, blinds);
  gcu_allocator_free(alloc, text);
  langs_release(&touched);
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

/* Plan @p count languages' trees (a NULL tree removes its language). */
static GDEFSTOR_Result multi_trees(Multi * m, const GDEFSTOR_LanguageTree * trees,
    size_t count) {
  size_t i;
  GDEFSTOR_Result rc = multi_reserve(m, count);
  for (i = 0; rc == GDEFSTOR_OK && i < count; i++) {
    rc = trees[i].tree == NULL
             ? multi_remove(m, trees[i].language, trees[i].language_len)
             : multi_tree(m, trees[i].language, trees[i].language_len,
                   trees[i].tree);
  }
  return rc;
}

static GDEFSTOR_Result check_trees(const GDEFSTOR_LanguageTree * trees,
    size_t count) {
  size_t i;
  if (trees == NULL && count != 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (count > GDEFSTOR_MODEL_MAX_LANGUAGES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  for (i = 0; i < count; i++) {
    if (trees[i].size != sizeof trees[i] ||
        !gdefstor_model_lang_ok(trees[i].language, trees[i].language_len)) {
      return GDEFSTOR_ERR_INVALID;
    }
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_entity_save_languages(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    const GDEFSTOR_RevisionMeta * meta, const GDEFSTOR_LanguageTree * trees,
    size_t count, uint64_t * out_revision) {
  void * txn = NULL;
  Ent e;
  GDEFSTOR_Type * type = NULL;
  Multi m;
  GDEFSTOR_Result rc = check_trees(trees, count);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = open_save(write, public_id, public_id_len, base_revision, meta, &txn,
      &e, &type);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = multi_begin(&m, write, txn, &e, type);
  if (rc == GDEFSTOR_OK) {
    rc = multi_trees(&m, trees, count);
  }
  if (rc == GDEFSTOR_OK) {
    rc = multi_apply(&m, write, e.row, e.revision, meta, out_revision);
  }
  multi_release(&m);
  gdefstor_type_free(type);
  ent_release(write->store->alloc, &e);
  return rc;
}

GDEFSTOR_Result gdefstor_entity_save(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    const GDEFSTOR_RevisionMeta * meta, const char * language,
    size_t language_len, const GDEFSTOR_Tree * tree, uint64_t * out_revision) {
  GDEFSTOR_LanguageTree one;
  if (tree == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&one, 0, sizeof one);
  one.size = sizeof one;
  one.language = language;
  one.language_len = language_len;
  one.tree = tree;
  return gdefstor_entity_save_languages(write, public_id, public_id_len,
      base_revision, meta, &one, 1, out_revision);
}

GDEFSTOR_Result gdefstor_entity_remove_language(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    const GDEFSTOR_RevisionMeta * meta, const char * language,
    size_t language_len, uint64_t * out_revision) {
  GDEFSTOR_LanguageTree one;
  memset(&one, 0, sizeof one);
  one.size = sizeof one;
  one.language = language;
  one.language_len = language_len;
  return gdefstor_entity_save_languages(write, public_id, public_id_len,
      base_revision, meta, &one, 1, out_revision);
}

/* ---- reverting ----------------------------------------------------------- */

GDEFSTOR_Result gdefstor_entity_revert(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    uint64_t to_revision, const GDEFSTOR_RevisionMeta * meta,
    uint64_t * out_revision) {
  void * txn = NULL;
  Ent e;
  GDEFSTOR_Type * type = NULL;
  Langs past;
  Multi m;
  GDEFSTOR_Result rc;
  rc = open_save(write, public_id, public_id_len, base_revision, meta, &txn,
      &e, &type);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&past, 0, sizeof past);
  past.alloc = write->store->alloc;
  rc = multi_begin(&m, write, txn, &e, type);
  m.revert = 1;
  if (rc == GDEFSTOR_OK && (to_revision == 0 || to_revision > e.revision)) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  if (rc == GDEFSTOR_OK) {
    rc = load_langs(write->store, txn, write->tenant, write->tenant_len, e.row,
        to_revision, &past);
  }
  if (rc == GDEFSTOR_OK) {
    rc = multi_reserve(&m, m.now.n + past.n);
  }
  /* Every language it has now or had then, in byte order: back to its past
   * tree, or removed when it had none. */
  if (rc == GDEFSTOR_OK) {
    size_t i = 0;
    size_t j = 0;
    while (rc == GDEFSTOR_OK && (i < m.now.n || j < past.n)) {
      int c = i == m.now.n ? 1
              : j == past.n
                  ? -1
                  : lang_cmp(m.now.v[i].tag, m.now.v[i].len, past.v[j].tag,
                        past.v[j].len);
      if (c < 0) {
        rc = multi_remove(&m, m.now.v[i].tag, m.now.v[i].len);
        i += 1;
      } else {
        rc = multi_revert(&m, past.v[j].tag, past.v[j].len, to_revision);
        i += c == 0 ? 1 : 0;
        j += 1;
      }
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = multi_apply(&m, write, e.row, e.revision, meta, out_revision);
  }
  multi_release(&m);
  langs_release(&past);
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
    size_t public_id_len, const char * language, size_t language_len,
    uint64_t revision, GDEFSTOR_Tree ** out_tree) {
  Ent e;
  GDEFSTOR_Type * type = NULL;
  Items it;
  GDEFSTOR_Result rc;
  if (out_tree == NULL || !gdefstor_model_lang_ok(language, language_len)) {
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
    rc = load_items(store, NULL, tenant, tenant_len, e.row, revision, language,
        language_len, 0, &it);
  }
  if (rc == GDEFSTOR_OK) {
    /* No item: no tree in that language then. */
    rc = it.n == 0 ? GDEFSTOR_ERR_INVALID
                   : build_tree(store->alloc, type, &it, out_tree);
    items_release(&it);
  }
  gdefstor_type_free(type);
  ent_release(store->alloc, &e);
  return rc;
}

struct GDEFSTOR_Languages {
  Langs l;
};

GDEFSTOR_Result gdefstor_entity_languages(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, uint64_t revision, GDEFSTOR_Languages ** out) {
  Ent e;
  GDEFSTOR_Languages * l;
  GDEFSTOR_Result rc;
  if (out == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = open_read(store, tenant, tenant_len, public_id, public_id_len, &e,
      NULL);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (revision > e.revision) {
    ent_release(store->alloc, &e);
    return GDEFSTOR_ERR_INVALID;
  }
  l = (GDEFSTOR_Languages *)gdefstor_mem0(store->alloc, sizeof *l);
  if (l == NULL) {
    ent_release(store->alloc, &e);
    return GDEFSTOR_ERR_OOM;
  }
  l->l.alloc = store->alloc;
  if (e.revision != 0) {
    rc = load_langs(store, NULL, tenant, tenant_len, e.row, revision, &l->l);
  }
  ent_release(store->alloc, &e);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(store->alloc, l);
    return rc;
  }
  *out = l;
  return GDEFSTOR_OK;
}

size_t gdefstor_languages_count(const GDEFSTOR_Languages * l) {
  return l == NULL ? 0 : l->l.n;
}

GDEFSTOR_Result gdefstor_languages_get(const GDEFSTOR_Languages * l,
    size_t index, const char ** out_language, size_t * out_len) {
  if (l == NULL || out_language == NULL || out_len == NULL ||
      index >= l->l.n) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_language = l->l.v[index].tag;
  *out_len = l->l.v[index].len;
  return GDEFSTOR_OK;
}

void gdefstor_languages_free(GDEFSTOR_Languages * l) {
  const GDEFSTOR_Allocator * alloc;
  if (l == NULL) {
    return;
  }
  alloc = l->l.alloc;
  langs_release(&l->l);
  gcu_allocator_free(alloc, l);
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
    gcu_allocator_free(h->alloc, (void *)h->revs[i].languages);
  }
  for (i = 0; i < h->nstate; i++) {
    gcu_allocator_free(h->alloc, (void *)h->states[i].actor);
  }
  gcu_allocator_free(h->alloc, h->revs);
  gcu_allocator_free(h->alloc, h->states);
  gcu_allocator_free(h->alloc, h);
}

/* A revision's languages as stored: tags in ascending byte order, each once,
 * joined by ','; at least one, since a revision writes a row. */
static int langs_text_ok(const char * p, size_t n) {
  size_t at = 0;
  const char * prev = NULL;
  size_t prev_len = 0;
  if (p == NULL || n == 0) {
    return 0;
  }
  while (at <= n) {
    size_t end = at;
    while (end < n && p[end] != ',') {
      end += 1;
    }
    if (!gdefstor_model_lang_ok(p + at, end - at) ||
        (prev != NULL && lang_cmp(prev, prev_len, p + at, end - at) >= 0)) {
      return 0;
    }
    prev = p + at;
    prev_len = end - at;
    at = end + 1;
  }
  return 1;
}

/* Whether a revision's language text names @p tag. */
static int langs_text_has(const char * p, size_t n, const char * tag,
    size_t len) {
  size_t at = 0;
  while (at <= n) {
    size_t end = at;
    while (end < n && p[end] != ',') {
      end += 1;
    }
    if (same_span(p + at, end - at, tag, len)) {
      return 1;
    }
    at = end + 1;
  }
  return 0;
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
  if (!langs_text_ok(r->languages, r->languages_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  o = &h->revs[h->nrev];
  *o = *r;
  o->who = gdefstor_dup(h->alloc, r->who, r->who_len);
  o->why = gdefstor_dup(h->alloc, r->why, r->why_len);
  o->languages = gdefstor_dup(h->alloc, r->languages, r->languages_len);
  if (o->who == NULL || o->why == NULL || o->languages == NULL) {
    gcu_allocator_free(h->alloc, (void *)o->who);
    gcu_allocator_free(h->alloc, (void *)o->why);
    gcu_allocator_free(h->alloc, (void *)o->languages);
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
    size_t public_id_len, const char * language, size_t language_len,
    GDEFSTOR_History ** out_history) {
  Ent e;
  GDEFSTOR_History * h;
  GDEFSTOR_ModelVisitor v;
  size_t * idx = NULL;
  size_t i;
  GDEFSTOR_Result rc;
  if (out_history == NULL ||
      (language_len == 0 ? language != NULL
                         : !gdefstor_model_lang_ok(language, language_len))) {
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
  if (rc == GDEFSTOR_OK && language_len != 0) {
    /* One language's history: the revisions that changed its tree. */
    size_t k = 0;
    for (i = 0; i < h->nrev; i++) {
      GDEFSTOR_ModelRevision * r = &h->revs[i];
      if (langs_text_has(r->languages, r->languages_len, language,
              language_len)) {
        h->revs[k++] = *r;
      } else {
        gcu_allocator_free(h->alloc, (void *)r->who);
        gcu_allocator_free(h->alloc, (void *)r->why);
        gcu_allocator_free(h->alloc, (void *)r->languages);
      }
    }
    h->nrev = k;
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
  out->languages = r->languages;
  out->languages_len = r->languages_len;
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
  char lang[GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES];
  size_t lang_len;
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
    size_t public_id_len, const char * language, size_t language_len,
    uint64_t from, uint64_t to, GDEFSTOR_Diff ** out_diff) {
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
  if (out_diff == NULL ||
      (language_len == 0 ? language != NULL
                         : !gdefstor_model_lang_ok(language, language_len))) {
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
    rc = load_items(store, NULL, tenant, tenant_len, e.row, from, language,
        language_len, 0, &a);
  }
  if (rc == GDEFSTOR_OK && to > 0) {
    rc = load_items(store, NULL, tenant, tenant_len, e.row, to, language,
        language_len, 0, &b);
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
    if (x != NULL && y != NULL && x->item == y->item &&
        !same_span(x->lang, x->lang_len, y->lang, y->lang_len)) {
      /* An item belongs to one language for life. */
      rc = GDEFSTOR_ERR_CORRUPT;
      break;
    }
    o->item = which->item;
    o->field = type->group_owner[which->group];
    memcpy(o->lang, which->lang, which->lang_len);
    o->lang_len = which->lang_len;
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
  out->language = x->lang;
  out->language_len = x->lang_len;
  return GDEFSTOR_OK;
}

/* ---- drafts -------------------------------------------------------------- */

typedef struct DraftCatch {
  const GDEFSTOR_Allocator * alloc;
  int seen;
  char * type;
  size_t type_len;
  uint64_t base;
  uint8_t * json;     /* the draft document */
  size_t json_len;
  GDEFSTOR_DraftPart * parts; /* its languages, in order */
  size_t nparts;
} DraftCatch;

static void draft_catch_release(DraftCatch * c) {
  gcu_allocator_free(c->alloc, c->type);
  gcu_allocator_free(c->alloc, c->json);
  gcu_allocator_free(c->alloc, c->parts);
  c->type = NULL;
  c->json = NULL;
  c->parts = NULL;
  c->nparts = 0;
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
  {
    GDEFSTOR_Result rc = gdefstor_model_draft_unpack(c->alloc, c->json,
        c->json_len, &c->parts, &c->nparts);
    if (rc != GDEFSTOR_OK) {
      draft_catch_release(c);
      c->seen = 0;
      return rc;
    }
  }
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

static int by_draft_lang(const void * ctx, size_t a, size_t b) {
  const GDEFSTOR_DraftTree * t = (const GDEFSTOR_DraftTree *)ctx;
  return lang_cmp(t[a].language, t[a].language_len, t[b].language,
      t[b].language_len);
}

/* The draft document of @p count trees: each checked, in language order. */
static GDEFSTOR_Result draft_document(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_DraftTree * trees, size_t count, uint8_t ** out,
    size_t * out_len) {
  const GDEFSTOR_DraftTree ** sorted = NULL;
  size_t * idx = NULL;
  size_t i;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (trees == NULL && count != 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (count > GDEFSTOR_MODEL_MAX_LANGUAGES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  for (i = 0; rc == GDEFSTOR_OK && i < count; i++) {
    if (trees[i].size != sizeof trees[i] || trees[i].json == NULL ||
        !gdefstor_model_lang_ok(trees[i].language, trees[i].language_len)) {
      rc = GDEFSTOR_ERR_INVALID;
    } else {
      rc = gdefstor_model_json_ok(alloc, trees[i].json, trees[i].json_len);
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(alloc, count, by_draft_lang, trees, &idx);
  }
  if (rc == GDEFSTOR_OK) {
    sorted = (const GDEFSTOR_DraftTree **)gdefstor_mem(alloc,
        (count + 1) * sizeof *sorted);
    if (sorted == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  for (i = 0; rc == GDEFSTOR_OK && i < count; i++) {
    sorted[i] = &trees[idx[i]];
    if (i > 0 && by_draft_lang(trees, idx[i - 1], idx[i]) == 0) {
      /* A language named twice. */
      rc = GDEFSTOR_ERR_INVALID;
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_draft_pack(alloc, sorted, count, out, out_len);
  }
  gcu_allocator_free(alloc, (void *)sorted);
  gcu_allocator_free(alloc, idx);
  return rc;
}

static GDEFSTOR_Result draft_store(GDEFSTOR_Write * write,
    GDEFSTOR_Store * store, void * txn, const char * type, size_t type_len,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    const uint8_t * json, size_t json_len);

GDEFSTOR_Result gdefstor_draft_put(GDEFSTOR_Write * write, const char * type,
    size_t type_len, const char * public_id, size_t public_id_len,
    uint64_t base_revision, const GDEFSTOR_DraftTree * trees, size_t count) {
  GDEFSTOR_Store * store;
  void * txn = NULL;
  uint8_t * json = NULL;
  size_t json_len = 0;
  GDEFSTOR_Result rc = write_store(write, &store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_model_id_ok(public_id, public_id_len,
          write->limits.max_public_id_bytes, 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = draft_document(store->alloc, trees, count, &json, &json_len);
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_txn(write, &txn);
  }
  if (rc == GDEFSTOR_OK) {
    rc = draft_store(write, store, txn, type, type_len, public_id,
        public_id_len, base_revision, json, json_len);
  }
  gcu_allocator_free(store->alloc, json);
  return rc;
}

/* Store a draft document after the entity or the reservation is checked. */
static GDEFSTOR_Result draft_store(GDEFSTOR_Write * write,
    GDEFSTOR_Store * store, void * txn, const char * type, size_t type_len,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    const uint8_t * json, size_t json_len) {
  GDEFSTOR_ModelDraft d;
  GDEFSTOR_Type * t = NULL;
  Ent e;
  int exists;
  GDEFSTOR_Result rc;
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
  out->language_count = draft->c.nparts;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_draft_tree(const GDEFSTOR_Draft * draft,
    size_t index, GDEFSTOR_DraftTree * out) {
  const GDEFSTOR_DraftPart * p;
  if (draft == NULL || out == NULL || out->size != sizeof *out ||
      index >= draft->c.nparts) {
    return GDEFSTOR_ERR_INVALID;
  }
  p = &draft->c.parts[index];
  out->language = p->lang;
  out->language_len = p->lang_len;
  out->json = draft->c.json + p->off;
  out->json_len = p->len;
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
  GDEFSTOR_Tree ** trees = NULL;
  GDEFSTOR_LanguageTree * lt = NULL;
  Multi m;
  int have_m = 0;
  uint64_t row = 0;
  size_t i;
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
    trees = (GDEFSTOR_Tree **)gdefstor_mem0(store->alloc,
        (c.nparts + 1) * sizeof *trees);
    lt = (GDEFSTOR_LanguageTree *)gdefstor_mem0(store->alloc,
        (c.nparts + 1) * sizeof *lt);
    if (trees == NULL || lt == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  for (i = 0; rc == GDEFSTOR_OK && i < c.nparts; i++) {
    const GDEFSTOR_DraftPart * p = &c.parts[i];
    rc = gdefstor_model_from_json(type, c.json + p->off, p->len, store->alloc,
        &trees[i]);
    if (rc == GDEFSTOR_ERR_FORMAT || rc == GDEFSTOR_ERR_CORRUPT) {
      rc = GDEFSTOR_ERR_INVALID;
    }
    lt[i].size = sizeof lt[i];
    lt[i].language = p->lang;
    lt[i].language_len = p->lang_len;
    lt[i].tree = trees[i];
  }
  /* Every language is checked first, then (a reserved id) the entity is
   * created and the revision written, so a draft that does not validate
   * creates nothing. */
  if (rc == GDEFSTOR_OK) {
    rc = multi_begin(&m, write, txn, &e, type);
    have_m = 1;
  }
  if (rc == GDEFSTOR_OK) {
    rc = multi_trees(&m, lt, c.nparts);
  }
  if (rc == GDEFSTOR_OK && !exists) {
    GDEFSTOR_ModelEntity me;
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
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = multi_apply(&m, write, exists ? e.row : row, e.revision, meta,
        out_revision);
  }
  if (rc == GDEFSTOR_OK) {
    rc = store->model.draft_delete(store->model.ctx, txn, public_id,
        public_id_len);
    if (rc != GDEFSTOR_OK) {
      rc = gdefstor_model_break(write, rc);
    }
  }
  if (have_m) {
    multi_release(&m);
  }
  for (i = 0; trees != NULL && i < c.nparts; i++) {
    gdefstor_tree_free(trees[i]);
  }
  gcu_allocator_free(store->alloc, trees);
  gcu_allocator_free(store->alloc, lt);
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
  l_hex(l, r->languages, r->languages_len);
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
  /* Last, so that the words before it keep their places. */
  l_hex(l, i->language, i->language_len);
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

static GDEFSTOR_Result dump_blind(void * ctx, uint64_t row,
    const GDEFSTOR_ModelBlind * b) {
  Lines * l = (Lines *)ctx;
  if (b->mac == NULL) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  l_str(l, "B");
  l_u64(l, row);
  l_u64(l, b->item);
  l_u64(l, b->valid_from);
  l_u64(l, b->field);
  l_hex(l, b->subject, b->subject_len);
  l_hex(l, b->mac, GDEFSTOR_BLIND_MAC_BYTES);
  l_u64(l, b->key_id);
  l_u64(l, b->version);
  l_hex(l, b->language, b->language_len);
  l_end(l);
  return l->rc;
}

static GDEFSTOR_Result dump_record(void * ctx, const GDEFSTOR_ModelRecord * r) {
  Lines * l = (Lines *)ctx;
  if (r == NULL || (r->bytes == NULL && r->len != 0)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  l_str(l, "K");
  l_hex(l, r->ns, r->ns_len);
  l_hex(l, r->key, r->key_len);
  l_hex(l, r->bytes, r->len);
  l_end(l);
  return l->rc;
}

/* A counter never advanced reads 0 and is not a row: a backend that keeps a
 * row for its floor alone lists it at 0, which is left out. */
static GDEFSTOR_Result dump_counter(void * ctx, int kind, const char * name,
    size_t name_len, uint64_t value) {
  Lines * l = (Lines *)ctx;
  if (kind != GDEFSTOR_MODEL_COUNTER && kind != GDEFSTOR_MODEL_STREAM) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (value == 0) {
    return GDEFSTOR_OK;
  }
  l_str(l, "C");
  l_u64(l, (uint64_t)kind);
  l_hex(l, name, name_len);
  l_u64(l, value);
  l_end(l);
  return l->rc;
}

static GDEFSTOR_Result dump_outbox(void * ctx, const GDEFSTOR_ModelOutbox * o) {
  Lines * l = (Lines *)ctx;
  if (o == NULL || (o->payload == NULL && o->payload_len != 0)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  l_str(l, "O");
  l_hex(l, o->stream, o->stream_len);
  l_u64(l, o->seq);
  l_hex(l, o->public_id, o->public_id_len);
  l_hex(l, o->format, o->format_len);
  l_hex(l, o->payload, o->payload_len);
  l_i64(l, o->lease_until_ms);
  l_u64(l, o->failures);
  l_u64(l, (uint64_t)(o->parked != 0));
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
  if (gdefstor_model_has_blind(store)) {
    v.blind = dump_blind;
  }
  if (gdefstor_model_has_sys(store)) {
    v.record = dump_record;
    v.counter = dump_counter;
    v.outbox = dump_outbox;
  }
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
