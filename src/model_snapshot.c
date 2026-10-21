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
 * The entity part of `defiant-snapshot/1` (documentation/formats.md): a
 * logical export of the truth the entity port holds, read through the
 * backend's `dump`, and its import, written through the backend's restore
 * members. What is derived (current tables, the blind table, indexes) is
 * never read here and never written: an import's revisions rebuild the
 * current tables as they are applied, and blind rows are core's to rebuild
 * from decryptable values (AD-23).
 *
 * Every array is sorted before it is spelled, so the same truth gives the
 * same bytes whatever order a backend visits its rows in.
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/defiant-storage/model_backend.h>
#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The format of a per-entity member (AD-34), and its name with the slash. */
#define REVISION_PREFIX GDEFSTOR_REVISION_NAME "/"
#define REVISION_FORMAT GDEFSTOR_REVISION_NAME "/1"

/* ---- the truth, held in memory -------------------------------------------- */

typedef struct SVer {
  uint64_t row;
  char * lang;
  size_t lang_len;
  uint32_t group;
  uint64_t item;
  uint64_t parent;
  double order;
  uint64_t from;
  uint64_t to;
  GDEFSTOR_Value * vals; /* by column; type 0 is no value */
  size_t nvals;
} SVer;

typedef struct SRev {
  uint64_t row;
  uint64_t revision;
  uint64_t commit;
  char * who;
  size_t who_len;
  int64_t when;
  char * why;
  size_t why_len;
  char * langs;
  size_t langs_len;
  char * schema;
  size_t schema_len;
} SRev;

typedef struct SState {
  uint64_t row;
  uint64_t seq;
  int published;
  int deleted;
  char * actor;
  size_t actor_len;
  int64_t when;
} SState;

typedef struct SEnt {
  uint64_t row;   /* the source's row; on import, the new one */
  uint64_t order; /* its place among the tenant's entities, 1, 2, 3 ... */
  char * type;
  size_t type_len;
  char * pid;
  size_t pid_len;
  char * creator;
  size_t creator_len;
  int64_t created;
  uint64_t revision;
  uint64_t last_item;
  size_t type_at; /* index of its type */
  /* its rows: ranges of the sorted arrays below */
  size_t state0, nstate;
  size_t rev0, nrev;
  size_t ver0, nver;
  int seen_member; /* on import: its entity member was read */
} SEnt;

typedef struct SType {
  char * name;
  size_t name_len;
  uint8_t * text;
  size_t text_len;
  GDEFSTOR_Type * decl;
  int seen_entities; /* on import: its entities member was read */
  size_t ent0;       /* on import: its entities, by public id, in ents */
  size_t nent;
} SType;

typedef struct SDraft {
  char * pid;
  size_t pid_len;
  char * type;
  size_t type_len;
  uint64_t base;
  uint8_t * json;
  size_t json_len;
  char * schema;
  size_t schema_len;
} SDraft;

typedef struct SRec {
  char * ns;
  size_t ns_len;
  char * key;
  size_t key_len;
  uint8_t * bytes;
  size_t len;
  char * schema;
  size_t schema_len;
} SRec;

typedef struct SCnt {
  int kind;
  char * name;
  size_t name_len;
  uint64_t value;
} SCnt;

typedef struct SOb {
  char * stream;
  size_t stream_len;
  uint64_t seq;
  char * pid;
  size_t pid_len;
  char * fmt;
  size_t fmt_len;
  uint8_t * payload;
  size_t payload_len;
  int64_t lease;
  uint32_t failures;
  int parked;
} SOb;

#define ARR(T, name) \
  T * name;          \
  size_t n##name;    \
  size_t cap##name

struct GDEFSTOR_SnapModel {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_Result rc; /* the first failure of a visit */
  ARR(SType, types);
  ARR(SEnt, ents);
  ARR(SState, states);
  ARR(SRev, revs);
  ARR(SVer, vers);
  ARR(SDraft, drafts);
  ARR(SRec, recs);
  ARR(SCnt, cnts);
  ARR(SOb, obs);
  int seen_drafts;
  int seen_records;
  int seen_counters;
  int seen_outbox;
};

