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
 * Snapshot isolation and the per-tenant writer queue. A backend put runs
 * only from commit. A read looks at the open write only when the isolation
 * plant is on, which is what check-defects flips.
 */

#include "internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/fiber.h>
#include <string.h>

static int same_span(const char * a, size_t a_len, const char * b,
    size_t b_len) {
  if (a_len != b_len) {
    return 0;
  }
  if (a_len == 0) {
    return 1;
  }
  return memcmp(a, b, a_len) == 0;
}

static void free_list(const GDEFSTOR_Allocator * alloc, char ** keys,
    size_t * lens, size_t count) {
  size_t i;
  for (i = 0; i < count; i++) {
    gcu_allocator_free(alloc, keys[i]);
  }
  gcu_allocator_free(alloc, keys);
  gcu_allocator_free(alloc, lens);
}

void gdefstor_free_pending(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_Pending * row) {
  if (row == NULL) {
    return;
  }
  gcu_allocator_free(alloc, row->key);
  gcu_allocator_free(alloc, row->bytes);
  gcu_allocator_free(alloc, row->id);
  gcu_allocator_free(alloc, row->stream);
  gcu_allocator_free(alloc, row->payload);
  gcu_allocator_free(alloc, row);
}

static void free_pending_list(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_Pending * row) {
  while (row != NULL) {
    GDEFSTOR_Pending * next = row->next;
    gdefstor_free_pending(alloc, row);
    row = next;
  }
}

static GDEFSTOR_Gate * find_gate(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len) {
  GDEFSTOR_Gate * g;
  for (g = store->gates; g != NULL; g = g->next) {
    if (same_span(g->tenant, g->tenant_len, tenant, tenant_len)) {
      return g;
    }
  }
  return NULL;
}

typedef struct OpenLink {
  GDEFSTOR_Write * write;
  struct OpenLink * next;
} OpenLink;

static OpenLink * open_links;

static GDEFSTOR_Write * open_for(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len) {
  OpenLink * link;
  for (link = open_links; link != NULL; link = link->next) {
    if (link->write != NULL && link->write->store == store &&
        link->write->alive &&
        same_span(link->write->tenant, link->write->tenant_len, tenant,
            tenant_len)) {
      return link->write;
    }
  }
  return NULL;
}

