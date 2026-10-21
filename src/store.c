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
 * The store and the per-tenant writer queue. A write is one transaction of
 * the entity backend (model.c opens it on the write's first operation); this
 * file decides who may hold a tenant's write and when the next one runs.
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

static GDEFSTOR_Gate * find_gate(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len) {
  GDEFSTOR_Gate * gate;
  for (gate = store->gates; gate != NULL; gate = gate->next) {
    if (same_span(gate->tenant, gate->tenant_len, tenant, tenant_len)) {
      return gate;
    }
  }
  return NULL;
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

/* Release the tenant's gate and free the write. The gate's release may switch
 * to the next writer before it returns, so the write stays intact until that
 * switch comes back. */
static GDEFSTOR_Result finish_write(GDEFSTOR_Store * store,
    GDEFSTOR_Write * write) {
  const GDEFSTOR_Allocator * alloc = store->alloc;
  GDEFSTOR_Gate * gate = find_gate(store, write->tenant, write->tenant_len);
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  write->alive = 0;
  if (gate != NULL) {
    gate->open = NULL;
    rc = wake_waiter(gate);
  }
  gcu_allocator_free(alloc, write->tenant);
  write->tenant = NULL;
  gcu_allocator_free(alloc, write);
  return rc;
}

GDEFSTOR_Result gdefstor_store_create_on(const GDEFSTOR_ModelBackend * model,
    const GDEFSTOR_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store) {
  GDEFSTOR_Store * store;
  store = (GDEFSTOR_Store *)gdefstor_mem0(allocator, sizeof *store);
  if (store == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  store->alloc = gdefstor_alloc(allocator);
  gdefstor_limits_resolve(limits, &store->limits);
  store->model = *model;
  *out_store = store;
  return GDEFSTOR_OK;
}

void gdefstor_store_destroy(GDEFSTOR_Store * store) {
  GDEFSTOR_Gate * gate;
  if (store == NULL) {
    return;
  }
  /* An open write is aborted first, so nothing uncommitted is published. */
  for (gate = store->gates; gate != NULL; gate = gate->next) {
    if (gate->open != NULL && gate->open->alive) {
      gdefstor_write_abort(gate->open);
    }
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
  if (store->model.destroy != NULL) {
    store->model.destroy(store->model.ctx);
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
  gate->open = write;
  *out_write = write;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_write_commit(GDEFSTOR_Write * write) {
  GDEFSTOR_Store * store;
  GDEFSTOR_Result rc;
  GDEFSTOR_Result done;
  if (write == NULL || !write->alive || write->store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  store = write->store;
  rc = gdefstor_model_finish(write, 1);
  done = finish_write(store, write);
  return done != GDEFSTOR_OK ? done : rc;
}

GDEFSTOR_Result gdefstor_write_abort(GDEFSTOR_Write * write) {
  GDEFSTOR_Store * store;
  GDEFSTOR_Result rc;
  GDEFSTOR_Result done;
  if (write == NULL) {
    return GDEFSTOR_OK;
  }
  if (!write->alive || write->store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  store = write->store;
  rc = gdefstor_model_finish(write, 0);
  done = finish_write(store, write);
  return done != GDEFSTOR_OK ? done : rc;
}
