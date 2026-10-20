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
 * The query form over entities (query.h, "Over entities"): checking a query
 * against the declarations, and the reference engine every backend's answer
 * must equal (gdefstor_model_scan()).
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/* ---- checking ------------------------------------------------------------ */

/* The type an operand has: a GDEFSTOR_FieldType, and a decimal's scale. A
 * constant list with no values has type 0 and matches any type. A decimal
 * constant has scale -1: it takes the other side's. */
typedef struct Class {
  int type;
  int scale;
} Class;

typedef struct Check {
  GDEFSTOR_Store * store;
  const char * tenant;
  size_t tenant_len;
  const GDEFSTOR_Type * outer;
  const GDEFSTOR_Type ** exists; /* one per EXISTS, depth first */
  size_t nexists;
  size_t cap;
  size_t nodes;
} Check;

static void check_release(Check * c) {
  size_t i;
  for (i = 0; i < c->nexists; i++) {
    gdefstor_type_free((GDEFSTOR_Type *)c->exists[i]);
  }
  gcu_allocator_free(c->store->alloc, (void *)c->exists);
  c->exists = NULL;
  c->nexists = 0;
}

/* The scalar field an operand path names, for a filter or a sort. */
static GDEFSTOR_Result resolve_field(const GDEFSTOR_Type * t,
    const GDEFSTOR_QueryOperand * o, size_t * out) {
  size_t fi;
  const GDEFSTOR_MField * f;
  if (o->field == NULL || o->field_len == 0 ||
      gdefstor_type_find(t, o->field, o->field_len, &fi) != GDEFSTOR_OK) {
    return GDEFSTOR_ERR_INVALID;
  }
  f = &t->f[fi];
  /* A personal value is ciphertext: only a blind index could match it. */
  if (f->type == GDEFSTOR_FIELD_GROUP || f->eff_retired ||
      (f->flags & GDEFSTOR_FIELD_PERSONAL) != 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out = fi;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result check_values(const GDEFSTOR_QueryOperand * o,
    Class * out) {
  size_t i;
  int type = 0;
  if (o->value_count > GDEFSTOR_QUERY_MAX_TEXTS) {
    return GDEFSTOR_ERR_LIMIT;
  }
  if (o->values == NULL && o->value_count != 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; i < o->value_count; i++) {
    const GDEFSTOR_Value * v = &o->values[i];
    if (v->type < GDEFSTOR_FIELD_TEXT || v->type >= GDEFSTOR_FIELD_GROUP ||
        (type != 0 && v->type != type) || v->subject_len != 0 ||
        (v->bytes == NULL && v->len != 0)) {
      return GDEFSTOR_ERR_INVALID;
    }
    type = v->type;
    if (v->len > GDEFSTOR_QUERY_MAX_TEXT_BYTES) {
      return GDEFSTOR_ERR_LIMIT;
    }
    if ((v->type == GDEFSTOR_FIELD_TEXT &&
            !gdefstor_model_utf8_ok(v->bytes, v->len)) ||
        (v->type == GDEFSTOR_FIELD_FLOAT &&
            !(v->real == v->real && v->real - v->real == 0.0)) ||
        (v->type == GDEFSTOR_FIELD_BOOLEAN && v->integer != 0 &&
            v->integer != 1)) {
      return GDEFSTOR_ERR_INVALID;
    }
  }
  out->type = type;
  out->scale = type == GDEFSTOR_FIELD_DECIMAL ? -1 : 0;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result check_operand(const Check * c,
    const GDEFSTOR_QueryOperand * o, const GDEFSTOR_Type * here,
    int in_exists, Class * out) {
  int outer = (o->flags & GDEFSTOR_QUERY_OUTER) != 0;
  const GDEFSTOR_Type * t = outer ? c->outer : here;
  if ((o->flags & ~GDEFSTOR_QUERY_OUTER) != 0 || (outer && !in_exists)) {
    return GDEFSTOR_ERR_INVALID;
  }
  switch (o->kind) {
  case GDEFSTOR_QUERY_FIELD: {
    size_t fi;
    GDEFSTOR_Result rc = resolve_field(t, o, &fi);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    out->type = t->f[fi].type;
    out->scale = t->f[fi].scale;
    return GDEFSTOR_OK;
  }
  case GDEFSTOR_QUERY_VALUES:
    if (outer) {
      return GDEFSTOR_ERR_INVALID;
    }
    return check_values(o, out);
  case GDEFSTOR_QUERY_META:
    out->scale = 0;
    switch (o->meta) {
    case GDEFSTOR_META_PUBLISHED:
    case GDEFSTOR_META_DELETED:
      out->type = GDEFSTOR_FIELD_BOOLEAN;
      return GDEFSTOR_OK;
    case GDEFSTOR_META_CREATOR:
      out->type = GDEFSTOR_FIELD_REFERENCE;
      return GDEFSTOR_OK;
    case GDEFSTOR_META_CREATED:
      out->type = GDEFSTOR_FIELD_DATETIME;
      return GDEFSTOR_OK;
    case GDEFSTOR_META_REVISION:
      out->type = GDEFSTOR_FIELD_INTEGER;
      return GDEFSTOR_OK;
    default:
      return GDEFSTOR_ERR_INVALID;
    }
  case GDEFSTOR_QUERY_ENTITY:
    out->type = GDEFSTOR_FIELD_REFERENCE;
    out->scale = 0;
    return GDEFSTOR_OK;
  default:
    /* COLUMN and CONST are the projection form's. */
    return GDEFSTOR_ERR_INVALID;
  }
}

static int ordered_type(int type) {
  return type == 0 || type == GDEFSTOR_FIELD_INTEGER ||
         type == GDEFSTOR_FIELD_DECIMAL || type == GDEFSTOR_FIELD_FLOAT ||
         type == GDEFSTOR_FIELD_DATE || type == GDEFSTOR_FIELD_DATETIME;
}

static int same_class(const Class * a, const Class * b) {
  if (a->type == 0 || b->type == 0) {
    return 1;
  }
  if (a->type != b->type) {
    return 0;
  }
  return a->type != GDEFSTOR_FIELD_DECIMAL || a->scale < 0 || b->scale < 0 ||
         a->scale == b->scale;
}

static GDEFSTOR_Result check_node(Check * c, const GDEFSTOR_QueryNode * n,
    const GDEFSTOR_Type * here, size_t depth, int in_exists) {
  size_t i;
  GDEFSTOR_Result rc;
  if (n == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (depth > GDEFSTOR_QUERY_MAX_DEPTH) {
    return GDEFSTOR_ERR_LIMIT;
  }
  c->nodes += 1;
  if (c->nodes > GDEFSTOR_QUERY_MAX_NODES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  switch (n->op) {
  case GDEFSTOR_QUERY_TRUE:
  case GDEFSTOR_QUERY_FALSE:
    return n->child_count == 0 ? GDEFSTOR_OK : GDEFSTOR_ERR_INVALID;
  case GDEFSTOR_QUERY_AND:
  case GDEFSTOR_QUERY_OR:
  case GDEFSTOR_QUERY_NOT:
    if ((n->op == GDEFSTOR_QUERY_NOT && n->child_count != 1) ||
        (n->child_count > 0 && n->children == NULL)) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (n->child_count > GDEFSTOR_QUERY_MAX_NODES) {
      return GDEFSTOR_ERR_LIMIT;
    }
    for (i = 0; i < n->child_count; i++) {
      rc = check_node(c, n->children[i], here, depth + 1, in_exists);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    return GDEFSTOR_OK;
  case GDEFSTOR_QUERY_EXISTS: {
    GDEFSTOR_Type * inner = NULL;
    if (in_exists || n->child_count != 1 || n->children == NULL ||
        n->exists_type == NULL) {
      return GDEFSTOR_ERR_INVALID;
    }
    rc = gdefstor_model_type_load(c->store, NULL, c->tenant, c->tenant_len,
        n->exists_type, n->exists_type_len, &inner);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    if (c->nexists == c->cap) {
      size_t grown = c->cap == 0 ? 4 : c->cap * 2;
      const GDEFSTOR_Type ** next = (const GDEFSTOR_Type **)
          gcu_allocator_realloc(c->store->alloc, (void *)c->exists,
              grown * sizeof *next);
      if (next == NULL) {
        gdefstor_type_free(inner);
        return GDEFSTOR_ERR_OOM;
      }
      c->exists = next;
      c->cap = grown;
    }
    c->exists[c->nexists] = inner;
    c->nexists += 1;
    return check_node(c, n->children[0], inner, depth + 1, 1);
  }
  case GDEFSTOR_QUERY_EQ:
  case GDEFSTOR_QUERY_NE:
  case GDEFSTOR_QUERY_IN:
  case GDEFSTOR_QUERY_LT:
  case GDEFSTOR_QUERY_LE:
  case GDEFSTOR_QUERY_GT:
  case GDEFSTOR_QUERY_GE: {
    Class l;
    Class r;
    if (n->child_count != 0) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (n->op == GDEFSTOR_QUERY_IN && n->right.kind != GDEFSTOR_QUERY_VALUES) {
      return GDEFSTOR_ERR_INVALID;
    }
    rc = check_operand(c, &n->left, here, in_exists, &l);
    if (rc == GDEFSTOR_OK) {
      rc = check_operand(c, &n->right, here, in_exists, &r);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    if (!same_class(&l, &r)) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (n->op >= GDEFSTOR_QUERY_LT && n->op <= GDEFSTOR_QUERY_GE &&
        (!ordered_type(l.type) || !ordered_type(r.type))) {
      return GDEFSTOR_ERR_INVALID;
    }
    return GDEFSTOR_OK;
  }
  case GDEFSTOR_QUERY_BLIND: {
    /* Blind-index equality: a personal field of the entity this node is
     * decided on, and one MAC. */
    const GDEFSTOR_QueryOperand * l = &n->left;
    const GDEFSTOR_QueryOperand * r = &n->right;
    const GDEFSTOR_MField * f;
    size_t fi;
    if (n->child_count != 0 || l->kind != GDEFSTOR_QUERY_FIELD ||
        l->flags != 0 || r->kind != GDEFSTOR_QUERY_CONST || r->flags != 0 ||
        r->text_count != 1 || r->texts == NULL || r->texts[0].bytes == NULL ||
        r->texts[0].len != GDEFSTOR_BLIND_MAC_BYTES || l->field == NULL ||
        l->field_len == 0 ||
        gdefstor_type_find(here, l->field, l->field_len, &fi) != GDEFSTOR_OK) {
      return GDEFSTOR_ERR_INVALID;
    }
    f = &here->f[fi];
    if (f->type == GDEFSTOR_FIELD_GROUP || f->eff_retired ||
        (f->flags & GDEFSTOR_FIELD_PERSONAL) == 0) {
      return GDEFSTOR_ERR_INVALID;
    }
    return gdefstor_model_has_blind(c->store) ? GDEFSTOR_OK
                                              : GDEFSTOR_ERR_UNSUPPORTED;
  }
  default:
    return GDEFSTOR_ERR_INVALID;
  }
}

static GDEFSTOR_Result check_sort(const Check * c, const GDEFSTOR_QuerySort * s) {
  const GDEFSTOR_QueryOperand * o = &s->key;
  Class k;
  if (s->numeric != 0 || o->flags != 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (o->kind == GDEFSTOR_QUERY_FIELD) {
    size_t fi;
    GDEFSTOR_Result rc = resolve_field(c->outer, o, &fi);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    /* One value per entity: a single field of the root group. */
    if (c->outer->f[fi].parent != GDEFSTOR_NONE ||
        (c->outer->f[fi].flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
      return GDEFSTOR_ERR_INVALID;
    }
    return GDEFSTOR_OK;
  }
  if (o->kind != GDEFSTOR_QUERY_META && o->kind != GDEFSTOR_QUERY_ENTITY) {
    return GDEFSTOR_ERR_INVALID;
  }
  return check_operand(c, o, c->outer, 0, &k);
}

/* ---- the entry points ---------------------------------------------------- */

static GDEFSTOR_Result run(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_Query * q, int count_only, GDEFSTOR_ModelHits * hits) {
  Check c;
  GDEFSTOR_Type * outer = NULL;
  GDEFSTOR_ModelPlan plan;
  size_t i;
  GDEFSTOR_Result rc = gdefstor_model_ready(store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (q->sort_count > GDEFSTOR_QUERY_MAX_SORTS) {
    return GDEFSTOR_ERR_LIMIT;
  }
  if ((q->sorts == NULL && q->sort_count != 0) || q->select_count != 0 ||
      q->entity_numeric != 0 ||
      !gdefstor_model_lang_ok(q->language, q->language_len)) {
    /* One language, always: every field in the query reads it. */
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_model_type_load(store, NULL, tenant, tenant_len, type,
      type_len, &outer);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&c, 0, sizeof c);
  c.store = store;
  c.tenant = tenant;
  c.tenant_len = tenant_len;
  c.outer = outer;
  if (q->where != NULL) {
    rc = check_node(&c, q->where, outer, 1, 0);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < q->sort_count; i++) {
    rc = check_sort(&c, &q->sorts[i]);
  }
  if (rc == GDEFSTOR_OK) {
    memset(&plan, 0, sizeof plan);
    plan.alloc = store->alloc;
    plan.type = outer;
    plan.query = q;
    plan.exists_types = c.exists;
    plan.exists_count = c.nexists;
    plan.count_only = count_only;
    plan.limit = q->limit == 0 ? UINT64_MAX
                 : (q->limit > GDEFSTOR_QUERY_LIMIT_MAX ? GDEFSTOR_QUERY_LIMIT_MAX
                                                        : q->limit);
    memset(hits, 0, sizeof *hits);
    rc = store->model.query(store->model.ctx, tenant, tenant_len, &plan, hits);
    if (rc == GDEFSTOR_OK &&
        ((hits->count > 0 && (hits->rows == NULL || hits->public_ids == NULL ||
                                 hits->public_id_lens == NULL)) ||
            hits->count > plan.limit || (count_only && hits->count != 0))) {
      rc = GDEFSTOR_ERR_INTERNAL;
    }
    if (rc != GDEFSTOR_OK) {
      gdefstor_model_hits_release(store->alloc, hits);
    }
  }
  check_release(&c);
  gdefstor_type_free(outer);
  return rc;
}

GDEFSTOR_Result gdefstor_entity_query(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_Query * query, GDEFSTOR_QueryResult ** out_result) {
  GDEFSTOR_ModelHits hits;
  GDEFSTOR_Result rc;
  if (store == NULL || query == NULL || out_result == NULL ||
      query->size != sizeof(GDEFSTOR_Query)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = run(store, tenant, tenant_len, type, type_len, query, 0, &hits);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_query_result_adopt(store->alloc, &hits, out_result);
  gdefstor_model_hits_release(store->alloc, &hits);
  return rc;
}

GDEFSTOR_Result gdefstor_entity_count(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const char * language, size_t language_len,
    const GDEFSTOR_QueryNode * where, uint64_t * out_count) {
  GDEFSTOR_Query q;
  GDEFSTOR_ModelHits hits;
  GDEFSTOR_Result rc;
  if (store == NULL || out_count == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.where = where;
  q.language = language;
  q.language_len = language_len;
  rc = run(store, tenant, tenant_len, type, type_len, &q, 1, &hits);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  *out_count = hits.total;
  gdefstor_model_hits_release(store->alloc, &hits);
  return GDEFSTOR_OK;
}

void gdefstor_model_hits_release(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_ModelHits * hits) {
  size_t i;
  if (hits == NULL) {
    return;
  }
  for (i = 0; i < hits->count; i++) {
    if (hits->public_ids != NULL) {
      gcu_allocator_free(alloc, hits->public_ids[i]);
    }
  }
  gcu_allocator_free(alloc, hits->rows);
  gcu_allocator_free(alloc, hits->public_ids);
  gcu_allocator_free(alloc, hits->public_id_lens);
  memset(hits, 0, sizeof *hits);
}

/* ---- the reference engine ------------------------------------------------ */

/* One entity and its current items, owned. */
typedef struct SItem {
  uint32_t group;
  uint64_t item;
  uint64_t from;
  GDEFSTOR_Value * vals;
  size_t nvals;
  char lang[GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES];
  size_t lang_len;
} SItem;

/* One blind row of an entity: the version and field it indexes, its MAC. */
typedef struct SBlind {
  uint64_t item;
  uint64_t from;
  uint32_t field;
  uint8_t mac[GDEFSTOR_BLIND_MAC_BYTES];
  char lang[GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES];
  size_t lang_len;
} SBlind;

typedef struct Snap {
  uint64_t row;
  char * pid;
  size_t pid_len;
  char * creator;
  size_t creator_len;
  int64_t created_ms;
  uint64_t revision;
  int published;
  int deleted;
  SItem * items;
  size_t nitems;
  size_t capitems;
  SBlind * blinds; /* read only when the query has a blind node */
  size_t nblinds;
  size_t capblinds;
} Snap;

typedef struct Hit {
  uint64_t row;
  char * pid;
  size_t pid_len;
  GDEFSTOR_Value keys[GDEFSTOR_QUERY_MAX_SORTS]; /* owned copies */
} Hit;

typedef struct Inner {
  Snap * v;
  size_t n;
  size_t cap;
} Inner;

typedef struct Engine {
  const GDEFSTOR_ModelPlan * plan;
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_ModelScanFn scan;
  void * scan_ctx;
  Inner * inner;      /* one per EXISTS */
  size_t exists_at;   /* the EXISTS being evaluated, depth first */
  /* collecting */
  Snap cur;
  int have_cur;
  Inner * into;       /* collecting an inner type: keep every entity */
  /* the main pass */
  Hit * hits;
  size_t nhits;
  size_t caphits;
  uint64_t total;
  GDEFSTOR_Result rc;
} Engine;

static void snap_release(const GDEFSTOR_Allocator * alloc, Snap * s) {
  size_t i;
  size_t k;
  for (i = 0; i < s->nitems; i++) {
    for (k = 0; k < s->items[i].nvals; k++) {
      gdefstor_model_value_clear(alloc, &s->items[i].vals[k]);
    }
    gcu_allocator_free(alloc, s->items[i].vals);
  }
  gcu_allocator_free(alloc, s->items);
  gcu_allocator_free(alloc, s->blinds);
  gcu_allocator_free(alloc, s->pid);
  gcu_allocator_free(alloc, s->creator);
  memset(s, 0, sizeof *s);
}

/* A set of values: pointers into a snapshot or the query, or into `own`. */
typedef struct Set {
  const GDEFSTOR_Value ** v;
  size_t n;
  size_t cap;
  GDEFSTOR_Value own;
} Set;

static GDEFSTOR_Result set_add(Engine * e, Set * s, const GDEFSTOR_Value * v) {
  if (s->n == s->cap) {
    size_t grown = s->cap == 0 ? 8 : s->cap * 2;
    const GDEFSTOR_Value ** next = (const GDEFSTOR_Value **)
        gcu_allocator_realloc(e->alloc, (void *)s->v, grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    s->v = next;
    s->cap = grown;
  }
  s->v[s->n] = v;
  s->n += 1;
  return GDEFSTOR_OK;
}

static void set_release(Engine * e, Set * s) {
  gcu_allocator_free(e->alloc, (void *)s->v);
  memset(s, 0, sizeof *s);
}

/* The values an operand has on @p snap (or on @p outer for an OUTER one). */
static GDEFSTOR_Result operand_set(Engine * e, const GDEFSTOR_QueryOperand * o,
    const GDEFSTOR_Type * here, const Snap * snap, const Snap * outer,
    Set * out) {
  const Snap * s = snap;
  const GDEFSTOR_Type * t = here;
  size_t i;
  memset(out, 0, sizeof *out);
  if ((o->flags & GDEFSTOR_QUERY_OUTER) != 0) {
    s = outer;
    t = e->plan->type;
  }
  switch (o->kind) {
  case GDEFSTOR_QUERY_VALUES:
    for (i = 0; i < o->value_count; i++) {
      GDEFSTOR_Result rc = set_add(e, out, &o->values[i]);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    return GDEFSTOR_OK;
  case GDEFSTOR_QUERY_FIELD: {
    size_t fi;
    uint32_t group;
    uint32_t col;
    if (gdefstor_type_find(t, o->field, o->field_len, &fi) != GDEFSTOR_OK) {
      return GDEFSTOR_ERR_INTERNAL;
    }
    if ((t->f[fi].flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
      group = t->f[fi].group;
      col = 0;
    } else {
      group = t->f[fi].in_group;
      col = t->f[fi].column;
    }
    /* Every current item of the field's group is on the path: some item. */
    for (i = 0; i < s->nitems; i++) {
      const SItem * it = &s->items[i];
      if (it->group == group && col < it->nvals && it->vals[col].type != 0) {
        GDEFSTOR_Result rc = set_add(e, out, &it->vals[col]);
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
      }
    }
    return GDEFSTOR_OK;
  }
  case GDEFSTOR_QUERY_ENTITY:
    out->own.type = GDEFSTOR_FIELD_REFERENCE;
    out->own.bytes = (const uint8_t *)s->pid;
    out->own.len = s->pid_len;
    return set_add(e, out, &out->own);
  case GDEFSTOR_QUERY_META:
    switch (o->meta) {
    case GDEFSTOR_META_PUBLISHED:
      out->own.type = GDEFSTOR_FIELD_BOOLEAN;
      out->own.integer = s->published;
      break;
    case GDEFSTOR_META_DELETED:
      out->own.type = GDEFSTOR_FIELD_BOOLEAN;
      out->own.integer = s->deleted;
      break;
    case GDEFSTOR_META_CREATOR:
      if (s->creator_len == 0) {
        return GDEFSTOR_OK;
      }
      out->own.type = GDEFSTOR_FIELD_REFERENCE;
      out->own.bytes = (const uint8_t *)s->creator;
      out->own.len = s->creator_len;
      break;
    case GDEFSTOR_META_CREATED:
      out->own.type = GDEFSTOR_FIELD_DATETIME;
      out->own.integer = s->created_ms;
      break;
    case GDEFSTOR_META_REVISION:
      out->own.type = GDEFSTOR_FIELD_INTEGER;
      out->own.integer = (int64_t)s->revision;
      break;
    default:
      return GDEFSTOR_ERR_INTERNAL;
    }
    return set_add(e, out, &out->own);
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
}

static int relation(int op, int c) {
  switch (op) {
  case GDEFSTOR_QUERY_LT:
    return c < 0;
  case GDEFSTOR_QUERY_LE:
    return c <= 0;
  case GDEFSTOR_QUERY_GT:
    return c > 0;
  case GDEFSTOR_QUERY_GE:
    return c >= 0;
  default:
    return c == 0;
  }
}

static GDEFSTOR_Result eval(Engine * e, const GDEFSTOR_QueryNode * n,
    const GDEFSTOR_Type * here, const Snap * snap, const Snap * outer,
    int * out);

/* Blind equality: some current item of the field's group has a value whose
 * version (the item and its valid_from) has a blind row of this field with
 * this MAC. A blind row of an older version of the item does not count. */
static GDEFSTOR_Result eval_blind(const GDEFSTOR_QueryNode * n,
    const GDEFSTOR_Type * here, const Snap * snap, int * out) {
  const uint8_t * mac = n->right.texts[0].bytes;
  const GDEFSTOR_MField * f;
  uint32_t group;
  uint32_t col;
  size_t fi;
  size_t i;
  size_t k;
  *out = 0;
  if (gdefstor_type_find(here, n->left.field, n->left.field_len, &fi) !=
      GDEFSTOR_OK) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  f = &here->f[fi];
  if ((f->flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
    group = f->group;
    col = 0;
  } else {
    group = f->in_group;
    col = f->column;
  }
  for (i = 0; i < snap->nitems && !*out; i++) {
    const SItem * it = &snap->items[i];
    if (it->group != group || col >= it->nvals || it->vals[col].type == 0) {
      continue;
    }
    for (k = 0; k < snap->nblinds; k++) {
      const SBlind * b = &snap->blinds[k];
      if (b->item == it->item && b->from == it->from && b->field == f->id &&
          memcmp(b->mac, mac, GDEFSTOR_BLIND_MAC_BYTES) == 0) {
        *out = 1;
        break;
      }
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result eval_exists(Engine * e, const GDEFSTOR_QueryNode * n,
    const Snap * snap, int * out) {
  size_t k = e->exists_at;
  const Inner * in = &e->inner[k];
  const GDEFSTOR_Type * t = e->plan->exists_types[k];
  size_t i;
  *out = 0;
  e->exists_at += 1;
  for (i = 0; i < in->n; i++) {
    int holds = 0;
    GDEFSTOR_Result rc;
    rc = eval(e, n->children[0], t, &in->v[i], snap, &holds);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    if (holds) {
      *out = 1;
      return GDEFSTOR_OK;
    }
  }
  return GDEFSTOR_OK;
}

/* Count the EXISTS nodes under @p n, to keep exists_at depth first when a
 * branch is not evaluated. */
static size_t exists_below(const GDEFSTOR_QueryNode * n) {
  size_t i;
  size_t c = n->op == GDEFSTOR_QUERY_EXISTS ? 1 : 0;
  for (i = 0; i < n->child_count; i++) {
    c += exists_below(n->children[i]);
  }
  return c;
}

static GDEFSTOR_Result eval(Engine * e, const GDEFSTOR_QueryNode * n,
    const GDEFSTOR_Type * here, const Snap * snap, const Snap * outer,
    int * out) {
  size_t i;
  GDEFSTOR_Result rc;
  switch (n->op) {
  case GDEFSTOR_QUERY_TRUE:
    *out = 1;
    return GDEFSTOR_OK;
  case GDEFSTOR_QUERY_FALSE:
    *out = 0;
    return GDEFSTOR_OK;
  case GDEFSTOR_QUERY_AND:
    for (i = 0; i < n->child_count; i++) {
      int r = 0;
      rc = eval(e, n->children[i], here, snap, outer, &r);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      if (!r) {
        size_t j;
        for (j = i + 1; j < n->child_count; j++) {
          e->exists_at += exists_below(n->children[j]);
        }
        *out = 0;
        return GDEFSTOR_OK;
      }
    }
    *out = 1;
    return GDEFSTOR_OK;
  case GDEFSTOR_QUERY_OR:
    for (i = 0; i < n->child_count; i++) {
      int r = 0;
      rc = eval(e, n->children[i], here, snap, outer, &r);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      if (r) {
        size_t j;
        for (j = i + 1; j < n->child_count; j++) {
          e->exists_at += exists_below(n->children[j]);
        }
        *out = 1;
        return GDEFSTOR_OK;
      }
    }
    *out = 0;
    return GDEFSTOR_OK;
  case GDEFSTOR_QUERY_NOT: {
    int r = 0;
    rc = eval(e, n->children[0], here, snap, outer, &r);
    *out = !r;
    return rc;
  }
  case GDEFSTOR_QUERY_EXISTS:
    return eval_exists(e, n, snap, out);
  case GDEFSTOR_QUERY_BLIND:
    return eval_blind(n, here, snap, out);
  default: {
    Set l;
    Set r;
    size_t a;
    size_t b;
    int found = 0;
    rc = operand_set(e, &n->left, here, snap, outer, &l);
    if (rc == GDEFSTOR_OK) {
      rc = operand_set(e, &n->right, here, snap, outer, &r);
      if (rc != GDEFSTOR_OK) {
        set_release(e, &l);
        return rc;
      }
    } else {
      set_release(e, &l);
      return rc;
    }
    /* Some pair stands in the relation; != is "no pair is equal". */
    for (a = 0; a < l.n && !found; a++) {
      for (b = 0; b < r.n && !found; b++) {
        int c = gdefstor_model_value_compare(l.v[a], r.v[b]);
        if (relation(n->op == GDEFSTOR_QUERY_NE ? GDEFSTOR_QUERY_EQ : n->op,
                c)) {
          found = 1;
        }
      }
    }
    set_release(e, &l);
    set_release(e, &r);
    *out = n->op == GDEFSTOR_QUERY_NE ? !found : found;
    return GDEFSTOR_OK;
  }
  }
}

static int hit_cmp(const void * ctx, size_t a, size_t b) {
  const Engine * e = (const Engine *)ctx;
  const GDEFSTOR_Query * q = e->plan->query;
  const Hit * x = &e->hits[a];
  const Hit * y = &e->hits[b];
  size_t k;
  for (k = 0; k < q->sort_count; k++) {
    int c = gdefstor_model_value_compare(&x->keys[k], &y->keys[k]);
    if (c != 0) {
      return q->sorts[k].descending ? -c : c;
    }
  }
  /* The internal row, ascending, is the final tiebreak. */
  return x->row < y->row ? -1 : (x->row > y->row ? 1 : 0);
}

static GDEFSTOR_Result keep_hit(Engine * e, const Snap * s) {
  const GDEFSTOR_Query * q = e->plan->query;
  Hit * h;
  size_t k;
  if (e->nhits == e->caphits) {
    size_t grown = e->caphits == 0 ? 64 : e->caphits * 2;
    Hit * next = (Hit *)gcu_allocator_realloc(e->alloc, e->hits,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    e->hits = next;
    e->caphits = grown;
  }
  h = &e->hits[e->nhits];
  memset(h, 0, sizeof *h);
  h->row = s->row;
  h->pid = gdefstor_dup(e->alloc, s->pid, s->pid_len);
  if (h->pid == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  h->pid_len = s->pid_len;
  e->nhits += 1;
  for (k = 0; k < q->sort_count; k++) {
    Set v;
    GDEFSTOR_Result rc =
        operand_set(e, &q->sorts[k].key, e->plan->type, s, NULL, &v);
    if (rc == GDEFSTOR_OK && v.n > 0) {
      rc = gdefstor_model_value_copy(e->alloc, &h->keys[k], v.v[0]);
    }
    set_release(e, &v);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result sort_hits(Engine * e, size_t ** out) {
  return gdefstor_model_sort(e->alloc, e->nhits, hit_cmp, e, out);
}

/* One entity of the main pass is complete: decide it. */
static GDEFSTOR_Result decide(Engine * e, const Snap * s) {
  int holds = 1;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  e->exists_at = 0;
  if (e->plan->query->where != NULL) {
    rc = eval(e, e->plan->query->where, e->plan->type, s, NULL, &holds);
  }
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (holds) {
    e->total += 1;
    if (!e->plan->count_only) {
      rc = keep_hit(e, s);
    }
  }
  return rc;
}

static int in_language(const Engine * e, const char * lang, size_t len) {
  const GDEFSTOR_Query * q = e->plan->query;
  return len == q->language_len && memcmp(lang, q->language, len) == 0;
}

/* Keep only the entity's tree in the query's language: a query reads one
 * language, and an entity with no tree in it reads as one whose every field
 * is absent, never as another language's. */
static void keep_language(Engine * e, Snap * s) {
  size_t i;
  size_t k = 0;
  for (i = 0; i < s->nitems; i++) {
    SItem * it = &s->items[i];
    if (!in_language(e, it->lang, it->lang_len)) {
      size_t j;
      for (j = 0; j < it->nvals; j++) {
        gdefstor_model_value_clear(e->alloc, &it->vals[j]);
      }
      gcu_allocator_free(e->alloc, it->vals);
      continue;
    }
    s->items[k++] = *it;
  }
  s->nitems = k;
  k = 0;
  for (i = 0; i < s->nblinds; i++) {
    if (in_language(e, s->blinds[i].lang, s->blinds[i].lang_len)) {
      s->blinds[k++] = s->blinds[i];
    }
  }
  s->nblinds = k;
}

static GDEFSTOR_Result flush(Engine * e) {
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (!e->have_cur) {
    return GDEFSTOR_OK;
  }
  e->have_cur = 0;
  keep_language(e, &e->cur);
  if (e->into != NULL) {
    Inner * in = e->into;
    if (in->n == in->cap) {
      size_t grown = in->cap == 0 ? 16 : in->cap * 2;
      Snap * next = (Snap *)gcu_allocator_realloc(e->alloc, in->v,
          grown * sizeof *next);
      if (next == NULL) {
        snap_release(e->alloc, &e->cur);
        return GDEFSTOR_ERR_OOM;
      }
      in->v = next;
      in->cap = grown;
    }
    in->v[in->n] = e->cur;
    in->n += 1;
    memset(&e->cur, 0, sizeof e->cur);
    return GDEFSTOR_OK;
  }
  rc = decide(e, &e->cur);
  snap_release(e->alloc, &e->cur);
  return rc;
}

static GDEFSTOR_Result on_entity(void * ctx, const GDEFSTOR_ModelEntity * x) {
  Engine * e = (Engine *)ctx;
  GDEFSTOR_Result rc = flush(e);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&e->cur, 0, sizeof e->cur);
  e->cur.row = x->row;
  e->cur.pid = gdefstor_dup(e->alloc, x->public_id, x->public_id_len);
  e->cur.creator = gdefstor_dup(e->alloc, x->creator, x->creator_len);
  if (e->cur.pid == NULL || e->cur.creator == NULL) {
    snap_release(e->alloc, &e->cur);
    return GDEFSTOR_ERR_OOM;
  }
  e->cur.pid_len = x->public_id_len;
  e->cur.creator_len = x->creator_len;
  e->cur.created_ms = x->created_ms;
  e->cur.revision = x->revision;
  e->cur.published = x->published != 0;
  e->cur.deleted = x->deleted != 0;
  e->have_cur = 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result on_item(void * ctx, uint64_t row,
    const GDEFSTOR_ModelItem * i) {
  Engine * e = (Engine *)ctx;
  Snap * s = &e->cur;
  SItem * it;
  size_t k;
  if (!e->have_cur || row != s->row) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  if (s->nitems == s->capitems) {
    size_t grown = s->capitems == 0 ? 8 : s->capitems * 2;
    SItem * next = (SItem *)gcu_allocator_realloc(e->alloc, s->items,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    s->items = next;
    s->capitems = grown;
  }
  it = &s->items[s->nitems];
  memset(it, 0, sizeof *it);
  it->group = i->group;
  it->item = i->item;
  it->from = i->valid_from;
  if (!gdefstor_model_lang_ok(i->language, i->language_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  memcpy(it->lang, i->language, i->language_len);
  it->lang_len = i->language_len;
  if (i->value_count > 0) {
    it->vals = (GDEFSTOR_Value *)gdefstor_mem0(e->alloc,
        i->value_count * sizeof *it->vals);
    if (it->vals == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
  }
  s->nitems += 1;
  for (k = 0; k < i->value_count; k++) {
    GDEFSTOR_Result rc =
        gdefstor_model_value_copy(e->alloc, &it->vals[k], &i->values[k]);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    it->nvals = k + 1;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result on_blind(void * ctx, uint64_t row,
    const GDEFSTOR_ModelBlind * b) {
  Engine * e = (Engine *)ctx;
  Snap * s = &e->cur;
  SBlind * o;
  if (!e->have_cur || row != s->row || b->mac == NULL) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  if (!gdefstor_model_lang_ok(b->language, b->language_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (s->nblinds == s->capblinds) {
    size_t grown = s->capblinds == 0 ? 4 : s->capblinds * 2;
    SBlind * next = (SBlind *)gcu_allocator_realloc(e->alloc, s->blinds,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    s->blinds = next;
    s->capblinds = grown;
  }
  o = &s->blinds[s->nblinds];
  o->item = b->item;
  o->from = b->valid_from;
  o->field = b->field;
  memcpy(o->mac, b->mac, GDEFSTOR_BLIND_MAC_BYTES);
  memcpy(o->lang, b->language, b->language_len);
  o->lang_len = b->language_len;
  s->nblinds += 1;
  return GDEFSTOR_OK;
}

/* Whether a tree asks blind equality anywhere: only then are blind rows read. */
static int has_blind_node(const GDEFSTOR_QueryNode * n) {
  size_t i;
  if (n == NULL) {
    return 0;
  }
  if (n->op == GDEFSTOR_QUERY_BLIND) {
    return 1;
  }
  for (i = 0; i < n->child_count; i++) {
    if (has_blind_node(n->children[i])) {
      return 1;
    }
  }
  return 0;
}

static GDEFSTOR_Result scan_type(Engine * e, const GDEFSTOR_Type * t,
    Inner * into) {
  GDEFSTOR_ModelVisitor v;
  GDEFSTOR_Result rc;
  memset(&v, 0, sizeof v);
  v.ctx = e;
  v.entity = on_entity;
  v.item = on_item;
  if (has_blind_node(e->plan->query->where)) {
    v.blind = on_blind;
  }
  e->into = into;
  e->have_cur = 0;
  rc = e->scan(e->scan_ctx, t->name, t->name_len, &v);
  if (rc == GDEFSTOR_OK) {
    rc = flush(e);
  } else if (e->have_cur) {
    snap_release(e->alloc, &e->cur);
    e->have_cur = 0;
  }
  e->into = NULL;
  return rc;
}

GDEFSTOR_Result gdefstor_model_scan(const GDEFSTOR_ModelPlan * plan,
    GDEFSTOR_ModelScanFn scan, void * scan_ctx, GDEFSTOR_ModelHits * out) {
  Engine e;
  size_t i;
  size_t * order = NULL;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (plan == NULL || plan->query == NULL || plan->type == NULL ||
      scan == NULL || out == NULL ||
      (plan->exists_count > 0 && plan->exists_types == NULL)) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&e, 0, sizeof e);
  e.plan = plan;
  e.alloc = gdefstor_alloc(plan->alloc);
  e.scan = scan;
  e.scan_ctx = scan_ctx;
  memset(out, 0, sizeof *out);
  if (plan->exists_count > 0) {
    e.inner = (Inner *)gdefstor_mem0(e.alloc,
        plan->exists_count * sizeof *e.inner);
    if (e.inner == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
  }
  /* Each type an EXISTS looks at is read once, whole. */
  for (i = 0; rc == GDEFSTOR_OK && i < plan->exists_count; i++) {
    rc = scan_type(&e, plan->exists_types[i], &e.inner[i]);
  }
  if (rc == GDEFSTOR_OK) {
    rc = scan_type(&e, plan->type, NULL);
  }
  if (rc == GDEFSTOR_OK && !plan->count_only) {
    rc = sort_hits(&e, &order);
  }
  if (rc == GDEFSTOR_OK) {
    uint64_t offset = plan->query->offset;
    size_t first = offset < e.nhits ? (size_t)offset : e.nhits;
    size_t count = e.nhits - first;
    if ((uint64_t)count > plan->limit) {
      count = (size_t)plan->limit;
    }
    out->total = e.total;
    if (!plan->count_only && count > 0) {
      out->rows = (uint64_t *)gdefstor_mem0(e.alloc, count * sizeof *out->rows);
      out->public_ids = (char **)gdefstor_mem0(e.alloc,
          count * sizeof *out->public_ids);
      out->public_id_lens = (size_t *)gdefstor_mem0(e.alloc,
          count * sizeof *out->public_id_lens);
      if (out->rows == NULL || out->public_ids == NULL ||
          out->public_id_lens == NULL) {
        rc = GDEFSTOR_ERR_OOM;
      }
      for (i = 0; rc == GDEFSTOR_OK && i < count; i++) {
        Hit * h = &e.hits[order[first + i]];
        out->rows[i] = h->row;
        out->public_ids[i] = h->pid;
        out->public_id_lens[i] = h->pid_len;
        h->pid = NULL;
        out->count = i + 1;
      }
    }
  }
  if (rc != GDEFSTOR_OK) {
    gdefstor_model_hits_release(e.alloc, out);
  }
  gcu_allocator_free(e.alloc, order);
  for (i = 0; i < e.nhits; i++) {
    size_t k;
    gcu_allocator_free(e.alloc, e.hits[i].pid);
    for (k = 0; k < GDEFSTOR_QUERY_MAX_SORTS; k++) {
      gdefstor_model_value_clear(e.alloc, &e.hits[i].keys[k]);
    }
  }
  gcu_allocator_free(e.alloc, e.hits);
  for (i = 0; i < plan->exists_count && e.inner != NULL; i++) {
    size_t k;
    for (k = 0; k < e.inner[i].n; k++) {
      snap_release(e.alloc, &e.inner[i].v[k]);
    }
    gcu_allocator_free(e.alloc, e.inner[i].v);
  }
  gcu_allocator_free(e.alloc, e.inner);
  return rc;
}