static GDEFSTOR_Result remember_open(GDEFSTOR_Write * write) {
  OpenLink * link;
  link = (OpenLink *)gdefstor_mem0(write->store->alloc, sizeof *link);
  if (link == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  link->write = write;
  link->next = open_links;
  open_links = link;
  return GDEFSTOR_OK;
}

static void forget_open(GDEFSTOR_Write * write) {
  OpenLink ** pp = &open_links;
  while (*pp != NULL) {
    if ((*pp)->write == write) {
      OpenLink * dead = *pp;
      *pp = dead->next;
      if (write->store != NULL) {
        gcu_allocator_free(write->store->alloc, dead);
      }
      return;
    }
    pp = &(*pp)->next;
  }
}

GDEFSTOR_Result gdefstor_lookup(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len, int uncommitted,
    uint8_t ** out_bytes, size_t * out_len) {
  if (store == NULL || tenant == NULL || key == NULL || out_bytes == NULL ||
      out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (uncommitted || gdefstor_plant_see_uncommitted) {
    GDEFSTOR_Write * w = open_for(store, tenant, tenant_len);
    GDEFSTOR_Pending * p;
    if (w != NULL) {
      for (p = w->head; p != NULL; p = p->next) {
        if (p->key != NULL &&
            same_span(p->key, p->key_len, key, key_len)) {
          uint8_t * copy = (uint8_t *)gdefstor_mem(store->alloc, p->len);
          if (copy == NULL) {
            return GDEFSTOR_ERR_OOM;
          }
          if (p->len > 0) {
            memcpy(copy, p->bytes, p->len);
          }
          *out_bytes = copy;
          *out_len = p->len;
          return GDEFSTOR_OK;
        }
      }
    }
  }
  return store->backend.get(store->backend.ctx, tenant, tenant_len, key,
      key_len, out_bytes, out_len);
}

GDEFSTOR_Result gdefstor_stage(GDEFSTOR_Write * write, GDEFSTOR_Pending * row) {
  GDEFSTOR_Pending * p;
  if (write == NULL || row == NULL || !write->alive) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (row->key != NULL) {
    for (p = write->head; p != NULL; p = p->next) {
      if (p->key != NULL &&
          same_span(p->key, p->key_len, row->key, row->key_len)) {
        return GDEFSTOR_ERR_INVALID;
      }
    }
  }
  row->next = NULL;
  if (write->tail == NULL) {
    write->head = row;
    write->tail = row;
  } else {
    write->tail->next = row;
    write->tail = row;
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_count(GDEFSTOR_Store * store, GDEFSTOR_Write * write,
    const char * prefix, size_t prefix_len, size_t * out_count) {
  char ** keys = NULL;
  size_t * lens = NULL;
  size_t count = 0;
  size_t i;
  GDEFSTOR_Result rc;
  GDEFSTOR_Pending * p;
  const char * tenant;
  size_t tenant_len;

  if (out_count == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_count = 0;
  if (write != NULL) {
    tenant = write->tenant;
    tenant_len = write->tenant_len;
  } else {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = store->backend.list(store->backend.ctx, tenant, tenant_len, prefix,
      prefix_len, &keys, &lens, &count);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  *out_count = count;
  for (p = write->head; p != NULL; p = p->next) {
    int seen = 0;
    if (p->key != NULL && p->key_len >= prefix_len &&
        memcmp(p->key, prefix, prefix_len) == 0) {
      for (i = 0; i < count; i++) {
        if (lens[i] == p->key_len &&
            memcmp(keys[i], p->key, p->key_len) == 0) {
          seen = 1;
          break;
        }
      }
      if (!seen) {
        *out_count += 1;
      }
    } else if (p->key == NULL && prefix_len >= 2 &&
        ((p->kind == GDEFSTOR_KIND_REV && prefix[0] == 'r') ||
            (p->kind == GDEFSTOR_KIND_BLOB && prefix[0] == 'b') ||
            (p->kind == GDEFSTOR_KIND_PROJ && prefix[0] == 'p') ||
            (p->kind == GDEFSTOR_KIND_OUT && prefix[0] == 'o')) &&
        prefix[1] == ':') {
      *out_count += 1;
    }
  }
  free_list(store->alloc, keys, lens, count);
  return GDEFSTOR_OK;
}

GDEFSTOR_Meta * gdefstor_meta_find(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * id, size_t id_len) {
  GDEFSTOR_Meta * m;
  for (m = store->meta; m != NULL; m = m->next) {
    if (same_span(m->tenant, m->tenant_len, tenant, tenant_len) &&
        same_span(m->id, m->id_len, id, id_len)) {
      return m;
    }
  }
  return NULL;
}

GDEFSTOR_Result gdefstor_meta_add(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * id, size_t id_len) {
  GDEFSTOR_Meta * m;
  if (gdefstor_meta_find(store, tenant, tenant_len, id, id_len) != NULL) {
    return GDEFSTOR_OK;
  }
  m = (GDEFSTOR_Meta *)gdefstor_mem0(store->alloc, sizeof *m);
  if (m == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  m->tenant = gdefstor_dup(store->alloc, tenant, tenant_len);
  m->id = gdefstor_dup(store->alloc, id, id_len);
  if (m->tenant == NULL || m->id == NULL) {
    gcu_allocator_free(store->alloc, m->tenant);
    gcu_allocator_free(store->alloc, m->id);
    gcu_allocator_free(store->alloc, m);
    return GDEFSTOR_ERR_OOM;
  }
  m->tenant_len = tenant_len;
  m->id_len = id_len;
  m->next = store->meta;
  store->meta = m;
  return GDEFSTOR_OK;
}

void gdefstor_meta_drop(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * id, size_t id_len) {
  GDEFSTOR_Meta ** pp = &store->meta;
  while (*pp != NULL) {
    GDEFSTOR_Meta * m = *pp;
    if (same_span(m->tenant, m->tenant_len, tenant, tenant_len) &&
        same_span(m->id, m->id_len, id, id_len)) {
      *pp = m->next;
      gcu_allocator_free(store->alloc, m->tenant);
      gcu_allocator_free(store->alloc, m->id);
      gcu_allocator_free(store->alloc, m);
      return;
    }
    pp = &m->next;
  }
}

static void unlink_waiter(GDEFSTOR_Gate * gate, GDEFSTOR_Waiter * waiter,
    const GDEFSTOR_Allocator * alloc) {
  GDEFSTOR_Waiter ** pp = &gate->head;
  GDEFSTOR_Waiter * prev = NULL;
  while (*pp != NULL) {
    if (*pp == waiter) {
      *pp = waiter->next;
      if (gate->tail == waiter) {
        gate->tail = prev;
      }
      gcu_allocator_free(alloc, waiter);
      return;
    }
    prev = *pp;
    pp = &(*pp)->next;
  }
}

/* The parked fiber unlinks itself after yield returns. A failed switch
 * leaves the waiter queued so a later release can try again. */
static GDEFSTOR_Result wake_waiter(GDEFSTOR_Gate * gate) {
  gate->held = 0;
  if (gate->head == NULL) {
    return GDEFSTOR_OK;
  }
  if (gcu_fiber_switch_to(gate->head->fiber) != GCU_FIBER_OK) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result release_gate(GDEFSTOR_Store * store,
    GDEFSTOR_Write * write) {
  GDEFSTOR_Gate * gate = find_gate(store, write->tenant, write->tenant_len);
  write->alive = 0;
  forget_open(write);
  if (gate == NULL) {
    return GDEFSTOR_OK;
  }
  return wake_waiter(gate);
}

static GDEFSTOR_Result assign_and_encode(GDEFSTOR_Store * store,
    GDEFSTOR_Write * write);

typedef struct AppliedKey {
  char * key;
  size_t key_len;
  const char * id;
  size_t id_len;
} AppliedKey;

static void rollback_applied(GDEFSTOR_Store * store, GDEFSTOR_Write * write,
    AppliedKey * applied, size_t applied_count) {
  size_t i;
  for (i = 0; i < applied_count; i++) {
    store->backend.remove(store->backend.ctx, write->tenant, write->tenant_len,
        applied[i].key, applied[i].key_len);
    if (applied[i].id != NULL) {
      gdefstor_meta_drop(store, write->tenant, write->tenant_len,
          applied[i].id, applied[i].id_len);
    }
    gcu_allocator_free(store->alloc, applied[i].key);
  }
  gcu_allocator_free(store->alloc, applied);
}

static GDEFSTOR_Result apply_pending(GDEFSTOR_Store * store,
    GDEFSTOR_Write * write, int outbox_only) {
  GDEFSTOR_Pending * p;
  AppliedKey * applied = NULL;
  size_t applied_count = 0;
  size_t applied_cap = 0;
  GDEFSTOR_Result rc;

  rc = assign_and_encode(store, write);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  for (p = write->head; p != NULL; p = p->next) {
    AppliedKey * next;
    char * key_copy;
    if (outbox_only && p->kind != GDEFSTOR_KIND_OUT) {
      continue;
    }
    if (p->key == NULL || p->bytes == NULL) {
      rollback_applied(store, write, applied, applied_count);
      return GDEFSTOR_ERR_INTERNAL;
    }
    if (applied_count == applied_cap) {
      size_t cap = applied_cap == 0 ? 4 : applied_cap * 2;
      next = (AppliedKey *)gcu_allocator_realloc(store->alloc, applied,
          cap * sizeof *next);
      if (next == NULL) {
        rollback_applied(store, write, applied, applied_count);
        return GDEFSTOR_ERR_OOM;
      }
      applied = next;
      applied_cap = cap;
    }
    key_copy = gdefstor_dup(store->alloc, p->key, p->key_len);
    if (key_copy == NULL) {
      rollback_applied(store, write, applied, applied_count);
      return GDEFSTOR_ERR_OOM;
    }
    applied[applied_count].key = key_copy;
    applied[applied_count].key_len = p->key_len;
    applied[applied_count].id = NULL;
    applied[applied_count].id_len = 0;
    applied_count += 1;
    rc = store->backend.put(store->backend.ctx, write->tenant,
        write->tenant_len, p->key, p->key_len, p->bytes, p->len);
    if (rc != GDEFSTOR_OK) {
      rollback_applied(store, write, applied, applied_count);
      return rc;
    }
    if (p->kind == GDEFSTOR_KIND_OUT) {
      rc = gdefstor_meta_add(store, write->tenant, write->tenant_len, p->id,
          p->id_len);
      if (rc != GDEFSTOR_OK) {
        rollback_applied(store, write, applied, applied_count);
        return rc;
      }
      applied[applied_count - 1].id = p->id;
      applied[applied_count - 1].id_len = p->id_len;
    }
  }
  {
    size_t i;
    for (i = 0; i < applied_count; i++) {
      gcu_allocator_free(store->alloc, applied[i].key);
    }
  }
  gcu_allocator_free(store->alloc, applied);
  return GDEFSTOR_OK;
}

static void limits_for_stored(size_t len, GDEFSTOR_Limits * cap) {
  gdefstor_limits_default(cap);
  if (len > cap->max_document_bytes) {
    cap->max_document_bytes = len;
  }
  if (len > cap->max_public_id_bytes) {
    cap->max_public_id_bytes = len;
  }
  if (len > cap->max_stream_bytes) {
    cap->max_stream_bytes = len;
  }
  if (len > cap->max_outbox_payload_bytes) {
    cap->max_outbox_payload_bytes = len;
  }
  if (len > cap->max_json_depth) {
    cap->max_json_depth = len;
  }
}

static uint64_t parse_seq_from_row(const uint8_t * bytes, size_t len,
    const char * stream, size_t stream_len, int * match) {
  GDEFSTOR_Limits cap;
  GDEFSTOR_Outbox_Row * row = NULL;
  uint64_t seq = 0;
  *match = 0;
  limits_for_stored(len, &cap);
  if (gdefstor_decode_outbox(NULL, &cap, bytes, len, &row) != GDEFSTOR_OK) {
    return 0;
  }
  if (same_span(row->stream, row->stream_len, stream, stream_len)) {
    *match = 1;
    seq = row->seq;
  }
  gdefstor_outbox_row_free(row);
  return seq;
}

static GDEFSTOR_Result assign_and_encode(GDEFSTOR_Store * store,
    GDEFSTOR_Write * write) {
  GDEFSTOR_Pending * p;
  for (p = write->head; p != NULL; p = p->next) {
    char ** keys = NULL;
    size_t * lens = NULL;
    size_t count = 0;
    size_t i;
    uint64_t max_seq = 0;
    GDEFSTOR_Result rc;
    char seq_text[32];
    int n;
    if (p->kind != GDEFSTOR_KIND_OUT || p->encoded) {
      continue;
    }
    rc = store->backend.list(store->backend.ctx, write->tenant,
        write->tenant_len, "o:", 2, &keys, &lens, &count);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    for (i = 0; i < count; i++) {
      uint8_t * bytes = NULL;
      size_t len = 0;
      int match = 0;
      uint64_t seq;
      rc = store->backend.get(store->backend.ctx, write->tenant,
          write->tenant_len, keys[i], lens[i], &bytes, &len);
      if (rc != GDEFSTOR_OK) {
        free_list(store->alloc, keys, lens, count);
        return rc;
      }
      seq = parse_seq_from_row(bytes, len, p->stream, p->stream_len, &match);
      gcu_allocator_free(store->alloc, bytes);
      if (match && seq > max_seq) {
        max_seq = seq;
      }
    }
    free_list(store->alloc, keys, lens, count);
    {
      GDEFSTOR_Pending * earlier;
      for (earlier = write->head; earlier != p; earlier = earlier->next) {
        int match = 0;
        uint64_t seq;
        if (earlier->kind != GDEFSTOR_KIND_OUT || !earlier->encoded) {
          continue;
        }
        seq = parse_seq_from_row(earlier->bytes, earlier->len, p->stream,
            p->stream_len, &match);
        if (match && seq > max_seq) {
          max_seq = seq;
        }
      }
    }
    if (max_seq == UINT64_MAX) {
      return GDEFSTOR_ERR_LIMIT;
    }
    n = 0;
    {
      uint64_t seq = max_seq + 1;
      rc = gdefstor_encode_outbox(store->alloc, &write->limits, p->id,
          p->id_len, p->stream, p->stream_len, seq, p->payload,
          p->payload_len, &p->bytes, &p->len);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      rc = gdefstor_key_hex(store->alloc, "o:", p->id, p->id_len, NULL, 0, 0,
          &p->key, &p->key_len);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      p->encoded = 1;
      (void)seq_text;
      (void)n;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result finish_write(GDEFSTOR_Store * store,
    GDEFSTOR_Write * write) {
  const GDEFSTOR_Allocator * alloc = store->alloc;
  /* release_gate reads the tenant and may switch to the next writer before
   * it returns, so the write stays intact until that switch comes back. */
  GDEFSTOR_Result rc = release_gate(store, write);
  free_pending_list(alloc, write->head);
  write->head = NULL;
  write->tail = NULL;
  gcu_allocator_free(alloc, write->tenant);
  write->tenant = NULL;
  gcu_allocator_free(alloc, write);
  return rc;
}

GDEFSTOR_Result gdefstor_store_create(const GDEFSTOR_Backend * backend,
    const GDEFSTOR_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store) {
  GDEFSTOR_Store * store;
  if (backend == NULL || out_store == NULL || backend->destroy == NULL ||
      backend->put == NULL || backend->get == NULL ||
      backend->remove == NULL || backend->list == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  store = (GDEFSTOR_Store *)gdefstor_mem0(allocator, sizeof *store);
  if (store == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  store->alloc = gdefstor_alloc(allocator);
  gdefstor_limits_resolve(limits, &store->limits);
  store->backend = *backend;
  *out_store = store;
  return GDEFSTOR_OK;
}

void gdefstor_store_destroy(GDEFSTOR_Store * store) {
  GDEFSTOR_Meta * meta;
  GDEFSTOR_Gate * gate;
  if (store == NULL) {
    return;
  }
  for (;;) {
    OpenLink * link;
    int found = 0;
    for (link = open_links; link != NULL; link = link->next) {
      if (link->write != NULL && link->write->store == store &&
          link->write->alive) {
        gdefstor_write_abort(link->write);
        found = 1;
        break;
      }
    }
    if (!found) {
      break;
    }
  }
  meta = store->meta;
  while (meta != NULL) {
    GDEFSTOR_Meta * next = meta->next;
    gcu_allocator_free(store->alloc, meta->tenant);
    gcu_allocator_free(store->alloc, meta->id);
    gcu_allocator_free(store->alloc, meta);
    meta = next;
  }
  gate = store->gates;
  while (gate != NULL) {
    GDEFSTOR_Gate * next = gate->next;
    GDEFSTOR_Waiter * w = gate->head;
    while (w != NULL) {
      GDEFSTOR_Waiter * wn = w->next;
      gcu_allocator_free(store->alloc, w);
      w = wn;
    }
    gcu_allocator_free(store->alloc, gate->tenant);
    gcu_allocator_free(store->alloc, gate);
    gate = next;
  }
  if (store->backend.destroy != NULL) {
    store->backend.destroy(store->backend.ctx);
  }
  gcu_allocator_free(store->alloc, store);
}

GDEFSTOR_Result gdefstor_write_begin(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Write ** out_write) {
  GDEFSTOR_Gate * gate;
  GDEFSTOR_Write * write;
  GDEFSTOR_Result rc;
  if (store == NULL || out_write == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  gate = find_gate(store, tenant, tenant_len);
  if (gate == NULL) {
    gate = (GDEFSTOR_Gate *)gdefstor_mem0(store->alloc, sizeof *gate);
    if (gate == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    gate->tenant = gdefstor_dup(store->alloc, tenant, tenant_len);
    if (gate->tenant == NULL) {
      gcu_allocator_free(store->alloc, gate);
      return GDEFSTOR_ERR_OOM;
    }
    gate->tenant_len = tenant_len;
    gate->next = store->gates;
    store->gates = gate;
  }
  for (;;) {
    if (!gate->held) {
      break;
    }
    if (gcu_fiber_current() == NULL) {
      return GDEFSTOR_ERR_BUSY;
    }
    {
      GDEFSTOR_Waiter * waiter =
          (GDEFSTOR_Waiter *)gdefstor_mem0(store->alloc, sizeof *waiter);
      if (waiter == NULL) {
        return GDEFSTOR_ERR_OOM;
      }
      waiter->fiber = gcu_fiber_current();
      if (gate->tail == NULL) {
        gate->head = waiter;
        gate->tail = waiter;
      } else {
        gate->tail->next = waiter;
        gate->tail = waiter;
      }
      if (gcu_fiber_yield() != GCU_FIBER_OK) {
        unlink_waiter(gate, waiter, store->alloc);
        return GDEFSTOR_ERR_INTERNAL;
      }
      unlink_waiter(gate, waiter, store->alloc);
    }
  }
  gate->held = 1;
  write = (GDEFSTOR_Write *)gdefstor_mem0(store->alloc, sizeof *write);
  if (write == NULL) {
    rc = wake_waiter(gate);
    return rc != GDEFSTOR_OK ? rc : GDEFSTOR_ERR_OOM;
  }
  write->store = store;
  write->tenant = gdefstor_dup(store->alloc, tenant, tenant_len);
  if (write->tenant == NULL) {
    gcu_allocator_free(store->alloc, write);
    rc = wake_waiter(gate);
    return rc != GDEFSTOR_OK ? rc : GDEFSTOR_ERR_OOM;
  }
  write->tenant_len = tenant_len;
  if (limits == NULL) {
    write->limits = store->limits;
  } else {
    write->limits = *limits;
  }
  write->alive = 1;
  rc = remember_open(write);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(store->alloc, write->tenant);
    gcu_allocator_free(store->alloc, write);
    {
      GDEFSTOR_Result wake = wake_waiter(gate);
      return wake != GDEFSTOR_OK ? wake : rc;
    }
  }
  *out_write = write;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_write_commit(GDEFSTOR_Write * write) {
  GDEFSTOR_Store * store;
  GDEFSTOR_Result rc;
  if (write == NULL || !write->alive || write->store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  store = write->store;
  rc = apply_pending(store, write, 0);
  if (rc != GDEFSTOR_OK) {
    free_pending_list(store->alloc, write->head);
    write->head = NULL;
    write->tail = NULL;
  }
  {
    GDEFSTOR_Result done = finish_write(store, write);
    if (done != GDEFSTOR_OK) {
      return done;
    }
  }
  return rc;
}

GDEFSTOR_Result gdefstor_write_abort(GDEFSTOR_Write * write) {
  GDEFSTOR_Store * store;
  if (write == NULL) {
    return GDEFSTOR_OK;
  }
  if (!write->alive || write->store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  store = write->store;
  if (gdefstor_plant_keep_outbox) {
    GDEFSTOR_Result rc = apply_pending(store, write, 1);
    GDEFSTOR_Result done = finish_write(store, write);
    if (done != GDEFSTOR_OK) {
      return done;
    }
    return rc;
  }
  return finish_write(store, write);
}
