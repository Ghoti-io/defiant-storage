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
 * Records, counters and the outbox in the entity write (model.h; AD-31,
 * AD-33). The port checks every name and value, assigns outbox sequences and
 * leases, and checks what a backend answers, so that every backend reports
 * the same thing; the backend keeps the rows (model_backend.h).
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/* ---- small things -------------------------------------------------------- */

static int same_span(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

/* `[A-Za-z0-9_.:-]`, and `/` with @p slash. */
static int sys_char(unsigned char c, int slash) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' ||
         c == '-' || (slash && c == '/');
}

/* 1 to @p cap bytes of sys_char(). */
static int sys_name_ok(const char * p, size_t n, size_t cap, int slash) {
  size_t i;
  if (p == NULL || n == 0 || n > cap) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (!sys_char((unsigned char)p[i], slash)) {
      return 0;
    }
  }
  return 1;
}

static int name_ok(const char * p, size_t n) {
  return sys_name_ok(p, n, GDEFSTOR_SYSTEM_MAX_NAME_BYTES, 0);
}

static int key_ok(const char * p, size_t n) {
  return sys_name_ok(p, n, GDEFSTOR_RECORD_MAX_KEY_BYTES, 0);
}

/* The store serves these families, for a read of @p tenant. */
static GDEFSTOR_Result read_ready(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len) {
  GDEFSTOR_Result rc = gdefstor_model_ready(store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  return gdefstor_model_has_sys(store) ? GDEFSTOR_OK : GDEFSTOR_ERR_UNSUPPORTED;
}

/* The write's entity transaction, for a write to these families. */
static GDEFSTOR_Result write_ready(GDEFSTOR_Write * write, void ** out_txn) {
  GDEFSTOR_Result rc;
  if (write == NULL || !write->alive || write->store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_model_ready(write->store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_model_has_sys(write->store)) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  return gdefstor_model_txn(write, out_txn);
}

/* A write of its own, for the outbox calls that take a store: the tenant's
 * writer queue and one backend transaction, as any write. */
static GDEFSTOR_Result own_begin(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, GDEFSTOR_Write ** out_write, void ** out_txn) {
  GDEFSTOR_Write * w = NULL;
  GDEFSTOR_Result rc = gdefstor_write_begin(store, tenant, tenant_len, NULL,
      &w);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_txn(w, out_txn);
  if (rc != GDEFSTOR_OK) {
    gdefstor_write_abort(w);
    return rc;
  }
  *out_write = w;
  return GDEFSTOR_OK;
}

/* Commit the write when @p rc is OK, else abort it and return @p rc. */
static GDEFSTOR_Result own_end(GDEFSTOR_Write * w, GDEFSTOR_Result rc) {
  if (rc == GDEFSTOR_OK) {
    return gdefstor_write_commit(w);
  }
  gdefstor_write_abort(w);
  return rc;
}

/* ---- records ------------------------------------------------------------- */

GDEFSTOR_Result gdefstor_record_put(GDEFSTOR_Write * write, const char * ns,
    size_t ns_len, const char * key, size_t key_len, const uint8_t * bytes,
    size_t len) {
  GDEFSTOR_ModelRecord r;
  GDEFSTOR_Store * store;
  void * txn = NULL;
  GDEFSTOR_Result rc;
  if (!name_ok(ns, ns_len) || !key_ok(key, key_len) ||
      (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (len > GDEFSTOR_RECORD_MAX_BYTES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  rc = write_ready(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  store = write->store;
  memset(&r, 0, sizeof r);
  r.ns = ns;
  r.ns_len = ns_len;
  r.key = key;
  r.key_len = key_len;
  r.bytes = len == 0 ? NULL : bytes;
  r.len = len;
  rc = store->model.record_put(store->model.sys_ctx, txn, &r);
  return rc == GDEFSTOR_OK ? rc : gdefstor_model_break(write, rc);
}

GDEFSTOR_Result gdefstor_record_delete(GDEFSTOR_Write * write, const char * ns,
    size_t ns_len, const char * key, size_t key_len, int * out_deleted) {
  GDEFSTOR_Store * store;
  void * txn = NULL;
  int deleted = 0;
  GDEFSTOR_Result rc;
  if (!name_ok(ns, ns_len) || !key_ok(key, key_len)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = write_ready(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  store = write->store;
  rc = store->model.record_delete(store->model.sys_ctx, txn, ns, ns_len, key,
      key_len, &deleted);
  if (rc != GDEFSTOR_OK) {
    return gdefstor_model_break(write, rc);
  }
  if (out_deleted != NULL) {
    *out_deleted = deleted != 0;
  }
  return GDEFSTOR_OK;
}

typedef struct RecCatch {
  const GDEFSTOR_Allocator * alloc;
  const char * ns;
  size_t ns_len;
  const char * key;
  size_t key_len;
  int found;
  uint8_t * bytes;
  size_t len;
} RecCatch;

static GDEFSTOR_Result catch_record(void * ctx, const GDEFSTOR_ModelRecord * r) {
  RecCatch * c = (RecCatch *)ctx;
  if (c->found || r == NULL || !same_span(r->ns, r->ns_len, c->ns, c->ns_len) ||
      !same_span(r->key, r->key_len, c->key, c->key_len) ||
      (r->bytes == NULL && r->len != 0)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  /* One byte more, so that an empty value is a block too. */
  c->bytes = (uint8_t *)gdefstor_mem(c->alloc, r->len + 1);
  if (c->bytes == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  if (r->len > 0) {
    memcpy(c->bytes, r->bytes, r->len);
  }
  c->len = r->len;
  c->found = 1;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_record_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * ns, size_t ns_len,
    const char * key, size_t key_len, int * out_found, uint8_t ** out_bytes,
    size_t * out_len) {
  RecCatch c;
  GDEFSTOR_ModelVisitor v;
  GDEFSTOR_Result rc = read_ready(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!name_ok(ns, ns_len) || !key_ok(key, key_len) || out_found == NULL ||
      out_bytes == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&c, 0, sizeof c);
  c.alloc = store->alloc;
  c.ns = ns;
  c.ns_len = ns_len;
  c.key = key;
  c.key_len = key_len;
  memset(&v, 0, sizeof v);
  v.ctx = &c;
  v.record = catch_record;
  rc = store->model.record_get(store->model.sys_ctx, NULL, tenant, tenant_len,
      ns, ns_len, key, key_len, &v);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(store->alloc, c.bytes);
    return rc;
  }
  *out_found = c.found;
  *out_bytes = c.bytes;
  *out_len = c.len;
  return GDEFSTOR_OK;
}

typedef struct RecItem {
  char * key;
  size_t key_len;
  uint8_t * bytes;
  size_t len;
} RecItem;

struct GDEFSTOR_RecordList {
  const GDEFSTOR_Allocator * alloc;
  RecItem * v;
  size_t n;
  size_t cap;
  /* what the page must be, while it is read */
  const char * ns;
  size_t ns_len;
  const char * prefix;
  size_t prefix_len;
  size_t limit;
};

void gdefstor_record_list_free(GDEFSTOR_RecordList * list) {
  size_t i;
  if (list == NULL) {
    return;
  }
  for (i = 0; i < list->n; i++) {
    gcu_allocator_free(list->alloc, list->v[i].key);
    gcu_allocator_free(list->alloc, list->v[i].bytes);
  }
  gcu_allocator_free(list->alloc, list->v);
  gcu_allocator_free(list->alloc, list);
}

/* Negative, 0, positive: byte order, a prefix first. */
static int key_cmp(const char * a, size_t an, const char * b, size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = n == 0 ? 0 : memcmp(a, b, n);
  if (c != 0) {
    return c;
  }
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

static GDEFSTOR_Result take_record(void * ctx, const GDEFSTOR_ModelRecord * r) {
  GDEFSTOR_RecordList * l = (GDEFSTOR_RecordList *)ctx;
  RecItem * o;
  /* The page the backend owes: this namespace, this prefix, ascending, and
   * no more than the limit. Anything else is not an answer to the call. */
  if (r == NULL || (r->bytes == NULL && r->len != 0) || l->n >= l->limit ||
      !same_span(r->ns, r->ns_len, l->ns, l->ns_len) ||
      !key_ok(r->key, r->key_len) || r->key_len < l->prefix_len ||
      (l->prefix_len > 0 && memcmp(r->key, l->prefix, l->prefix_len) != 0) ||
      (l->n > 0 && key_cmp(l->v[l->n - 1].key, l->v[l->n - 1].key_len, r->key,
                       r->key_len) >= 0)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (l->n == l->cap) {
    size_t grown = l->cap == 0 ? 8 : l->cap * 2;
    RecItem * next = (RecItem *)gcu_allocator_realloc(l->alloc, l->v,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    l->v = next;
    l->cap = grown;
  }
  o = &l->v[l->n];
  memset(o, 0, sizeof *o);
  o->key = gdefstor_dup(l->alloc, r->key, r->key_len);
  if (o->key == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  if (r->len > 0) {
    o->bytes = (uint8_t *)gdefstor_mem(l->alloc, r->len);
    if (o->bytes == NULL) {
      gcu_allocator_free(l->alloc, o->key);
      return GDEFSTOR_ERR_OOM;
    }
    memcpy(o->bytes, r->bytes, r->len);
  }
  o->key_len = r->key_len;
  o->len = r->len;
  l->n += 1;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_record_list(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * ns, size_t ns_len,
    const char * prefix, size_t prefix_len, uint64_t offset, size_t limit,
    GDEFSTOR_RecordList ** out_list) {
  GDEFSTOR_RecordList * l;
  GDEFSTOR_ModelVisitor v;
  GDEFSTOR_Result rc = read_ready(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!name_ok(ns, ns_len) || out_list == NULL || limit == 0 ||
      (prefix_len > 0 && !key_ok(prefix, prefix_len))) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (limit > GDEFSTOR_RECORD_MAX_PAGE) {
    return GDEFSTOR_ERR_LIMIT;
  }
  l = (GDEFSTOR_RecordList *)gdefstor_mem0(store->alloc, sizeof *l);
  if (l == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  l->alloc = store->alloc;
  l->ns = ns;
  l->ns_len = ns_len;
  l->prefix = prefix;
  l->prefix_len = prefix_len;
  l->limit = limit;
  memset(&v, 0, sizeof v);
  v.ctx = l;
  v.record = take_record;
  rc = store->model.record_list(store->model.sys_ctx, tenant, tenant_len, ns,
      ns_len, prefix_len == 0 ? NULL : prefix, prefix_len, offset, limit, &v);
  if (rc != GDEFSTOR_OK) {
    gdefstor_record_list_free(l);
    return rc;
  }
  l->ns = NULL;
  l->prefix = NULL;
  *out_list = l;
  return GDEFSTOR_OK;
}

size_t gdefstor_record_list_count(const GDEFSTOR_RecordList * list) {
  return list == NULL ? 0 : list->n;
}

GDEFSTOR_Result gdefstor_record_list_item(const GDEFSTOR_RecordList * list,
    size_t index, const char ** out_key, size_t * out_key_len,
    const uint8_t ** out_bytes, size_t * out_len) {
  if (list == NULL || index >= list->n || out_key == NULL ||
      out_key_len == NULL || out_bytes == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_key = list->v[index].key;
  *out_key_len = list->v[index].key_len;
  *out_bytes = list->v[index].bytes;
  *out_len = list->v[index].len;
  return GDEFSTOR_OK;
}

/* ---- counters ------------------------------------------------------------ */

GDEFSTOR_Result gdefstor_counter_next(GDEFSTOR_Write * write,
    const char * name, size_t name_len, uint64_t * out_value) {
  GDEFSTOR_Store * store;
  void * txn = NULL;
  uint64_t v = 0;
  GDEFSTOR_Result rc;
  if (!name_ok(name, name_len) || out_value == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = write_ready(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  store = write->store;
  rc = store->model.counter_next(store->model.sys_ctx, txn,
      GDEFSTOR_MODEL_COUNTER, name, name_len, &v);
  if (rc == GDEFSTOR_ERR_LIMIT) {
    /* At INT64_MAX, refused before anything was written. */
    return rc;
  }
  if (rc == GDEFSTOR_OK && (v == 0 || v > (uint64_t)INT64_MAX)) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  if (rc != GDEFSTOR_OK) {
    return gdefstor_model_break(write, rc);
  }
  *out_value = v;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_counter_peek(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * name,
    size_t name_len, uint64_t * out_value) {
  uint64_t v = 0;
  GDEFSTOR_Result rc = read_ready(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!name_ok(name, name_len) || out_value == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = store->model.counter_get(store->model.sys_ctx, tenant, tenant_len,
      GDEFSTOR_MODEL_COUNTER, name, name_len, &v);
  if (rc == GDEFSTOR_OK && v > (uint64_t)INT64_MAX) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  if (rc == GDEFSTOR_OK) {
    *out_value = v;
  }
  return rc;
}

/* ---- the outbox ---------------------------------------------------------- */

static int format_ok(const char * p, size_t n) {
  return sys_name_ok(p, n, GDEFSTOR_OUTBOX_MAX_FORMAT_BYTES, 1);
}

GDEFSTOR_Result gdefstor_model_outbox_append(GDEFSTOR_Write * write,
    const char * stream, size_t stream_len, const char * public_id,
    size_t public_id_len, const char * format, size_t format_len,
    const uint8_t * payload, size_t payload_len, uint64_t * out_seq) {
  GDEFSTOR_ModelOutbox o;
  GDEFSTOR_Store * store;
  void * txn = NULL;
  uint64_t seq = 0;
  GDEFSTOR_Result rc;
  if (write == NULL || write->store == NULL || !name_ok(stream, stream_len) ||
      !gdefstor_model_id_ok(public_id, public_id_len,
          write->store->limits.max_public_id_bytes, 0) ||
      !format_ok(format, format_len) || (payload == NULL && payload_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (payload_len > GDEFSTOR_OUTBOX_MAX_PAYLOAD_BYTES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  rc = write_ready(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  store = write->store;
  /* The sequence comes from the stream's own counter, in this transaction:
   * an aborted write gives its numbers back, as its rows never appear. */
  rc = store->model.counter_next(store->model.sys_ctx, txn,
      GDEFSTOR_MODEL_STREAM, stream, stream_len, &seq);
  if (rc == GDEFSTOR_ERR_LIMIT) {
    return rc;
  }
  if (rc == GDEFSTOR_OK && (seq == 0 || seq > (uint64_t)INT64_MAX)) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  if (rc == GDEFSTOR_OK) {
    memset(&o, 0, sizeof o);
    o.stream = stream;
    o.stream_len = stream_len;
    o.seq = seq;
    o.public_id = public_id;
    o.public_id_len = public_id_len;
    o.format = format;
    o.format_len = format_len;
    o.payload = payload_len == 0 ? NULL : payload;
    o.payload_len = payload_len;
    rc = store->model.outbox_insert(store->model.sys_ctx, txn, &o);
  }
  if (rc != GDEFSTOR_OK) {
    /* After the sequence was taken: the write is not what it was. */
    return gdefstor_model_break(write, rc);
  }
  if (out_seq != NULL) {
    *out_seq = seq;
  }
  return GDEFSTOR_OK;
}

typedef struct ObRow {
  char * stream;
  size_t stream_len;
  uint64_t seq;
  char * pid;
  size_t pid_len;
  char * format;
  size_t format_len;
  uint8_t * payload;
  size_t payload_len;
  uint32_t failures;
  int64_t until;
} ObRow;

struct GDEFSTOR_OutboxClaim {
  const GDEFSTOR_Allocator * alloc;
  ObRow * v;
  size_t n;
  size_t cap;
  /* what the claim must be, while it is taken */
  const char * stream;
  size_t stream_len;
  int64_t until;
  size_t max;
};

void gdefstor_model_outbox_claim_free(GDEFSTOR_OutboxClaim * claim) {
  size_t i;
  if (claim == NULL) {
    return;
  }
  for (i = 0; i < claim->n; i++) {
    gcu_allocator_free(claim->alloc, claim->v[i].stream);
    gcu_allocator_free(claim->alloc, claim->v[i].pid);
    gcu_allocator_free(claim->alloc, claim->v[i].format);
    gcu_allocator_free(claim->alloc, claim->v[i].payload);
  }
  gcu_allocator_free(claim->alloc, claim->v);
  gcu_allocator_free(claim->alloc, claim);
}

static GDEFSTOR_Result take_claimed(void * ctx, const GDEFSTOR_ModelOutbox * o) {
  GDEFSTOR_OutboxClaim * c = (GDEFSTOR_OutboxClaim *)ctx;
  ObRow * r;
  /* The rows the backend owes: this stream, unparked, under this lease,
   * strictly ascending, and no more than asked. */
  if (o == NULL || c->n >= c->max || o->parked ||
      o->lease_until_ms != c->until ||
      !same_span(o->stream, o->stream_len, c->stream, c->stream_len) ||
      o->seq == 0 || (c->n > 0 && c->v[c->n - 1].seq >= o->seq) ||
      (o->public_id == NULL && o->public_id_len != 0) ||
      (o->format == NULL && o->format_len != 0) ||
      (o->payload == NULL && o->payload_len != 0)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (c->n == c->cap) {
    size_t grown = c->cap == 0 ? 8 : c->cap * 2;
    ObRow * next = (ObRow *)gcu_allocator_realloc(c->alloc, c->v,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    c->v = next;
    c->cap = grown;
  }
  r = &c->v[c->n];
  memset(r, 0, sizeof *r);
  r->stream = gdefstor_dup(c->alloc, o->stream, o->stream_len);
  r->pid = gdefstor_dup(c->alloc, o->public_id, o->public_id_len);
  r->format = gdefstor_dup(c->alloc, o->format, o->format_len);
  if (o->payload_len > 0) {
    r->payload = (uint8_t *)gdefstor_mem(c->alloc, o->payload_len);
    if (r->payload != NULL) {
      memcpy(r->payload, o->payload, o->payload_len);
    }
  }
  if (r->stream == NULL || r->pid == NULL || r->format == NULL ||
      (o->payload_len > 0 && r->payload == NULL)) {
    gcu_allocator_free(c->alloc, r->stream);
    gcu_allocator_free(c->alloc, r->pid);
    gcu_allocator_free(c->alloc, r->format);
    gcu_allocator_free(c->alloc, r->payload);
    return GDEFSTOR_ERR_OOM;
  }
  r->stream_len = o->stream_len;
  r->seq = o->seq;
  r->pid_len = o->public_id_len;
  r->format_len = o->format_len;
  r->payload_len = o->payload_len;
  r->failures = o->failures;
  r->until = o->lease_until_ms;
  c->n += 1;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_model_outbox_claim(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * stream,
    size_t stream_len, int64_t lease_ms, int64_t now_ms, size_t max,
    GDEFSTOR_OutboxClaim ** out_claim) {
  GDEFSTOR_OutboxClaim * c;
  GDEFSTOR_ModelVisitor v;
  GDEFSTOR_Write * w = NULL;
  void * txn = NULL;
  GDEFSTOR_Result rc = read_ready(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!name_ok(stream, stream_len) || out_claim == NULL || lease_ms <= 0 ||
      max == 0 || now_ms > INT64_MAX - lease_ms) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (max > GDEFSTOR_OUTBOX_MAX_CLAIM) {
    return GDEFSTOR_ERR_LIMIT;
  }
  c = (GDEFSTOR_OutboxClaim *)gdefstor_mem0(store->alloc, sizeof *c);
  if (c == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  c->alloc = store->alloc;
  c->stream = stream;
  c->stream_len = stream_len;
  c->until = now_ms + lease_ms;
  c->max = max;
  rc = own_begin(store, tenant, tenant_len, &w, &txn);
  if (rc != GDEFSTOR_OK) {
    gdefstor_model_outbox_claim_free(c);
    return rc;
  }
  memset(&v, 0, sizeof v);
  v.ctx = c;
  v.outbox = take_claimed;
  rc = store->model.outbox_claim(store->model.sys_ctx, txn, stream, stream_len,
      now_ms, c->until, max, &v);
  rc = own_end(w, rc);
  if (rc != GDEFSTOR_OK) {
    gdefstor_model_outbox_claim_free(c);
    return rc;
  }
  c->stream = NULL;
  *out_claim = c;
  return GDEFSTOR_OK;
}

size_t gdefstor_model_outbox_claim_count(const GDEFSTOR_OutboxClaim * claim) {
  return claim == NULL ? 0 : claim->n;
}

GDEFSTOR_Result gdefstor_model_outbox_claim_entry(
    const GDEFSTOR_OutboxClaim * claim, size_t index,
    GDEFSTOR_OutboxEntry * out) {
  const ObRow * r;
  if (claim == NULL || index >= claim->n || out == NULL ||
      out->size != sizeof *out) {
    return GDEFSTOR_ERR_INVALID;
  }
  r = &claim->v[index];
  memset(out, 0, sizeof *out);
  out->size = sizeof *out;
  out->stream = r->stream;
  out->stream_len = r->stream_len;
  out->seq = r->seq;
  out->public_id = r->pid;
  out->public_id_len = r->pid_len;
  out->format = r->format;
  out->format_len = r->format_len;
  out->payload = r->payload;
  out->payload_len = r->payload_len;
  out->failures = r->failures;
  out->lease_until_ms = r->until;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_model_outbox_delivered(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * const * public_ids,
    const size_t * public_id_lens, size_t count, size_t * out_deleted) {
  GDEFSTOR_Write * w = NULL;
  void * txn = NULL;
  size_t deleted = 0;
  size_t i;
  GDEFSTOR_Result rc = read_ready(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (count > 0 && (public_ids == NULL || public_id_lens == NULL)) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; i < count; i++) {
    if (!gdefstor_model_id_ok(public_ids[i], public_id_lens[i],
            store->limits.max_public_id_bytes, 0)) {
      return GDEFSTOR_ERR_INVALID;
    }
  }
  if (count > 0) {
    rc = own_begin(store, tenant, tenant_len, &w, &txn);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    for (i = 0; rc == GDEFSTOR_OK && i < count; i++) {
      int gone = 0;
      rc = store->model.outbox_delete(store->model.sys_ctx, txn, public_ids[i],
          public_id_lens[i], &gone);
      deleted += gone != 0;
    }
    rc = own_end(w, rc);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  if (out_deleted != NULL) {
    *out_deleted = deleted;
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_model_outbox_failed(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, int64_t now_ms, int * out_parked) {
  GDEFSTOR_Write * w = NULL;
  void * txn = NULL;
  int found = 0;
  int parked = 0;
  GDEFSTOR_Result rc = read_ready(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_model_id_ok(public_id, public_id_len,
          store->limits.max_public_id_bytes, 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = own_begin(store, tenant, tenant_len, &w, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = store->model.outbox_fail(store->model.sys_ctx, txn, public_id,
      public_id_len, now_ms, GDEFSTOR_OUTBOX_PARK_FAILURES, &found, &parked);
  if (rc == GDEFSTOR_OK && !found) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  rc = own_end(w, rc);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (out_parked != NULL) {
    *out_parked = parked != 0;
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_model_outbox_parked(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * stream,
    size_t stream_len, uint64_t * out_count) {
  uint64_t n = 0;
  GDEFSTOR_Result rc = read_ready(store, tenant, tenant_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (out_count == NULL || (stream_len > 0 && !name_ok(stream, stream_len))) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = store->model.outbox_parked(store->model.sys_ctx, tenant, tenant_len,
      stream_len == 0 ? NULL : stream, stream_len, &n);
  if (rc == GDEFSTOR_OK) {
    *out_count = n;
  }
  return rc;
}