static int same(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

static int bytes_cmp(const char * a, size_t an, const char * b, size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = n == 0 ? 0 : memcmp(a, b, n);
  if (c != 0) {
    return c;
  }
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

static int u64_cmp(uint64_t a, uint64_t b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* Room for one more element of an array of @p size-byte elements, whose
 * pointer is at @p pv (copied in and out, so no pointer is read through
 * another pointer type). */
static GDEFSTOR_Result grow(const GDEFSTOR_Allocator * alloc, void * pv,
    size_t n, size_t * cap, size_t size) {
  void * v;
  void * next;
  size_t c;
  if (n < *cap) {
    return GDEFSTOR_OK;
  }
  c = *cap == 0 ? 16 : *cap * 2;
  if (c < *cap || c > SIZE_MAX / size) {
    return GDEFSTOR_ERR_LIMIT;
  }
  memcpy(&v, pv, sizeof v);
  next = gcu_allocator_realloc(alloc, v, c * size);
  if (next == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  memset((char *)next + *cap * size, 0, (c - *cap) * size);
  memcpy(pv, &next, sizeof next);
  *cap = c;
  return GDEFSTOR_OK;
}

#define GROW(m, name)                                                        \
  grow((m)->alloc, &(m)->name, (m)->n##name, &(m)->cap##name,               \
      sizeof *(m)->name)

/* A copy of @p n bytes (NUL-terminated), or NULL. */
static char * copy(const GDEFSTOR_Allocator * alloc, const void * p,
    size_t n) {
  return gdefstor_dup(alloc, n == 0 ? "" : (const char *)p, n);
}

static void vals_free(const GDEFSTOR_Allocator * a, GDEFSTOR_Value * v,
    size_t n) {
  size_t i;
  for (i = 0; v != NULL && i < n; i++) {
    gdefstor_model_value_clear(a, &v[i]);
  }
  gcu_allocator_free(a, v);
}

void gdefstor_snap_model_free(GDEFSTOR_SnapModel * m) {
  const GDEFSTOR_Allocator * a;
  size_t i;
  if (m == NULL) {
    return;
  }
  a = m->alloc;
  for (i = 0; i < m->ntypes; i++) {
    gcu_allocator_free(a, m->types[i].name);
    gcu_allocator_free(a, m->types[i].text);
    gdefstor_type_free(m->types[i].decl);
  }
  for (i = 0; i < m->nents; i++) {
    gcu_allocator_free(a, m->ents[i].type);
    gcu_allocator_free(a, m->ents[i].pid);
    gcu_allocator_free(a, m->ents[i].creator);
  }
  for (i = 0; i < m->nstates; i++) {
    gcu_allocator_free(a, m->states[i].actor);
  }
  for (i = 0; i < m->nrevs; i++) {
    gcu_allocator_free(a, m->revs[i].who);
    gcu_allocator_free(a, m->revs[i].why);
    gcu_allocator_free(a, m->revs[i].langs);
    gcu_allocator_free(a, m->revs[i].schema);
  }
  for (i = 0; i < m->nvers; i++) {
    gcu_allocator_free(a, m->vers[i].lang);
    vals_free(a, m->vers[i].vals, m->vers[i].nvals);
  }
  for (i = 0; i < m->ndrafts; i++) {
    gcu_allocator_free(a, m->drafts[i].pid);
    gcu_allocator_free(a, m->drafts[i].type);
    gcu_allocator_free(a, m->drafts[i].json);
    gcu_allocator_free(a, m->drafts[i].schema);
  }
  for (i = 0; i < m->nrecs; i++) {
    gcu_allocator_free(a, m->recs[i].ns);
    gcu_allocator_free(a, m->recs[i].key);
    gcu_allocator_free(a, m->recs[i].bytes);
    gcu_allocator_free(a, m->recs[i].schema);
  }
  for (i = 0; i < m->ncnts; i++) {
    gcu_allocator_free(a, m->cnts[i].name);
  }
  for (i = 0; i < m->nobs; i++) {
    gcu_allocator_free(a, m->obs[i].stream);
    gcu_allocator_free(a, m->obs[i].pid);
    gcu_allocator_free(a, m->obs[i].fmt);
    gcu_allocator_free(a, m->obs[i].payload);
  }
  gcu_allocator_free(a, m->types);
  gcu_allocator_free(a, m->ents);
  gcu_allocator_free(a, m->states);
  gcu_allocator_free(a, m->revs);
  gcu_allocator_free(a, m->vers);
  gcu_allocator_free(a, m->drafts);
  gcu_allocator_free(a, m->recs);
  gcu_allocator_free(a, m->cnts);
  gcu_allocator_free(a, m->obs);
  gcu_allocator_free(a, m);
}

static GDEFSTOR_SnapModel * model_new(const GDEFSTOR_Allocator * alloc) {
  GDEFSTOR_SnapModel * m =
      (GDEFSTOR_SnapModel *)gdefstor_mem0(alloc, sizeof *m);
  if (m != NULL) {
    m->alloc = alloc;
  }
  return m;
}

int gdefstor_snap_model_none(const GDEFSTOR_SnapModel * m) {
  return m == NULL || (m->ntypes == 0 && m->nents == 0 && m->ndrafts == 0 &&
                          m->nrecs == 0 && m->ncnts == 0 && m->nobs == 0);
}

/* ---- sorting -------------------------------------------------------------- */

static int type_qcmp(const void * a, const void * b) {
  const SType * x = (const SType *)a;
  const SType * y = (const SType *)b;
  return bytes_cmp(x->name, x->name_len, y->name, y->name_len);
}

static int ent_row_qcmp(const void * a, const void * b) {
  const SEnt * x = (const SEnt *)a;
  const SEnt * y = (const SEnt *)b;
  return u64_cmp(x->row, y->row);
}

static int state_qcmp(const void * a, const void * b) {
  const SState * x = (const SState *)a;
  const SState * y = (const SState *)b;
  int c = u64_cmp(x->row, y->row);
  return c != 0 ? c : u64_cmp(x->seq, y->seq);
}

static int rev_qcmp(const void * a, const void * b) {
  const SRev * x = (const SRev *)a;
  const SRev * y = (const SRev *)b;
  int c = u64_cmp(x->row, y->row);
  return c != 0 ? c : u64_cmp(x->revision, y->revision);
}

static int ver_qcmp(const void * a, const void * b) {
  const SVer * x = (const SVer *)a;
  const SVer * y = (const SVer *)b;
  int c = u64_cmp(x->row, y->row);
  if (c == 0) {
    c = u64_cmp(x->group, y->group);
  }
  if (c == 0) {
    c = u64_cmp(x->item, y->item);
  }
  return c != 0 ? c : u64_cmp(x->from, y->from);
}

static int draft_qcmp(const void * a, const void * b) {
  const SDraft * x = (const SDraft *)a;
  const SDraft * y = (const SDraft *)b;
  return bytes_cmp(x->pid, x->pid_len, y->pid, y->pid_len);
}

static int rec_qcmp(const void * a, const void * b) {
  const SRec * x = (const SRec *)a;
  const SRec * y = (const SRec *)b;
  int c = bytes_cmp(x->ns, x->ns_len, y->ns, y->ns_len);
  return c != 0 ? c : bytes_cmp(x->key, x->key_len, y->key, y->key_len);
}

static int cnt_qcmp(const void * a, const void * b) {
  const SCnt * x = (const SCnt *)a;
  const SCnt * y = (const SCnt *)b;
  int c = x->kind < y->kind ? -1 : (x->kind > y->kind ? 1 : 0);
  return c != 0 ? c : bytes_cmp(x->name, x->name_len, y->name, y->name_len);
}

static int ob_qcmp(const void * a, const void * b) {
  const SOb * x = (const SOb *)a;
  const SOb * y = (const SOb *)b;
  int c = bytes_cmp(x->stream, x->stream_len, y->stream, y->stream_len);
  return c != 0 ? c : u64_cmp(x->seq, y->seq);
}

#define SORT(m, name, cmp)                                                   \
  do {                                                                       \
    if ((m)->n##name > 1) {                                                  \
      qsort((m)->name, (m)->n##name, sizeof *(m)->name, cmp);                \
    }                                                                        \
  } while (0)

/* The entities, by row; and each one's states, revisions and versions as
 * ranges of those arrays, which are sorted by row first. ERR_CORRUPT for a
 * row that names no entity. */
static GDEFSTOR_Result link_rows(GDEFSTOR_SnapModel * m) {
  size_t i;
  size_t e;
  SORT(m, ents, ent_row_qcmp);
  SORT(m, states, state_qcmp);
  SORT(m, revs, rev_qcmp);
  SORT(m, vers, ver_qcmp);
  for (i = 0; i + 1 < m->nents; i++) {
    if (m->ents[i].row == m->ents[i + 1].row) {
      return GDEFSTOR_ERR_CORRUPT;
    }
  }
#define LINK(arr, first, count)                                              \
  e = 0;                                                                     \
  for (i = 0; i < m->n##arr; i++) {                                          \
    while (e < m->nents && m->ents[e].row < m->arr[i].row) {                 \
      e += 1;                                                                \
    }                                                                        \
    if (e == m->nents || m->ents[e].row != m->arr[i].row) {                  \
      return GDEFSTOR_ERR_CORRUPT;                                           \
    }                                                                        \
    if (m->ents[e].count == 0) {                                             \
      m->ents[e].first = i;                                                  \
    }                                                                        \
    m->ents[e].count += 1;                                                   \
  }
  LINK(states, state0, nstate)
  LINK(revs, rev0, nrev)
  LINK(vers, ver0, nver)
#undef LINK
  return GDEFSTOR_OK;
}

static size_t find_type(const GDEFSTOR_SnapModel * m, const char * name,
    size_t len) {
  size_t lo = 0;
  size_t hi = m->ntypes;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = bytes_cmp(m->types[mid].name, m->types[mid].name_len, name, len);
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < m->ntypes &&
      same(m->types[lo].name, m->types[lo].name_len, name, len)) {
    return lo;
  }
  return GDEFSTOR_NONE;
}

/* ---- export: catching what `dump` visits ---------------------------------- */

static GDEFSTOR_Result take_type(void * ctx, const char * name,
    size_t name_len, const uint8_t * text, size_t text_len) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)ctx;
  SType * t;
  GDEFSTOR_Result rc = GROW(m, types);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  t = &m->types[m->ntypes];
  t->name = copy(m->alloc, name, name_len);
  t->text = (uint8_t *)copy(m->alloc, text, text_len);
  if (t->name == NULL || t->text == NULL) {
    gcu_allocator_free(m->alloc, t->name);
    gcu_allocator_free(m->alloc, t->text);
    memset(t, 0, sizeof *t);
    return GDEFSTOR_ERR_OOM;
  }
  t->name_len = name_len;
  t->text_len = text_len;
  m->ntypes += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result take_entity(void * ctx, const GDEFSTOR_ModelEntity * e) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)ctx;
  SEnt * n;
  GDEFSTOR_Result rc = GROW(m, ents);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  n = &m->ents[m->nents];
  n->type = copy(m->alloc, e->type, e->type_len);
  n->pid = copy(m->alloc, e->public_id, e->public_id_len);
  n->creator = copy(m->alloc, e->creator, e->creator_len);
  if (n->type == NULL || n->pid == NULL || n->creator == NULL) {
    gcu_allocator_free(m->alloc, n->type);
    gcu_allocator_free(m->alloc, n->pid);
    gcu_allocator_free(m->alloc, n->creator);
    memset(n, 0, sizeof *n);
    return GDEFSTOR_ERR_OOM;
  }
  n->row = e->row;
  n->type_len = e->type_len;
  n->pid_len = e->public_id_len;
  n->creator_len = e->creator_len;
  n->created = e->created_ms;
  n->revision = e->revision;
  n->last_item = e->last_item;
  m->nents += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result take_state(void * ctx, uint64_t row,
    const GDEFSTOR_ModelState * s) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)ctx;
  SState * n;
  GDEFSTOR_Result rc = GROW(m, states);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  n = &m->states[m->nstates];
  n->actor = copy(m->alloc, s->actor, s->actor_len);
  if (n->actor == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  n->row = row;
  n->seq = s->seq;
  n->published = s->published != 0;
  n->deleted = s->deleted != 0;
  n->actor_len = s->actor_len;
  n->when = s->when_ms;
  m->nstates += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result take_revision(void * ctx, uint64_t row,
    const GDEFSTOR_ModelRevision * r) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)ctx;
  SRev * n;
  GDEFSTOR_Result rc = GROW(m, revs);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  n = &m->revs[m->nrevs];
  n->who = copy(m->alloc, r->who, r->who_len);
  n->why = copy(m->alloc, r->why, r->why_len);
  n->langs = copy(m->alloc, r->languages, r->languages_len);
  n->schema = copy(m->alloc, r->schema, r->schema_len);
  if (n->who == NULL || n->why == NULL || n->langs == NULL ||
      n->schema == NULL) {
    gcu_allocator_free(m->alloc, n->who);
    gcu_allocator_free(m->alloc, n->why);
    gcu_allocator_free(m->alloc, n->langs);
    gcu_allocator_free(m->alloc, n->schema);
    memset(n, 0, sizeof *n);
    return GDEFSTOR_ERR_OOM;
  }
  n->schema_len = r->schema_len;
  n->row = row;
  n->revision = r->revision;
  n->commit = r->commit_seq;
  n->who_len = r->who_len;
  n->when = r->when_ms;
  n->why_len = r->why_len;
  n->langs_len = r->languages_len;
  m->nrevs += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result take_item(void * ctx, uint64_t row,
    const GDEFSTOR_ModelItem * i) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)ctx;
  SVer * n;
  size_t k;
  size_t count = i->value_count;
  GDEFSTOR_Result rc;
  if (i->values == NULL && count != 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  /* Trailing no-values are not stored differences (model.md). */
  while (count > 0 && i->values[count - 1].type == 0) {
    count -= 1;
  }
  rc = GROW(m, vers);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  n = &m->vers[m->nvers];
  n->lang = copy(m->alloc, i->language, i->language_len);
  n->vals = (GDEFSTOR_Value *)gdefstor_mem0(m->alloc,
      (count == 0 ? 1 : count) * sizeof *n->vals);
  if (n->lang == NULL || n->vals == NULL) {
    gcu_allocator_free(m->alloc, n->lang);
    gcu_allocator_free(m->alloc, n->vals);
    memset(n, 0, sizeof *n);
    return GDEFSTOR_ERR_OOM;
  }
  for (k = 0; k < count; k++) {
    rc = gdefstor_model_value_copy(m->alloc, &n->vals[k], &i->values[k]);
    if (rc != GDEFSTOR_OK) {
      vals_free(m->alloc, n->vals, k);
      gcu_allocator_free(m->alloc, n->lang);
      memset(n, 0, sizeof *n);
      return rc;
    }
  }
  n->row = row;
  n->lang_len = i->language_len;
  n->group = i->group;
  n->item = i->item;
  n->parent = i->parent;
  n->order = i->order;
  n->from = i->valid_from;
  n->to = i->valid_to;
  n->nvals = count;
  m->nvers += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result take_draft(void * ctx, const GDEFSTOR_ModelDraft * d) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)ctx;
  SDraft * n;
  GDEFSTOR_Result rc = GROW(m, drafts);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  n = &m->drafts[m->ndrafts];
  n->pid = copy(m->alloc, d->public_id, d->public_id_len);
  n->type = copy(m->alloc, d->type, d->type_len);
  n->json = (uint8_t *)copy(m->alloc, d->json, d->json_len);
  n->schema = copy(m->alloc, d->schema, d->schema_len);
  if (n->pid == NULL || n->type == NULL || n->json == NULL ||
      n->schema == NULL) {
    gcu_allocator_free(m->alloc, n->pid);
    gcu_allocator_free(m->alloc, n->type);
    gcu_allocator_free(m->alloc, n->json);
    gcu_allocator_free(m->alloc, n->schema);
    memset(n, 0, sizeof *n);
    return GDEFSTOR_ERR_OOM;
  }
  n->schema_len = d->schema_len;
  n->pid_len = d->public_id_len;
  n->type_len = d->type_len;
  n->base = d->base_revision;
  n->json_len = d->json_len;
  m->ndrafts += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result take_record(void * ctx, const GDEFSTOR_ModelRecord * r) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)ctx;
  SRec * n;
  GDEFSTOR_Result rc;
  if (r == NULL || (r->bytes == NULL && r->len != 0)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = GROW(m, recs);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  n = &m->recs[m->nrecs];
  n->ns = copy(m->alloc, r->ns, r->ns_len);
  n->key = copy(m->alloc, r->key, r->key_len);
  n->bytes = (uint8_t *)copy(m->alloc, r->bytes, r->len);
  n->schema = copy(m->alloc, r->schema, r->schema_len);
  if (n->ns == NULL || n->key == NULL || n->bytes == NULL ||
      n->schema == NULL) {
    gcu_allocator_free(m->alloc, n->ns);
    gcu_allocator_free(m->alloc, n->key);
    gcu_allocator_free(m->alloc, n->bytes);
    gcu_allocator_free(m->alloc, n->schema);
    memset(n, 0, sizeof *n);
    return GDEFSTOR_ERR_OOM;
  }
  n->schema_len = r->schema_len;
  n->ns_len = r->ns_len;
  n->key_len = r->key_len;
  n->len = r->len;
  m->nrecs += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result take_counter(void * ctx, int kind, const char * name,
    size_t name_len, uint64_t value) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)ctx;
  SCnt * n;
  GDEFSTOR_Result rc;
  if (kind != GDEFSTOR_MODEL_COUNTER && kind != GDEFSTOR_MODEL_STREAM) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  /* A counter never advanced reads 0 and is not a row (model.c's dump). */
  if (value == 0) {
    return GDEFSTOR_OK;
  }
  rc = GROW(m, cnts);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  n = &m->cnts[m->ncnts];
  n->name = copy(m->alloc, name, name_len);
  if (n->name == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  n->kind = kind;
  n->name_len = name_len;
  n->value = value;
  m->ncnts += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result take_outbox(void * ctx, const GDEFSTOR_ModelOutbox * o) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)ctx;
  SOb * n;
  GDEFSTOR_Result rc;
  if (o == NULL || (o->payload == NULL && o->payload_len != 0)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = GROW(m, obs);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  n = &m->obs[m->nobs];
  n->stream = copy(m->alloc, o->stream, o->stream_len);
  n->pid = copy(m->alloc, o->public_id, o->public_id_len);
  n->fmt = copy(m->alloc, o->format, o->format_len);
  n->payload = (uint8_t *)copy(m->alloc, o->payload, o->payload_len);
  if (n->stream == NULL || n->pid == NULL || n->fmt == NULL ||
      n->payload == NULL) {
    gcu_allocator_free(m->alloc, n->stream);
    gcu_allocator_free(m->alloc, n->pid);
    gcu_allocator_free(m->alloc, n->fmt);
    gcu_allocator_free(m->alloc, n->payload);
    memset(n, 0, sizeof *n);
    return GDEFSTOR_ERR_OOM;
  }
  n->stream_len = o->stream_len;
  n->seq = o->seq;
  n->pid_len = o->public_id_len;
  n->fmt_len = o->format_len;
  n->payload_len = o->payload_len;
  n->lease = o->lease_until_ms;
  n->failures = o->failures;
  n->parked = o->parked != 0;
  m->nobs += 1;
  return GDEFSTOR_OK;
}

/* ---- export: spelling the members ----------------------------------------- */

typedef struct J {
  const GDEFSTOR_Allocator * alloc;
  char * buf;
  size_t len;
  size_t cap;
  GDEFSTOR_Result rc;
} J;

static void j_raw(J * j, const char * s, size_t n) {
  if (j->rc == GDEFSTOR_OK) {
    j->rc = gdefstor_buf_append(&j->buf, &j->len, &j->cap, j->alloc, s, n);
  }
}

static void j_lit(J * j, const char * s) {
  j_raw(j, s, strlen(s));
}

static void j_str(J * j, const char * s, size_t n) {
  if (j->rc == GDEFSTOR_OK) {
    j->rc = gdefstor_json_escape_append(&j->buf, &j->len, &j->cap, j->alloc,
        s, n);
  }
}

static void j_u64(J * j, uint64_t v) {
  char tmp[32];
  snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
  j_lit(j, tmp);
}

static void j_i64(J * j, int64_t v) {
  char tmp[32];
  snprintf(tmp, sizeof tmp, "%lld", (long long)v);
  j_lit(j, tmp);
}

static void j_bool(J * j, int v) {
  j_lit(j, v ? "true" : "false");
}

static const char kHex[] = "0123456789abcdef";

static void j_hexstr(J * j, const void * p, size_t n) {
  const unsigned char * b = (const unsigned char *)p;
  size_t i;
  j_raw(j, "\"", 1);
  for (i = 0; i < n; i++) {
    char pair[2];
    pair[0] = kHex[b[i] >> 4];
    pair[1] = kHex[b[i] & 0xF];
    j_raw(j, pair, 2);
  }
  j_raw(j, "\"", 1);
}

/* A double's bits as 16 hex digits: exact, and no locale in it. */
static void j_bits(J * j, double d) {
  uint64_t bits;
  char tmp[24];
  memcpy(&bits, &d, sizeof bits);
  snprintf(tmp, sizeof tmp, "\"%016llx\"", (unsigned long long)bits);
  j_lit(j, tmp);
}

/* Opaque bytes: `"text":"..."` when they are UTF-8 without NUL, else
 * `"hex":"..."`, so a reader without the framework sees text as text. */
static void j_blob(J * j, const uint8_t * p, size_t n) {
  if (gdefstor_model_utf8_ok(p, n)) {
    j_lit(j, "\"text\":");
    j_str(j, (const char *)p, n);
  } else {
    j_lit(j, "\"hex\":");
    j_hexstr(j, p, n);
  }
}

/* "model/<hex of name>" plus @p tail. */
static GDEFSTOR_Result member_name(const GDEFSTOR_Allocator * alloc,
    const char * a, size_t a_len, const char * b, size_t b_len,
    const char * tail, char ** out, size_t * out_len) {
  J j;
  size_t i;
  memset(&j, 0, sizeof j);
  j.alloc = alloc;
  j_lit(&j, "model/");
  for (i = 0; i < a_len; i++) {
    char pair[2];
    pair[0] = kHex[(unsigned char)a[i] >> 4];
    pair[1] = kHex[(unsigned char)a[i] & 0xF];
    j_raw(&j, pair, 2);
  }
  j_lit(&j, "/");
  for (i = 0; i < b_len; i++) {
    char pair[2];
    pair[0] = kHex[(unsigned char)b[i] >> 4];
    pair[1] = kHex[(unsigned char)b[i] & 0xF];
    j_raw(&j, pair, 2);
  }
  j_lit(&j, tail);
  if (j.rc != GDEFSTOR_OK) {
    gcu_allocator_free(alloc, j.buf);
    return j.rc;
  }
  *out = j.buf;
  *out_len = j.len;
  return GDEFSTOR_OK;
}

typedef struct Out {
  const GDEFSTOR_Allocator * alloc;
  const GDEFSTOR_Limits * cap;
  GDEFSTOR_SnapMember * v;
  size_t n;
  size_t capn;
  size_t total;
} Out;

/* Add a member, taking @p name and @p bytes (freed on failure too). */
static GDEFSTOR_Result add_member(Out * o, char * name, size_t name_len,
    char * bytes, size_t len) {
  GDEFSTOR_Result rc;
  if (len > o->cap->max_member_bytes || o->total > SIZE_MAX - len) {
    rc = GDEFSTOR_ERR_LIMIT;
  } else {
    rc = grow(o->alloc, &o->v, o->n, &o->capn, sizeof *o->v);
  }
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(o->alloc, name);
    gcu_allocator_free(o->alloc, bytes);
    return rc;
  }
  if (bytes == NULL) {
    bytes = gdefstor_dup(o->alloc, "", 0);
    if (bytes == NULL) {
      gcu_allocator_free(o->alloc, name);
      return GDEFSTOR_ERR_OOM;
    }
  }
  o->v[o->n].name = name;
  o->v[o->n].name_len = name_len;
  o->v[o->n].bytes = (uint8_t *)bytes;
  o->v[o->n].len = len;
  o->n += 1;
  o->total += len;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result add_json(Out * o, const char * name, J * j) {
  char * n;
  if (j->rc != GDEFSTOR_OK) {
    gcu_allocator_free(o->alloc, j->buf);
    return j->rc;
  }
  n = gdefstor_dup(o->alloc, name, strlen(name));
  if (n == NULL) {
    gcu_allocator_free(o->alloc, j->buf);
    return GDEFSTOR_ERR_OOM;
  }
  return add_member(o, n, strlen(name), j->buf, j->len);
}

static void spell_item(J * j, const GDEFSTOR_Type * t, const SVer * v) {
  size_t c;
  int first = 1;
  j_lit(j, "{\"lang\":");
  j_str(j, v->lang, v->lang_len);
  j_lit(j, ",\"group\":");
  j_u64(j, v->group);
  j_lit(j, ",\"item\":");
  j_u64(j, v->item);
  j_lit(j, ",\"parent\":");
  j_u64(j, v->parent);
  j_lit(j, ",\"order\":");
  j_bits(j, v->order);
  j_lit(j, ",\"from\":");
  j_u64(j, v->from);
  j_lit(j, ",\"to\":");
  j_u64(j, v->to);
  j_lit(j, ",\"values\":{");
  if (j->rc == GDEFSTOR_OK && v->group >= t->next_group) {
    j->rc = GDEFSTOR_ERR_CORRUPT;
  }
  for (c = 0; j->rc == GDEFSTOR_OK && c < v->nvals; c++) {
    const GDEFSTOR_MField * f;
    if (v->vals[c].type == 0) {
      continue;
    }
    if (c >= t->group_ncols[v->group]) {
      j->rc = GDEFSTOR_ERR_CORRUPT;
      break;
    }
    f = &t->f[t->col_field[t->col_base[v->group] + c]];
    if (v->vals[c].type != f->type) {
      j->rc = GDEFSTOR_ERR_CORRUPT;
      break;
    }
    if (!first) {
      j_raw(j, ",", 1);
    }
    first = 0;
    j_str(j, f->name, f->name_len);
    j_raw(j, ":", 1);
    if (j->rc == GDEFSTOR_OK) {
      j->rc = gdefstor_model_value_json(j->alloc, f, &v->vals[c], &j->buf,
          &j->len, &j->cap);
    }
  }
  j_lit(j, "}}");
}

/* The entities of one type, in byte order of public id: their rows in @p m,
 * as indexes. */
static GDEFSTOR_Result ents_of_type(const GDEFSTOR_SnapModel * m, size_t ti,
    size_t ** out, size_t * out_n);

static int ent_pid_cmp(const void * ctx, size_t a, size_t b) {
  const GDEFSTOR_SnapModel * m = (const GDEFSTOR_SnapModel *)ctx;
  return bytes_cmp(m->ents[a].pid, m->ents[a].pid_len, m->ents[b].pid,
      m->ents[b].pid_len);
}

static GDEFSTOR_Result ents_of_type(const GDEFSTOR_SnapModel * m, size_t ti,
    size_t ** out, size_t * out_n) {
  size_t * all = NULL;
  size_t * mine;
  size_t i;
  size_t n = 0;
  GDEFSTOR_Result rc = gdefstor_model_sort(m->alloc, m->nents, ent_pid_cmp, m,
      &all);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  mine = all;
  for (i = 0; i < m->nents; i++) {
    if (m->ents[all[i]].type_at == ti) {
      mine[n++] = all[i];
    }
  }
  *out = mine;
  *out_n = n;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result spell_type(Out * o, const GDEFSTOR_SnapModel * m,
    size_t ti) {
  const SType * t = &m->types[ti];
  size_t * idx = NULL;
  size_t n = 0;
  size_t i;
  char * name = NULL;
  size_t name_len = 0;
  J j;
  GDEFSTOR_Result rc;
  /* The declaration, byte for byte as stored (defiant-type/1). */
  rc = member_name(o->alloc, t->name, t->name_len, NULL, 0, "type.json",
      &name, &name_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  {
    char * text = copy(o->alloc, t->text, t->text_len);
    if (text == NULL) {
      gcu_allocator_free(o->alloc, name);
      return GDEFSTOR_ERR_OOM;
    }
    rc = add_member(o, name, name_len, text, t->text_len);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  rc = ents_of_type(m, ti, &idx, &n);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  /* The entities: immutable facts, revision and last item, state history. */
  memset(&j, 0, sizeof j);
  j.alloc = o->alloc;
  j_lit(&j, "{\"type\":");
  j_str(&j, t->name, t->name_len);
  j_lit(&j, ",\"entities\":[");
  for (i = 0; i < n; i++) {
    const SEnt * e = &m->ents[idx[i]];
    size_t s;
    if (i > 0) {
      j_raw(&j, ",", 1);
    }
    j_lit(&j, "{\"id\":");
    j_str(&j, e->pid, e->pid_len);
    j_lit(&j, ",\"order\":");
    j_u64(&j, e->order);
    j_lit(&j, ",\"creator\":");
    j_str(&j, e->creator, e->creator_len);
    j_lit(&j, ",\"created_ms\":");
    j_i64(&j, e->created);
    j_lit(&j, ",\"revision\":");
    j_u64(&j, e->revision);
    j_lit(&j, ",\"last_item\":");
    j_u64(&j, e->last_item);
    j_lit(&j, ",\"states\":[");
    for (s = 0; s < e->nstate; s++) {
      const SState * st = &m->states[e->state0 + s];
      if (j.rc == GDEFSTOR_OK && st->seq != s + 1) {
        j.rc = GDEFSTOR_ERR_CORRUPT;
      }
      if (s > 0) {
        j_raw(&j, ",", 1);
      }
      j_lit(&j, "{\"published\":");
      j_bool(&j, st->published);
      j_lit(&j, ",\"deleted\":");
      j_bool(&j, st->deleted);
      j_lit(&j, ",\"actor\":");
      j_str(&j, st->actor, st->actor_len);
      j_lit(&j, ",\"when_ms\":");
      j_i64(&j, st->when);
      j_lit(&j, "}");
    }
    j_lit(&j, "]}");
  }
  j_lit(&j, "]}");
  rc = member_name(o->alloc, t->name, t->name_len, NULL, 0, "entities.json",
      &name, &name_len);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(o->alloc, j.buf);
    gcu_allocator_free(o->alloc, idx);
    return rc;
  }
  if (j.rc != GDEFSTOR_OK) {
    gcu_allocator_free(o->alloc, name);
    gcu_allocator_free(o->alloc, j.buf);
    gcu_allocator_free(o->alloc, idx);
    return j.rc;
  }
  rc = add_member(o, name, name_len, j.buf, j.len);
  /* One member per entity: its revisions and every item version of every
   * language. */
  for (i = 0; rc == GDEFSTOR_OK && i < n; i++) {
    const SEnt * e = &m->ents[idx[i]];
    size_t k;
    memset(&j, 0, sizeof j);
    j.alloc = o->alloc;
    /* The member says its own format (AD-34): defiant-revision/1. */
    j_lit(&j, "{\"format\":\"" REVISION_FORMAT "\",\"type\":");
    j_str(&j, t->name, t->name_len);
    j_lit(&j, ",\"id\":");
    j_str(&j, e->pid, e->pid_len);
    j_lit(&j, ",\"revisions\":[");
    for (k = 0; k < e->nrev; k++) {
      const SRev * r = &m->revs[e->rev0 + k];
      if (k > 0) {
        j_raw(&j, ",", 1);
      }
      j_lit(&j, "{\"revision\":");
      j_u64(&j, r->revision);
      j_lit(&j, ",\"commit\":");
      j_u64(&j, r->commit);
      j_lit(&j, ",\"who\":");
      j_str(&j, r->who, r->who_len);
      j_lit(&j, ",\"when_ms\":");
      j_i64(&j, r->when);
      j_lit(&j, ",\"why\":");
      j_str(&j, r->why, r->why_len);
      j_lit(&j, ",\"languages\":");
      j_str(&j, r->langs, r->langs_len);
      /* The stamp it was written with, exactly (AD-15): which component's
       * data it is, and the schema version its trees are in. */
      j_lit(&j, ",\"schema\":");
      j_str(&j, r->schema, r->schema_len);
      j_lit(&j, "}");
    }
    j_lit(&j, "],\"items\":[");
    for (k = 0; k < e->nver; k++) {
      if (k > 0) {
        j_raw(&j, ",", 1);
      }
      spell_item(&j, t->decl, &m->vers[e->ver0 + k]);
    }
    j_lit(&j, "]}");
    rc = member_name(o->alloc, t->name, t->name_len, e->pid, e->pid_len,
        ".json", &name, &name_len);
    if (rc != GDEFSTOR_OK) {
      gcu_allocator_free(o->alloc, j.buf);
      break;
    }
    if (j.rc != GDEFSTOR_OK) {
      gcu_allocator_free(o->alloc, name);
      gcu_allocator_free(o->alloc, j.buf);
      rc = j.rc;
      break;
    }
    rc = add_member(o, name, name_len, j.buf, j.len);
  }
  gcu_allocator_free(o->alloc, idx);
  return rc;
}

static GDEFSTOR_Result spell_system(Out * o, const GDEFSTOR_SnapModel * m) {
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  size_t i;
  J j;
  if (m->ndrafts > 0) {
    memset(&j, 0, sizeof j);
    j.alloc = o->alloc;
    j_lit(&j, "{\"drafts\":[");
    for (i = 0; i < m->ndrafts; i++) {
      const SDraft * d = &m->drafts[i];
      if (i > 0) {
        j_raw(&j, ",", 1);
      }
      j_lit(&j, "{\"id\":");
      j_str(&j, d->pid, d->pid_len);
      j_lit(&j, ",\"type\":");
      j_str(&j, d->type, d->type_len);
      j_lit(&j, ",\"base\":");
      j_u64(&j, d->base);
      /* The stored defiant-draft/1 document, byte for byte. */
      j_lit(&j, ",\"document\":");
      j_str(&j, (const char *)d->json, d->json_len);
      j_lit(&j, ",\"schema\":");
      j_str(&j, d->schema, d->schema_len);
      j_lit(&j, "}");
    }
    j_lit(&j, "]}");
    rc = add_json(o, "model/drafts.json", &j);
  }
  if (rc == GDEFSTOR_OK && m->nrecs > 0) {
    memset(&j, 0, sizeof j);
    j.alloc = o->alloc;
    j_lit(&j, "{\"records\":[");
    for (i = 0; i < m->nrecs; i++) {
      const SRec * r = &m->recs[i];
      if (i > 0) {
        j_raw(&j, ",", 1);
      }
      j_lit(&j, "{\"namespace\":");
      j_str(&j, r->ns, r->ns_len);
      j_lit(&j, ",\"key\":");
      j_str(&j, r->key, r->key_len);
      j_raw(&j, ",", 1);
      j_blob(&j, r->bytes, r->len);
      j_lit(&j, ",\"schema\":");
      j_str(&j, r->schema, r->schema_len);
      j_lit(&j, "}");
    }
    j_lit(&j, "]}");
    rc = add_json(o, "model/records.json", &j);
  }
  if (rc == GDEFSTOR_OK && m->ncnts > 0) {
    memset(&j, 0, sizeof j);
    j.alloc = o->alloc;
    j_lit(&j, "{\"counters\":[");
    for (i = 0; i < m->ncnts; i++) {
      const SCnt * c = &m->cnts[i];
      if (i > 0) {
        j_raw(&j, ",", 1);
      }
      j_lit(&j, "{\"kind\":");
      j_lit(&j, c->kind == GDEFSTOR_MODEL_COUNTER ? "\"counter\""
                                                  : "\"stream\"");
      j_lit(&j, ",\"name\":");
      j_str(&j, c->name, c->name_len);
      j_lit(&j, ",\"value\":");
      j_u64(&j, c->value);
      j_lit(&j, "}");
    }
    j_lit(&j, "]}");
    rc = add_json(o, "model/counters.json", &j);
  }
  if (rc == GDEFSTOR_OK && m->nobs > 0) {
    memset(&j, 0, sizeof j);
    j.alloc = o->alloc;
    j_lit(&j, "{\"outbox\":[");
    for (i = 0; i < m->nobs; i++) {
      const SOb * b = &m->obs[i];
      if (i > 0) {
        j_raw(&j, ",", 1);
      }
      j_lit(&j, "{\"stream\":");
      j_str(&j, b->stream, b->stream_len);
      j_lit(&j, ",\"seq\":");
      j_u64(&j, b->seq);
      j_lit(&j, ",\"id\":");
      j_str(&j, b->pid, b->pid_len);
      j_lit(&j, ",\"format\":");
      j_str(&j, b->fmt, b->fmt_len);
      j_raw(&j, ",", 1);
      j_blob(&j, b->payload, b->payload_len);
      j_lit(&j, ",\"lease_until_ms\":");
      j_i64(&j, b->lease);
      j_lit(&j, ",\"failures\":");
      j_u64(&j, b->failures);
      j_lit(&j, ",\"parked\":");
      j_bool(&j, b->parked);
      j_lit(&j, "}");
    }
    j_lit(&j, "]}");
    rc = add_json(o, "model/outbox.json", &j);
  }
  return rc;
}

static void members_free(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_SnapMember * v, size_t n) {
  size_t i;
  for (i = 0; v != NULL && i < n; i++) {
    gcu_allocator_free(alloc, v[i].name);
    gcu_allocator_free(alloc, v[i].bytes);
  }
  gcu_allocator_free(alloc, v);
}

GDEFSTOR_Result gdefstor_snap_model_export(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * cap,
    GDEFSTOR_SnapMember ** out, size_t * out_count) {
  GDEFSTOR_SnapModel * m;
  GDEFSTOR_ModelVisitor v;
  Out o;
  size_t i;
  GDEFSTOR_Result rc;
  *out = NULL;
  *out_count = 0;
  m = model_new(store->alloc);
  if (m == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  memset(&v, 0, sizeof v);
  v.ctx = m;
  v.type = take_type;
  v.entity = take_entity;
  v.state = take_state;
  v.revision = take_revision;
  v.item = take_item;
  v.draft = take_draft;
  /* No `blind`: a backend visits blind rows only when it is set, so the
   * blind table is never read (AD-23: derived data stays out). */
  if (gdefstor_model_has_sys(store)) {
    v.record = take_record;
    v.counter = take_counter;
    v.outbox = take_outbox;
  }
  rc = store->model.dump(store->model.ctx, tenant, tenant_len,
      GDEFSTOR_MODEL_TRUTH, &v);
  if (rc == GDEFSTOR_OK) {
    rc = link_rows(m);
  }
  if (rc == GDEFSTOR_OK) {
    SORT(m, types, type_qcmp);
    SORT(m, drafts, draft_qcmp);
    SORT(m, recs, rec_qcmp);
    SORT(m, cnts, cnt_qcmp);
    SORT(m, obs, ob_qcmp);
    for (i = 0; rc == GDEFSTOR_OK && i < m->ntypes; i++) {
      if (i > 0 && same(m->types[i - 1].name, m->types[i - 1].name_len,
                       m->types[i].name, m->types[i].name_len)) {
        rc = GDEFSTOR_ERR_CORRUPT;
        break;
      }
      rc = gdefstor_model_type_parse(m->alloc, m->types[i].text,
          m->types[i].text_len, &m->types[i].decl);
    }
  }
  for (i = 0; rc == GDEFSTOR_OK && i < m->nents; i++) {
    /* Rows are internal (AD-34): what travels is each entity's place. */
    m->ents[i].order = i + 1;
    m->ents[i].type_at = find_type(m, m->ents[i].type, m->ents[i].type_len);
    if (m->ents[i].type_at == GDEFSTOR_NONE) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  memset(&o, 0, sizeof o);
  o.alloc = store->alloc;
  o.cap = cap;
  for (i = 0; rc == GDEFSTOR_OK && i < m->ntypes; i++) {
    rc = spell_type(&o, m, i);
  }
  if (rc == GDEFSTOR_OK) {
    rc = spell_system(&o, m);
  }
  gdefstor_snap_model_free(m);
  if (rc != GDEFSTOR_OK) {
    members_free(store->alloc, o.v, o.n);
    return rc;
  }
  *out = o.v;
  *out_count = o.n;
  return GDEFSTOR_OK;
}

/* ---- import: reading the members ------------------------------------------ */

static GDEFSTOR_Result from_json_status(GTEXT_JSON_Status code) {
  if (code == GTEXT_JSON_E_OOM) {
    return GDEFSTOR_ERR_OOM;
  }
  if (code == GTEXT_JSON_E_LIMIT || code == GTEXT_JSON_E_DEPTH) {
    return GDEFSTOR_ERR_LIMIT;
  }
  return GDEFSTOR_ERR_CORRUPT;
}

static GDEFSTOR_Result parse(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Limits * cap, const uint8_t * bytes, size_t len,
    GTEXT_JSON_Value ** out) {
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  memset(&err, 0, sizeof err);
  opt = gtext_json_parse_options_default();
  opt.max_depth = cap->max_json_depth == 0 ? 1 : cap->max_json_depth;
  opt.max_string_bytes = cap->max_member_bytes;
  opt.max_total_bytes = cap->max_member_bytes;
  opt.dupkeys = GTEXT_JSON_DUPKEY_ERROR;
  opt.allocator = (const GTEXT_Allocator *)alloc;
  opt.parse_int64 = 1;
  *out = gtext_json_parse((const char *)bytes, len, &opt, &err);
  if (*out == NULL) {
    rc = from_json_status(err.code);
  }
  gtext_json_error_free(&err);
  return rc;
}

/* An object with exactly @p n members. */
static int is_obj(const GTEXT_JSON_Value * v, size_t n) {
  return v != NULL && gtext_json_typeof(v) == GTEXT_JSON_OBJECT &&
         gtext_json_object_size(v) == n;
}

static const GTEXT_JSON_Value * get(const GTEXT_JSON_Value * o,
    const char * key) {
  return gtext_json_object_get(o, key, strlen(key));
}

static int get_str(const GTEXT_JSON_Value * o, const char * key,
    const char ** p, size_t * n) {
  return gtext_json_get_string(get(o, key), p, n) == GTEXT_JSON_OK;
}

/* A non-negative integer up to INT64_MAX. */
static int get_u64(const GTEXT_JSON_Value * o, const char * key,
    uint64_t * out) {
  int64_t v = 0;
  const GTEXT_JSON_Value * x = get(o, key);
  if (x == NULL || gtext_json_typeof(x) != GTEXT_JSON_NUMBER ||
      gtext_json_get_i64(x, &v) != GTEXT_JSON_OK || v < 0) {
    return 0;
  }
  *out = (uint64_t)v;
  return 1;
}

static int get_i64(const GTEXT_JSON_Value * o, const char * key,
    int64_t * out) {
  const GTEXT_JSON_Value * x = get(o, key);
  return x != NULL && gtext_json_typeof(x) == GTEXT_JSON_NUMBER &&
         gtext_json_get_i64(x, out) == GTEXT_JSON_OK;
}

static int get_bool(const GTEXT_JSON_Value * o, const char * key, int * out) {
  bool b = false;
  if (gtext_json_get_bool(get(o, key), &b) != GTEXT_JSON_OK) {
    return 0;
  }
  *out = b ? 1 : 0;
  return 1;
}

static const GTEXT_JSON_Value * get_arr(const GTEXT_JSON_Value * o,
    const char * key) {
  const GTEXT_JSON_Value * x = get(o, key);
  return x != NULL && gtext_json_typeof(x) == GTEXT_JSON_ARRAY ? x : NULL;
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

/* Lowercase hex of @p n bytes into @p out (n / 2 bytes, owned). */
static GDEFSTOR_Result unhex(const GDEFSTOR_Allocator * alloc, const char * s,
    size_t n, uint8_t ** out, size_t * out_len) {
  uint8_t * b;
  size_t i;
  if (n % 2 != 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  b = (uint8_t *)gdefstor_mem(alloc, n / 2 + 1);
  if (b == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < n / 2; i++) {
    int hi = hex_val(s[2 * i]);
    int lo = hex_val(s[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      gcu_allocator_free(alloc, b);
      return GDEFSTOR_ERR_CORRUPT;
    }
    b[i] = (uint8_t)(hi * 16 + lo);
  }
  b[n / 2] = 0;
  *out = b;
  *out_len = n / 2;
  return GDEFSTOR_OK;
}

/* A `"text"` or `"hex"` member (j_blob()), owned. */
static GDEFSTOR_Result get_blob(const GDEFSTOR_Allocator * alloc,
    const GTEXT_JSON_Value * o, uint8_t ** out, size_t * out_len) {
  const char * p = NULL;
  size_t n = 0;
  if (get_str(o, "text", &p, &n)) {
    /* Text is spelled as text exactly when it is UTF-8 (j_blob()). */
    if (!gdefstor_model_utf8_ok((const uint8_t *)p, n)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    *out = (uint8_t *)copy(alloc, p, n);
    *out_len = n;
    return *out == NULL ? GDEFSTOR_ERR_OOM : GDEFSTOR_OK;
  }
  if (get_str(o, "hex", &p, &n)) {
    GDEFSTOR_Result rc = unhex(alloc, p, n, out, out_len);
    if (rc == GDEFSTOR_OK &&
        gdefstor_model_utf8_ok(*out, *out_len)) {
      gcu_allocator_free(alloc, *out);
      *out = NULL;
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    return rc;
  }
  return GDEFSTOR_ERR_CORRUPT;
}

/* The name and, for an entity member, the public id a member name spells:
 * "model/<hex>/type.json", "model/<hex>/entities.json" or
 * "model/<hex>/<hex>.json". *out_kind 1, 2 or 3; 0 for none of those. */
static GDEFSTOR_Result split_name(const GDEFSTOR_Allocator * alloc,
    const char * name, size_t len, int * out_kind, char ** out_type,
    size_t * out_type_len, char ** out_pid, size_t * out_pid_len) {
  size_t slash;
  const char * tail;
  size_t tail_len;
  uint8_t * t = NULL;
  size_t tn = 0;
  GDEFSTOR_Result rc;
  *out_kind = 0;
  *out_type = NULL;
  *out_pid = NULL;
  if (len < 7 || memcmp(name, "model/", 6) != 0) {
    return GDEFSTOR_OK;
  }
  for (slash = 6; slash < len && name[slash] != '/'; slash++) {
  }
  if (slash == len || slash == 6) {
    return GDEFSTOR_OK;
  }
  tail = name + slash + 1;
  tail_len = len - slash - 1;
  rc = unhex(alloc, name + 6, slash - 6, &t, &tn);
  if (rc != GDEFSTOR_OK) {
    return rc == GDEFSTOR_ERR_OOM ? rc : GDEFSTOR_OK;
  }
  if (same(tail, tail_len, "type.json", 9)) {
    *out_kind = 1;
  } else if (same(tail, tail_len, "entities.json", 13)) {
    *out_kind = 2;
  } else if (tail_len > 5 && memcmp(tail + tail_len - 5, ".json", 5) == 0) {
    uint8_t * p = NULL;
    size_t pn = 0;
    rc = unhex(alloc, tail, tail_len - 5, &p, &pn);
    if (rc == GDEFSTOR_ERR_OOM) {
      gcu_allocator_free(alloc, t);
      return rc;
    }
    if (rc == GDEFSTOR_OK) {
      *out_kind = 3;
      *out_pid = (char *)p;
      *out_pid_len = pn;
    }
  }
  if (*out_kind == 0) {
    gcu_allocator_free(alloc, t);
    return GDEFSTOR_OK;
  }
  *out_type = (char *)t;
  *out_type_len = tn;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result read_type(GDEFSTOR_SnapModel * m, char * name,
    size_t name_len, const uint8_t * bytes, size_t len) {
  SType * t;
  GDEFSTOR_Type * decl = NULL;
  char * text = NULL;
  size_t text_len = 0;
  GDEFSTOR_Result rc = gdefstor_model_type_parse(m->alloc, bytes, len, &decl);
  if (rc == GDEFSTOR_ERR_OOM) {
    return rc;
  }
  if (rc != GDEFSTOR_OK) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  /* Stored as the port writes it, and named as its member says. */
  rc = gdefstor_model_type_text(decl, &text, &text_len);
  if (rc == GDEFSTOR_OK && (!same(text, text_len, (const char *)bytes, len) ||
                               !same(decl->name, decl->name_len, name,
                                   name_len))) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  gcu_allocator_free(m->alloc, text);
  if (rc == GDEFSTOR_OK) {
    rc = GROW(m, types);
  }
  if (rc != GDEFSTOR_OK) {
    gdefstor_type_free(decl);
    return rc;
  }
  t = &m->types[m->ntypes];
  t->text = (uint8_t *)copy(m->alloc, bytes, len);
  if (t->text == NULL) {
    gdefstor_type_free(decl);
    return GDEFSTOR_ERR_OOM;
  }
  t->name = gdefstor_dup(m->alloc, name, name_len);
  if (t->name == NULL) {
    gcu_allocator_free(m->alloc, t->text);
    t->text = NULL;
    gdefstor_type_free(decl);
    return GDEFSTOR_ERR_OOM;
  }
  t->name_len = name_len;
  t->text_len = len;
  t->decl = decl;
  m->ntypes += 1;
  return GDEFSTOR_OK;
}

/* Entities members are read after every type member (they are listed
 * later), so the type they name is found by name. */
static GDEFSTOR_Result read_entities(GDEFSTOR_SnapModel * m,
    const GDEFSTOR_Limits * cap, const char * type, size_t type_len,
    const GTEXT_JSON_Value * root) {
  const GTEXT_JSON_Value * arr = get_arr(root, "entities");
  SType * ty = &m->types[find_type(m, type, type_len)];
  const char * p;
  size_t n;
  size_t i;
  GDEFSTOR_Result rc;
  if (!is_obj(root, 2) || arr == NULL || !get_str(root, "type", &p, &n) ||
      !same(p, n, type, type_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  ty->ent0 = m->nents;
  for (i = 0; i < gtext_json_array_size(arr); i++) {
    const GTEXT_JSON_Value * e = gtext_json_array_get(arr, i);
    const GTEXT_JSON_Value * states = get_arr(e, "states");
    const char * pid;
    size_t pid_len;
    const char * cr;
    size_t cr_len;
    SEnt * x;
    size_t s;
    /* In byte order of public id, as an export lists them. */
    if (!is_obj(e, 7) || states == NULL || !get_str(e, "id", &pid, &pid_len) ||
        !gdefstor_model_id_ok(pid, pid_len, cap->max_public_id_bytes, 0) ||
        (m->nents > ty->ent0 &&
            bytes_cmp(m->ents[m->nents - 1].pid, m->ents[m->nents - 1].pid_len,
                pid, pid_len) >= 0) ||
        !get_str(e, "creator", &cr, &cr_len) ||
        !gdefstor_model_id_ok(cr, cr_len, cap->max_public_id_bytes, 1)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = GROW(m, ents);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    x = &m->ents[m->nents];
    if (!get_u64(e, "order", &x->order) ||
        !get_i64(e, "created_ms", &x->created) ||
        !get_u64(e, "revision", &x->revision) ||
        !get_u64(e, "last_item", &x->last_item)) {
      memset(x, 0, sizeof *x);
      return GDEFSTOR_ERR_CORRUPT;
    }
    x->type = copy(m->alloc, type, type_len);
    x->pid = copy(m->alloc, pid, pid_len);
    x->creator = copy(m->alloc, cr, cr_len);
    if (x->type == NULL || x->pid == NULL || x->creator == NULL) {
      gcu_allocator_free(m->alloc, x->type);
      gcu_allocator_free(m->alloc, x->pid);
      gcu_allocator_free(m->alloc, x->creator);
      memset(x, 0, sizeof *x);
      return GDEFSTOR_ERR_OOM;
    }
    x->type_len = type_len;
    x->pid_len = pid_len;
    x->creator_len = cr_len;
    /* Rows are given out on import; until then an entity's row is its
     * place, which keys its states, revisions and versions here. */
    x->row = x->order;
    m->nents += 1;
    ty->nent += 1;
    for (s = 0; s < gtext_json_array_size(states); s++) {
      const GTEXT_JSON_Value * st = gtext_json_array_get(states, s);
      const char * actor;
      size_t actor_len;
      SState * y;
      if (!is_obj(st, 4) || !get_str(st, "actor", &actor, &actor_len) ||
          !gdefstor_model_id_ok(actor, actor_len, cap->max_public_id_bytes,
              1)) {
        return GDEFSTOR_ERR_CORRUPT;
      }
      rc = GROW(m, states);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      y = &m->states[m->nstates];
      if (!get_bool(st, "published", &y->published) ||
          !get_bool(st, "deleted", &y->deleted) ||
          !get_i64(st, "when_ms", &y->when)) {
        memset(y, 0, sizeof *y);
        return GDEFSTOR_ERR_CORRUPT;
      }
      y->actor = copy(m->alloc, actor, actor_len);
      if (y->actor == NULL) {
        return GDEFSTOR_ERR_OOM;
      }
      y->actor_len = actor_len;
      y->row = x->row;
      y->seq = s + 1;
      m->nstates += 1;
    }
  }
  return GDEFSTOR_OK;
}

/* While members are read, a type's entities are a run of ents in byte
 * order of public id. */
static SEnt * find_ent(GDEFSTOR_SnapModel * m, size_t ti, const char * pid,
    size_t pid_len) {
  size_t lo = m->types[ti].ent0;
  size_t hi = lo + m->types[ti].nent;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (bytes_cmp(m->ents[mid].pid, m->ents[mid].pid_len, pid, pid_len) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < m->types[ti].ent0 + m->types[ti].nent &&
      same(m->ents[lo].pid, m->ents[lo].pid_len, pid, pid_len)) {
    return &m->ents[lo];
  }
  return NULL;
}

static GDEFSTOR_Result read_values(GDEFSTOR_SnapModel * m,
    const GDEFSTOR_Limits * cap, const GDEFSTOR_Type * t, SVer * v,
    const GTEXT_JSON_Value * vals) {
  size_t ncols;
  size_t k;
  if (vals == NULL || gtext_json_typeof(vals) != GTEXT_JSON_OBJECT ||
      v->group >= t->next_group) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  ncols = t->group_ncols[v->group];
  v->vals = (GDEFSTOR_Value *)gdefstor_mem0(m->alloc,
      (ncols == 0 ? 1 : ncols) * sizeof *v->vals);
  if (v->vals == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  v->nvals = ncols;
  for (k = 0; k < gtext_json_object_size(vals); k++) {
    const char * key = NULL;
    size_t key_len = 0;
    const GTEXT_JSON_Value * x = NULL;
    size_t c;
    const GDEFSTOR_MField * f = NULL;
    GDEFSTOR_Result rc;
    key = gtext_json_object_key(vals, k, &key_len);
    if (key == NULL) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    x = gtext_json_object_value(vals, k);
    for (c = 0; c < ncols; c++) {
      const GDEFSTOR_MField * g = &t->f[t->col_field[t->col_base[v->group] + c]];
      if (same(g->name, g->name_len, key, key_len)) {
        f = g;
        break;
      }
    }
    /* A null is not spelled: a value is either there or absent. */
    if (f == NULL || x == NULL || gtext_json_typeof(x) == GTEXT_JSON_NULL ||
        v->vals[c].type != 0) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = gdefstor_model_value_from_json(m->alloc, f, x, &v->vals[c]);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_model_value_check(f, &v->vals[c], cap->max_public_id_bytes);
    }
    if (rc != GDEFSTOR_OK) {
      return rc == GDEFSTOR_ERR_OOM ? rc : GDEFSTOR_ERR_CORRUPT;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result read_order(const GTEXT_JSON_Value * o, double * out) {
  const char * p;
  size_t n;
  uint64_t bits = 0;
  size_t i;
  if (!get_str(o, "order", &p, &n) || n != 16) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  for (i = 0; i < n; i++) {
    int h = hex_val(p[i]);
    if (h < 0) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    bits = bits * 16 + (uint64_t)h;
  }
  memcpy(out, &bits, sizeof *out);
  return isfinite(*out) ? GDEFSTOR_OK : GDEFSTOR_ERR_CORRUPT;
}

/* A row's "schema": the stamp it was written with (model.h), or empty for a
 * row its store kept from before stamps; never missing. */
static int stamp_ok(const GTEXT_JSON_Value * row, const char ** out,
    size_t * out_len) {
  return get_str(row, "schema", out, out_len) &&
         (*out_len == 0 || gdefstor_schema_ok(*out, *out_len));
}

static GDEFSTOR_Result read_entity(GDEFSTOR_SnapModel * m,
    const GDEFSTOR_Limits * cap, const char * type, size_t type_len,
    const char * pid, size_t pid_len, const GTEXT_JSON_Value * root) {
  const GTEXT_JSON_Value * revs = get_arr(root, "revisions");
  const GTEXT_JSON_Value * items = get_arr(root, "items");
  size_t ti = find_type(m, type, type_len);
  const char * p;
  size_t n;
  SEnt * e;
  size_t i;
  GDEFSTOR_Result rc;
  /* Its own format first: version 1 and no other (AD-34). A member without
   * one is not what an export writes; one of another version is a format
   * this build does not read. */
  if (root == NULL || gtext_json_typeof(root) != GTEXT_JSON_OBJECT ||
      !get_str(root, "format", &p, &n)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (!same(p, n, REVISION_FORMAT, strlen(REVISION_FORMAT))) {
    return n > strlen(REVISION_PREFIX) &&
                   memcmp(p, REVISION_PREFIX, strlen(REVISION_PREFIX)) == 0
               ? GDEFSTOR_ERR_UNSUPPORTED
               : GDEFSTOR_ERR_CORRUPT;
  }
  if (!is_obj(root, 5) || revs == NULL || items == NULL ||
      ti == GDEFSTOR_NONE || !get_str(root, "type", &p, &n) ||
      !same(p, n, type, type_len) || !get_str(root, "id", &p, &n) ||
      !same(p, n, pid, pid_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  e = m->types[ti].seen_entities ? find_ent(m, ti, pid, pid_len) : NULL;
  if (e == NULL || e->seen_member) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  e->seen_member = 1;
  for (i = 0; i < gtext_json_array_size(revs); i++) {
    const GTEXT_JSON_Value * r = gtext_json_array_get(revs, i);
    const char * who;
    size_t who_len;
    const char * why;
    size_t why_len;
    const char * langs;
    size_t langs_len;
    const char * schema;
    size_t schema_len;
    SRev * x;
    if (!is_obj(r, 7) || !get_str(r, "who", &who, &who_len) ||
        !gdefstor_model_id_ok(who, who_len, cap->max_public_id_bytes, 1) ||
        !get_str(r, "why", &why, &why_len) ||
        why_len > GDEFSTOR_MODEL_MAX_WHY_BYTES ||
        !gdefstor_model_utf8_ok((const uint8_t *)why, why_len) ||
        !get_str(r, "languages", &langs, &langs_len) ||
        !gdefstor_model_langs_ok(langs, langs_len) ||
        !stamp_ok(r, &schema, &schema_len)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = GROW(m, revs);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    x = &m->revs[m->nrevs];
    if (!get_u64(r, "revision", &x->revision) ||
        !get_u64(r, "commit", &x->commit) || x->commit == 0 ||
        !get_i64(r, "when_ms", &x->when)) {
      memset(x, 0, sizeof *x);
      return GDEFSTOR_ERR_CORRUPT;
    }
    x->who = copy(m->alloc, who, who_len);
    x->why = copy(m->alloc, why, why_len);
    x->langs = copy(m->alloc, langs, langs_len);
    x->schema = copy(m->alloc, schema, schema_len);
    if (x->who == NULL || x->why == NULL || x->langs == NULL ||
        x->schema == NULL) {
      gcu_allocator_free(m->alloc, x->who);
      gcu_allocator_free(m->alloc, x->why);
      gcu_allocator_free(m->alloc, x->langs);
      gcu_allocator_free(m->alloc, x->schema);
      memset(x, 0, sizeof *x);
      return GDEFSTOR_ERR_OOM;
    }
    x->schema_len = schema_len;
    x->who_len = who_len;
    x->why_len = why_len;
    x->langs_len = langs_len;
    x->row = e->row;
    m->nrevs += 1;
  }
  for (i = 0; i < gtext_json_array_size(items); i++) {
    const GTEXT_JSON_Value * it = gtext_json_array_get(items, i);
    const char * lang;
    size_t lang_len;
    uint64_t group = 0;
    SVer * x;
    if (!is_obj(it, 8) || !get_str(it, "lang", &lang, &lang_len) ||
        !gdefstor_model_lang_ok(lang, lang_len) ||
        !get_u64(it, "group", &group) || group > UINT32_MAX) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = GROW(m, vers);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    x = &m->vers[m->nvers];
    x->lang = copy(m->alloc, lang, lang_len);
    if (x->lang == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    x->lang_len = lang_len;
    x->group = (uint32_t)group;
    x->row = e->row;
    /* Counted now, so that a failure below frees what was read. */
    m->nvers += 1;
    if (!get_u64(it, "item", &x->item) || !get_u64(it, "parent", &x->parent) ||
        !get_u64(it, "from", &x->from) || !get_u64(it, "to", &x->to)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = read_order(it, &x->order);
    if (rc == GDEFSTOR_OK) {
      rc = read_values(m, cap, m->types[ti].decl, x, get(it, "values"));
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result read_drafts(GDEFSTOR_SnapModel * m,
    const GDEFSTOR_Limits * cap, const GTEXT_JSON_Value * root) {
  const GTEXT_JSON_Value * arr = get_arr(root, "drafts");
  size_t i;
  if (!is_obj(root, 1) || arr == NULL || gtext_json_array_size(arr) == 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  for (i = 0; i < gtext_json_array_size(arr); i++) {
    const GTEXT_JSON_Value * d = gtext_json_array_get(arr, i);
    const char * pid;
    size_t pid_len;
    const char * type;
    size_t type_len;
    const char * doc;
    size_t doc_len;
    const char * schema;
    size_t schema_len;
    SDraft * x;
    GDEFSTOR_Result rc;
    if (!is_obj(d, 5) || !stamp_ok(d, &schema, &schema_len) ||
        !get_str(d, "id", &pid, &pid_len) ||
        !gdefstor_model_id_ok(pid, pid_len, cap->max_public_id_bytes, 0) ||
        !get_str(d, "type", &type, &type_len) ||
        !gdefstor_model_type_name_ok(type, type_len) ||
        !get_str(d, "document", &doc, &doc_len) ||
        doc_len > GDEFSTOR_MODEL_MAX_DRAFT_BYTES) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    {
      GDEFSTOR_DraftPart * parts = NULL;
      size_t nparts = 0;
      rc = gdefstor_model_draft_unpack(m->alloc, (const uint8_t *)doc, doc_len,
          &parts, &nparts);
      gcu_allocator_free(m->alloc, parts);
      if (rc != GDEFSTOR_OK) {
        return rc == GDEFSTOR_ERR_OOM ? rc : GDEFSTOR_ERR_CORRUPT;
      }
    }
    rc = GROW(m, drafts);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    x = &m->drafts[m->ndrafts];
    if (!get_u64(d, "base", &x->base)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    x->pid = copy(m->alloc, pid, pid_len);
    x->type = copy(m->alloc, type, type_len);
    x->json = (uint8_t *)copy(m->alloc, doc, doc_len);
    x->schema = copy(m->alloc, schema, schema_len);
    if (x->pid == NULL || x->type == NULL || x->json == NULL ||
        x->schema == NULL) {
      gcu_allocator_free(m->alloc, x->pid);
      gcu_allocator_free(m->alloc, x->type);
      gcu_allocator_free(m->alloc, x->json);
      gcu_allocator_free(m->alloc, x->schema);
      memset(x, 0, sizeof *x);
      return GDEFSTOR_ERR_OOM;
    }
    x->schema_len = schema_len;
    x->pid_len = pid_len;
    x->type_len = type_len;
    x->json_len = doc_len;
    m->ndrafts += 1;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result read_records(GDEFSTOR_SnapModel * m,
    const GTEXT_JSON_Value * root) {
  const GTEXT_JSON_Value * arr = get_arr(root, "records");
  size_t i;
  if (!is_obj(root, 1) || arr == NULL || gtext_json_array_size(arr) == 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  for (i = 0; i < gtext_json_array_size(arr); i++) {
    const GTEXT_JSON_Value * r = gtext_json_array_get(arr, i);
    const char * ns;
    size_t ns_len;
    const char * key;
    size_t key_len;
    const char * schema;
    size_t schema_len;
    SRec * x;
    GDEFSTOR_Result rc;
    if (!is_obj(r, 4) || !stamp_ok(r, &schema, &schema_len) ||
        !get_str(r, "namespace", &ns, &ns_len) ||
        !gdefstor_model_sys_name_ok(ns, ns_len, GDEFSTOR_SYSTEM_MAX_NAME_BYTES,
            0) ||
        !get_str(r, "key", &key, &key_len) ||
        !gdefstor_model_sys_name_ok(key, key_len,
            GDEFSTOR_RECORD_MAX_KEY_BYTES, 0)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = GROW(m, recs);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    x = &m->recs[m->nrecs];
    rc = get_blob(m->alloc, r, &x->bytes, &x->len);
    if (rc == GDEFSTOR_OK && x->len > GDEFSTOR_RECORD_MAX_BYTES) {
      gcu_allocator_free(m->alloc, x->bytes);
      x->bytes = NULL;
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc != GDEFSTOR_OK) {
      memset(x, 0, sizeof *x);
      return rc;
    }
    x->ns = copy(m->alloc, ns, ns_len);
    x->key = copy(m->alloc, key, key_len);
    x->schema = copy(m->alloc, schema, schema_len);
    m->nrecs += 1;
    if (x->ns == NULL || x->key == NULL || x->schema == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    x->schema_len = schema_len;
    x->ns_len = ns_len;
    x->key_len = key_len;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result read_counters(GDEFSTOR_SnapModel * m,
    const GTEXT_JSON_Value * root) {
  const GTEXT_JSON_Value * arr = get_arr(root, "counters");
  size_t i;
  if (!is_obj(root, 1) || arr == NULL || gtext_json_array_size(arr) == 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  for (i = 0; i < gtext_json_array_size(arr); i++) {
    const GTEXT_JSON_Value * c = gtext_json_array_get(arr, i);
    const char * kind;
    size_t kind_len;
    const char * name;
    size_t name_len;
    uint64_t value = 0;
    SCnt * x;
    GDEFSTOR_Result rc;
    if (!is_obj(c, 3) || !get_str(c, "kind", &kind, &kind_len) ||
        !(same(kind, kind_len, "counter", 7) ||
            same(kind, kind_len, "stream", 6)) ||
        !get_str(c, "name", &name, &name_len) ||
        !gdefstor_model_sys_name_ok(name, name_len,
            GDEFSTOR_SYSTEM_MAX_NAME_BYTES, 0) ||
        !get_u64(c, "value", &value) || value == 0) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = GROW(m, cnts);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    x = &m->cnts[m->ncnts];
    x->name = copy(m->alloc, name, name_len);
    if (x->name == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    x->name_len = name_len;
    x->kind = kind_len == 7 ? GDEFSTOR_MODEL_COUNTER : GDEFSTOR_MODEL_STREAM;
    x->value = value;
    m->ncnts += 1;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result read_outbox(GDEFSTOR_SnapModel * m,
    const GDEFSTOR_Limits * cap, const GTEXT_JSON_Value * root) {
  const GTEXT_JSON_Value * arr = get_arr(root, "outbox");
  size_t i;
  if (!is_obj(root, 1) || arr == NULL || gtext_json_array_size(arr) == 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  for (i = 0; i < gtext_json_array_size(arr); i++) {
    const GTEXT_JSON_Value * b = gtext_json_array_get(arr, i);
    const char * stream;
    size_t stream_len;
    const char * pid;
    size_t pid_len;
    const char * fmt;
    size_t fmt_len;
    uint64_t failures = 0;
    SOb * x;
    GDEFSTOR_Result rc;
    if (!is_obj(b, 8) || !get_str(b, "stream", &stream, &stream_len) ||
        !gdefstor_model_sys_name_ok(stream, stream_len,
            GDEFSTOR_SYSTEM_MAX_NAME_BYTES, 0) ||
        !get_str(b, "id", &pid, &pid_len) ||
        !gdefstor_model_id_ok(pid, pid_len, cap->max_public_id_bytes, 0) ||
        !get_str(b, "format", &fmt, &fmt_len) ||
        !gdefstor_model_sys_name_ok(fmt, fmt_len,
            GDEFSTOR_OUTBOX_MAX_FORMAT_BYTES, 1) ||
        !get_u64(b, "failures", &failures) || failures > UINT32_MAX) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = GROW(m, obs);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    x = &m->obs[m->nobs];
    if (!get_u64(b, "seq", &x->seq) || x->seq == 0 ||
        !get_i64(b, "lease_until_ms", &x->lease) ||
        !get_bool(b, "parked", &x->parked)) {
      memset(x, 0, sizeof *x);
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = get_blob(m->alloc, b, &x->payload, &x->payload_len);
    if (rc == GDEFSTOR_OK &&
        x->payload_len > GDEFSTOR_OUTBOX_MAX_PAYLOAD_BYTES) {
      gcu_allocator_free(m->alloc, x->payload);
      x->payload = NULL;
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc != GDEFSTOR_OK) {
      memset(x, 0, sizeof *x);
      return rc;
    }
    x->stream = copy(m->alloc, stream, stream_len);
    x->pid = copy(m->alloc, pid, pid_len);
    x->fmt = copy(m->alloc, fmt, fmt_len);
    m->nobs += 1;
    if (x->stream == NULL || x->pid == NULL || x->fmt == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    x->stream_len = stream_len;
    x->pid_len = pid_len;
    x->fmt_len = fmt_len;
    x->failures = (uint32_t)failures;
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_snap_model_member(GDEFSTOR_SnapModel ** pm,
    const GDEFSTOR_Allocator * alloc, const GDEFSTOR_Limits * cap,
    const char * name, size_t name_len, const uint8_t * bytes, size_t len) {
  GDEFSTOR_SnapModel * m = *pm;
  GTEXT_JSON_Value * root = NULL;
  int kind = 0;
  char * type = NULL;
  size_t type_len = 0;
  char * pid = NULL;
  size_t pid_len = 0;
  GDEFSTOR_Result rc;
  if (m == NULL) {
    m = model_new(alloc);
    if (m == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    *pm = m;
  }
  rc = split_name(alloc, name, name_len, &kind, &type, &type_len, &pid,
      &pid_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (kind == 1) {
    rc = gdefstor_model_type_name_ok(type, type_len) &&
                 find_type(m, type, type_len) == GDEFSTOR_NONE &&
                 (m->ntypes == 0 ||
                     bytes_cmp(m->types[m->ntypes - 1].name,
                         m->types[m->ntypes - 1].name_len, type, type_len) < 0)
             ? read_type(m, type, type_len, bytes, len)
             : GDEFSTOR_ERR_CORRUPT;
  } else {
    rc = parse(alloc, cap, bytes, len, &root);
  }
  if (rc == GDEFSTOR_OK && kind == 2) {
    size_t ti = find_type(m, type, type_len);
    if (ti == GDEFSTOR_NONE || m->types[ti].seen_entities) {
      rc = GDEFSTOR_ERR_CORRUPT;
    } else {
      m->types[ti].seen_entities = 1;
      rc = read_entities(m, cap, type, type_len, root);
    }
  } else if (rc == GDEFSTOR_OK && kind == 3) {
    rc = read_entity(m, cap, type, type_len, pid, pid_len, root);
  } else if (rc == GDEFSTOR_OK && kind == 0) {
    if (same(name, name_len, "model/drafts.json", 17) && !m->seen_drafts) {
      m->seen_drafts = 1;
      rc = read_drafts(m, cap, root);
    } else if (same(name, name_len, "model/records.json", 18) &&
               !m->seen_records) {
      m->seen_records = 1;
      rc = read_records(m, root);
    } else if (same(name, name_len, "model/counters.json", 19) &&
               !m->seen_counters) {
      m->seen_counters = 1;
      rc = read_counters(m, root);
    } else if (same(name, name_len, "model/outbox.json", 17) &&
               !m->seen_outbox) {
      m->seen_outbox = 1;
      rc = read_outbox(m, cap, root);
    } else {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  if (root != NULL) {
    gtext_json_free(root);
  }
  gcu_allocator_free(alloc, type);
  gcu_allocator_free(alloc, pid);
  return rc;
}

/* ---- import: checking the whole ------------------------------------------- */

static int ob_pid_cmp(const void * ctx, size_t a, size_t b) {
  const GDEFSTOR_SnapModel * m = (const GDEFSTOR_SnapModel *)ctx;
  return bytes_cmp(m->obs[a].pid, m->obs[a].pid_len, m->obs[b].pid,
      m->obs[b].pid_len);
}

static int ent_order_qcmp(const void * a, const void * b) {
  const SEnt * x = (const SEnt *)a;
  const SEnt * y = (const SEnt *)b;
  return u64_cmp(x->order, y->order);
}

static int ent_pid_qcmp(const void * a, const void * b) {
  const SEnt * x = *(const SEnt * const *)a;
  const SEnt * y = *(const SEnt * const *)b;
  return bytes_cmp(x->pid, x->pid_len, y->pid, y->pid_len);
}

static const SEnt * ent_by_pid(const GDEFSTOR_SnapModel * m,
    const SEnt * const * by_pid, const char * pid, size_t pid_len) {
  size_t lo = 0;
  size_t hi = m->nents;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (bytes_cmp(by_pid[mid]->pid, by_pid[mid]->pid_len, pid, pid_len) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < m->nents &&
      same(by_pid[lo]->pid, by_pid[lo]->pid_len, pid, pid_len)) {
    return by_pid[lo];
  }
  return NULL;
}

/* One entity's revisions and versions, as an export of a real history
 * spells them: revisions 1 to the entity's revision, each with a commit
 * sequence; every version inside that range, ids within the last item,
 * no version named twice. */
static GDEFSTOR_Result check_entity(const GDEFSTOR_SnapModel * m,
    const SEnt * e) {
  size_t k;
  uint64_t top = 0;
  if (!e->seen_member || e->nrev != e->revision ||
      (e->revision == 0 && e->last_item != 0)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  for (k = 0; k < e->nrev; k++) {
    if (m->revs[e->rev0 + k].revision != k + 1) {
      return GDEFSTOR_ERR_CORRUPT;
    }
  }
  for (k = 0; k < e->nver; k++) {
    const SVer * v = &m->vers[e->ver0 + k];
    if (v->item == 0 || v->from == 0 || v->from > e->revision ||
        (v->to != 0 && (v->to <= v->from || v->to > e->revision)) ||
        (v->group == 0) != (v->parent == 0)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (k > 0) {
      const SVer * p = &m->vers[e->ver0 + k - 1];
      if (p->group == v->group && p->item == v->item && p->from == v->from) {
        return GDEFSTOR_ERR_CORRUPT;
      }
    }
    if (v->item > top) {
      top = v->item;
    }
  }
  return top <= e->last_item ? GDEFSTOR_OK : GDEFSTOR_ERR_CORRUPT;
}

GDEFSTOR_Result gdefstor_snap_model_check(GDEFSTOR_SnapModel * m) {
  const SEnt ** by_pid = NULL;
  size_t * ob_pid = NULL;
  size_t i;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (m == NULL) {
    return GDEFSTOR_OK;
  }
  for (i = 0; i < m->ntypes; i++) {
    if (!m->types[i].seen_entities) {
      return GDEFSTOR_ERR_CORRUPT;
    }
  }
  /* Each entity's place is 1, 2, 3 ...: the order rows are given out in. */
  SORT(m, ents, ent_order_qcmp);
  for (i = 0; i < m->nents; i++) {
    if (m->ents[i].order != i + 1) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    m->ents[i].type_at =
        find_type(m, m->ents[i].type, m->ents[i].type_len);
  }
  /* Their rows are their places, so the rows below link as an export's. */
  rc = link_rows(m);
  for (i = 0; rc == GDEFSTOR_OK && i < m->nents; i++) {
    rc = check_entity(m, &m->ents[i]);
  }
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  /* A public id is one entity's in the whole tenant (model.h). */
  by_pid = (const SEnt **)gdefstor_mem(m->alloc,
      (m->nents == 0 ? 1 : m->nents) * sizeof *by_pid);
  if (by_pid == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < m->nents; i++) {
    by_pid[i] = &m->ents[i];
  }
  if (m->nents > 1) {
    qsort((void *)by_pid, m->nents, sizeof *by_pid, ent_pid_qcmp);
  }
  for (i = 0; rc == GDEFSTOR_OK && i + 1 < m->nents; i++) {
    if (same(by_pid[i]->pid, by_pid[i]->pid_len, by_pid[i + 1]->pid,
            by_pid[i + 1]->pid_len)) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  /* A draft is of a declared type; of an existing entity, it is of that
   * entity's type and based on a revision it has; else it reserves its id
   * and is based on nothing. */
  SORT(m, drafts, draft_qcmp);
  for (i = 0; rc == GDEFSTOR_OK && i < m->ndrafts; i++) {
    const SDraft * d = &m->drafts[i];
    const SEnt * e = ent_by_pid(m, by_pid, d->pid, d->pid_len);
    if (find_type(m, d->type, d->type_len) == GDEFSTOR_NONE ||
        (i > 0 && same(m->drafts[i - 1].pid, m->drafts[i - 1].pid_len, d->pid,
                      d->pid_len)) ||
        (e != NULL && (!same(e->type, e->type_len, d->type, d->type_len) ||
                          d->base > e->revision)) ||
        (e == NULL && d->base != 0)) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  gcu_allocator_free(m->alloc, (void *)by_pid);
  SORT(m, recs, rec_qcmp);
  for (i = 0; rc == GDEFSTOR_OK && i + 1 < m->nrecs; i++) {
    if (rec_qcmp(&m->recs[i], &m->recs[i + 1]) == 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  SORT(m, cnts, cnt_qcmp);
  for (i = 0; rc == GDEFSTOR_OK && i + 1 < m->ncnts; i++) {
    if (cnt_qcmp(&m->cnts[i], &m->cnts[i + 1]) == 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  /* Outbox rows: one per (stream, sequence) and per public id, each within
   * its stream's counter, so the next append takes a new sequence. */
  SORT(m, obs, ob_qcmp);
  for (i = 0; rc == GDEFSTOR_OK && i < m->nobs; i++) {
    const SOb * b = &m->obs[i];
    size_t k;
    int covered = 0;
    if (i + 1 < m->nobs && ob_qcmp(b, &m->obs[i + 1]) == 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
      break;
    }
    for (k = 0; k < m->ncnts; k++) {
      if (m->cnts[k].kind == GDEFSTOR_MODEL_STREAM &&
          same(m->cnts[k].name, m->cnts[k].name_len, b->stream,
              b->stream_len) &&
          m->cnts[k].value >= b->seq) {
        covered = 1;
      }
    }
    if (!covered) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(m->alloc, m->nobs, ob_pid_cmp, m, &ob_pid);
  }
  for (i = 0; rc == GDEFSTOR_OK && i + 1 < m->nobs; i++) {
    if (ob_pid_cmp(m, ob_pid[i], ob_pid[i + 1]) == 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  gcu_allocator_free(m->alloc, ob_pid);
  return rc;
}

/* ---- import: writing ------------------------------------------------------ */

typedef struct EmptyProbe {
  int any;
} EmptyProbe;

/* Any row at all: the probe stops the dump at the first. */
#define PROBE_STOP GDEFSTOR_ERR_CONFLICT

static GDEFSTOR_Result probe_type(void * ctx, const char * n, size_t nl,
    const uint8_t * t, size_t tl) {
  (void)n;
  (void)nl;
  (void)t;
  (void)tl;
  ((EmptyProbe *)ctx)->any = 1;
  return PROBE_STOP;
}

static GDEFSTOR_Result probe_entity(void * ctx, const GDEFSTOR_ModelEntity * e) {
  (void)e;
  ((EmptyProbe *)ctx)->any = 1;
  return PROBE_STOP;
}

static GDEFSTOR_Result probe_row(void * ctx, uint64_t row, const void * x) {
  (void)row;
  (void)x;
  ((EmptyProbe *)ctx)->any = 1;
  return PROBE_STOP;
}

static GDEFSTOR_Result probe_state(void * ctx, uint64_t row,
    const GDEFSTOR_ModelState * s) {
  return probe_row(ctx, row, s);
}

static GDEFSTOR_Result probe_revision(void * ctx, uint64_t row,
    const GDEFSTOR_ModelRevision * r) {
  return probe_row(ctx, row, r);
}

static GDEFSTOR_Result probe_item(void * ctx, uint64_t row,
    const GDEFSTOR_ModelItem * i) {
  return probe_row(ctx, row, i);
}

static GDEFSTOR_Result probe_blind(void * ctx, uint64_t row,
    const GDEFSTOR_ModelBlind * b) {
  return probe_row(ctx, row, b);
}

static GDEFSTOR_Result probe_draft(void * ctx, const GDEFSTOR_ModelDraft * d) {
  return probe_row(ctx, 0, d);
}

static GDEFSTOR_Result probe_record(void * ctx, const GDEFSTOR_ModelRecord * r) {
  return probe_row(ctx, 0, r);
}

static GDEFSTOR_Result probe_counter(void * ctx, int kind, const char * name,
    size_t name_len, uint64_t value) {
  (void)kind;
  (void)name;
  (void)name_len;
  if (value == 0) {
    return GDEFSTOR_OK;
  }
  return probe_row(ctx, 0, NULL);
}

static GDEFSTOR_Result probe_outbox(void * ctx, const GDEFSTOR_ModelOutbox * o) {
  return probe_row(ctx, 0, o);
}

GDEFSTOR_Result gdefstor_snap_model_empty(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, int * out_empty) {
  EmptyProbe p;
  GDEFSTOR_ModelVisitor v;
  GDEFSTOR_Result rc;
  *out_empty = 1;
  memset(&p, 0, sizeof p);
  memset(&v, 0, sizeof v);
  v.ctx = &p;
  v.type = probe_type;
  v.entity = probe_entity;
  v.state = probe_state;
  v.revision = probe_revision;
  v.item = probe_item;
  v.draft = probe_draft;
  if (gdefstor_model_has_blind(store)) {
    v.blind = probe_blind;
  }
  if (gdefstor_model_has_sys(store)) {
    v.record = probe_record;
    v.counter = probe_counter;
    v.outbox = probe_outbox;
  }
  rc = store->model.dump(store->model.ctx, tenant, tenant_len,
      GDEFSTOR_MODEL_TRUTH, &v);
  if (p.any) {
    *out_empty = 0;
    return GDEFSTOR_OK;
  }
  return rc;
}

/* The order of an entity's versions by the revision that writes them, and
 * by the one that closes them. */
static int ver_from_cmp(const void * ctx, size_t a, size_t b) {
  const SVer * v = (const SVer *)ctx;
  return u64_cmp(v[a].from, v[b].from);
}

static int ver_to_cmp(const void * ctx, size_t a, size_t b) {
  const SVer * v = (const SVer *)ctx;
  return u64_cmp(v[a].to, v[b].to);
}

/* Write one entity's revisions back, in order: revision r inserts the
 * versions whose valid_from is r and closes those whose valid_to is r. */
static GDEFSTOR_Result restore_revisions(GDEFSTOR_Write * write, void * txn,
    const GDEFSTOR_SnapModel * m, const SEnt * e) {
  GDEFSTOR_Store * store = write->store;
  const SVer * vers = &m->vers[e->ver0];
  size_t * by_from = NULL;
  size_t * by_to = NULL;
  GDEFSTOR_ModelItem * ins = NULL;
  GDEFSTOR_ModelClose * cls = NULL;
  size_t fi = 0;
  size_t ti = 0;
  uint64_t r;
  GDEFSTOR_Result rc;
  if (e->nrev == 0) {
    return GDEFSTOR_OK;
  }
  rc = gdefstor_model_sort(m->alloc, e->nver, ver_from_cmp, vers, &by_from);
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_sort(m->alloc, e->nver, ver_to_cmp, vers, &by_to);
  }
  if (rc == GDEFSTOR_OK) {
    ins = (GDEFSTOR_ModelItem *)gdefstor_mem0(m->alloc,
        (e->nver == 0 ? 1 : e->nver) * sizeof *ins);
    cls = (GDEFSTOR_ModelClose *)gdefstor_mem0(m->alloc,
        (e->nver == 0 ? 1 : e->nver) * sizeof *cls);
    if (ins == NULL || cls == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  while (rc == GDEFSTOR_OK && ti < e->nver && vers[by_to[ti]].to == 0) {
    ti += 1;
  }
  for (r = 1; rc == GDEFSTOR_OK && r <= e->nrev; r++) {
    const SRev * sr = &m->revs[e->rev0 + r - 1];
    GDEFSTOR_ModelRevision rev;
    GDEFSTOR_ModelApply a;
    size_t ni = 0;
    size_t nc = 0;
    while (ti < e->nver && vers[by_to[ti]].to == r) {
      const SVer * v = &vers[by_to[ti]];
      cls[nc].group = v->group;
      cls[nc].item = v->item;
      cls[nc].valid_from = v->from;
      cls[nc].language = v->lang;
      cls[nc].language_len = v->lang_len;
      nc += 1;
      ti += 1;
    }
    while (fi < e->nver && vers[by_from[fi]].from == r) {
      const SVer * v = &vers[by_from[fi]];
      ins[ni].group = v->group;
      ins[ni].item = v->item;
      ins[ni].parent = v->parent;
      ins[ni].order = v->order;
      ins[ni].valid_from = r;
      ins[ni].valid_to = 0;
      ins[ni].values = v->vals;
      ins[ni].value_count = v->nvals;
      ins[ni].language = v->lang;
      ins[ni].language_len = v->lang_len;
      ni += 1;
      fi += 1;
    }
    memset(&rev, 0, sizeof rev);
    rev.revision = r;
    rev.commit_seq = sr->commit;
    rev.who = sr->who;
    rev.who_len = sr->who_len;
    rev.when_ms = sr->when;
    rev.why = sr->why;
    rev.why_len = sr->why_len;
    rev.languages = sr->langs;
    rev.languages_len = sr->langs_len;
    rev.schema = sr->schema;
    rev.schema_len = sr->schema_len;
    memset(&a, 0, sizeof a);
    a.row = e->row;
    a.revision = &rev;
    a.last_item = e->last_item;
    a.closes = cls;
    a.close_count = nc;
    a.inserts = ins;
    a.insert_count = ni;
    rc = store->model.restore_revision(store->model.restore_ctx, txn, &a);
  }
  gcu_allocator_free(m->alloc, by_from);
  gcu_allocator_free(m->alloc, by_to);
  gcu_allocator_free(m->alloc, ins);
  gcu_allocator_free(m->alloc, cls);
  return rc;
}

GDEFSTOR_Result gdefstor_snap_model_import(GDEFSTOR_Write * write,
    const GDEFSTOR_SnapModel * cm) {
  GDEFSTOR_SnapModel * m = (GDEFSTOR_SnapModel *)cm;
  GDEFSTOR_Store * store = write->store;
  void * txn = NULL;
  size_t i;
  GDEFSTOR_Result rc;
  if (gdefstor_snap_model_none(m)) {
    return GDEFSTOR_OK;
  }
  if (!gdefstor_model_has_restore(store) ||
      ((m->nrecs > 0 || m->ncnts > 0 || m->nobs > 0) &&
          !gdefstor_model_has_sys(store))) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  rc = gdefstor_model_txn(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  for (i = 0; rc == GDEFSTOR_OK && i < m->ntypes; i++) {
    rc = store->model.type_store(store->model.ctx, txn, m->types[i].decl,
        m->types[i].text, m->types[i].text_len);
  }
  /* Rows are given out in the entities' places, so the tenant's rows keep
   * the order they had. */
  for (i = 0; rc == GDEFSTOR_OK && i < m->nents; i++) {
    SEnt * e = &m->ents[i];
    GDEFSTOR_ModelEntity me;
    uint64_t row = 0;
    size_t s;
    memset(&me, 0, sizeof me);
    me.type = e->type;
    me.type_len = e->type_len;
    me.public_id = e->pid;
    me.public_id_len = e->pid_len;
    me.creator = e->creator;
    me.creator_len = e->creator_len;
    me.created_ms = e->created;
    rc = store->model.entity_insert(store->model.ctx, txn, &me, &row);
    if (rc == GDEFSTOR_OK && row == 0) {
      rc = GDEFSTOR_ERR_INTERNAL;
    }
    if (rc != GDEFSTOR_OK) {
      break;
    }
    e->row = row;
    for (s = 0; rc == GDEFSTOR_OK && s < e->nstate; s++) {
      const SState * st = &m->states[e->state0 + s];
      GDEFSTOR_ModelState ms;
      memset(&ms, 0, sizeof ms);
      ms.published = st->published;
      ms.deleted = st->deleted;
      ms.actor = st->actor;
      ms.actor_len = st->actor_len;
      ms.when_ms = st->when;
      rc = store->model.state_append(store->model.ctx, txn, row, &ms);
    }
    if (rc == GDEFSTOR_OK) {
      rc = restore_revisions(write, txn, m, e);
    }
  }
  for (i = 0; rc == GDEFSTOR_OK && i < m->ndrafts; i++) {
    const SDraft * d = &m->drafts[i];
    GDEFSTOR_ModelDraft md;
    memset(&md, 0, sizeof md);
    md.public_id = d->pid;
    md.public_id_len = d->pid_len;
    md.type = d->type;
    md.type_len = d->type_len;
    md.base_revision = d->base;
    md.json = d->json;
    md.json_len = d->json_len;
    md.schema = d->schema;
    md.schema_len = d->schema_len;
    rc = store->model.draft_put(store->model.ctx, txn, &md);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < m->nrecs; i++) {
    const SRec * r = &m->recs[i];
    GDEFSTOR_ModelRecord mr;
    memset(&mr, 0, sizeof mr);
    mr.ns = r->ns;
    mr.ns_len = r->ns_len;
    mr.key = r->key;
    mr.key_len = r->key_len;
    mr.bytes = r->len == 0 ? NULL : r->bytes;
    mr.len = r->len;
    mr.schema = r->schema;
    mr.schema_len = r->schema_len;
    rc = store->model.record_put(store->model.sys_ctx, txn, &mr);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < m->ncnts; i++) {
    const SCnt * c = &m->cnts[i];
    rc = store->model.restore_counter(store->model.restore_ctx, txn, c->kind,
        c->name, c->name_len, c->value);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < m->nobs; i++) {
    const SOb * b = &m->obs[i];
    GDEFSTOR_ModelOutbox mo;
    memset(&mo, 0, sizeof mo);
    mo.stream = b->stream;
    mo.stream_len = b->stream_len;
    mo.seq = b->seq;
    mo.public_id = b->pid;
    mo.public_id_len = b->pid_len;
    mo.format = b->fmt;
    mo.format_len = b->fmt_len;
    mo.payload = b->payload_len == 0 ? NULL : b->payload;
    mo.payload_len = b->payload_len;
    mo.lease_until_ms = b->lease;
    mo.failures = b->failures;
    mo.parked = b->parked;
    rc = store->model.restore_outbox(store->model.restore_ctx, txn, &mo);
  }
  if (rc != GDEFSTOR_OK) {
    return gdefstor_model_break(write, rc);
  }
  return GDEFSTOR_OK;
}

int gdefstor_snap_model_can_hold(const GDEFSTOR_Store * store) {
  return gdefstor_model_has_restore(store) && gdefstor_model_has_sys(store);
}

GDEFSTOR_Result gdefstor_snap_model_open(GDEFSTOR_Write * write) {
  void * txn = NULL;
  return gdefstor_model_txn(write, &txn);
}
