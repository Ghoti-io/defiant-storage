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
 * The result of an entity query (query.h): the page of public ids and the
 * total, taken from a backend's hits (model_query.c asks the backend).
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

struct GDEFSTOR_QueryResult {
  const GDEFSTOR_Allocator * alloc;
  uint64_t total;
  size_t count;
  char ** entities;
  size_t * entity_lens;
};

void gdefstor_query_result_free(GDEFSTOR_QueryResult * r) {
  size_t i;
  if (r == NULL) {
    return;
  }
  for (i = 0; i < r->count; i++) {
    gcu_allocator_free(r->alloc, r->entities[i]);
  }
  gcu_allocator_free(r->alloc, r->entities);
  gcu_allocator_free(r->alloc, r->entity_lens);
  gcu_allocator_free(r->alloc, r);
}

size_t gdefstor_query_result_rows(const GDEFSTOR_QueryResult * r) {
  return r == NULL ? 0 : r->count;
}

uint64_t gdefstor_query_result_total(const GDEFSTOR_QueryResult * r) {
  return r == NULL ? 0 : r->total;
}

GDEFSTOR_Result gdefstor_query_result_entity(const GDEFSTOR_QueryResult * r,
    size_t row, const char ** out_entity, size_t * out_len) {
  if (r == NULL || out_entity == NULL || out_len == NULL || row >= r->count) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_entity = r->entities[row];
  *out_len = r->entity_lens[row];
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_query_result_adopt(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_ModelHits * hits, GDEFSTOR_QueryResult ** out) {
  GDEFSTOR_QueryResult * r;
  size_t i;
  r = (GDEFSTOR_QueryResult *)gdefstor_mem0(alloc, sizeof *r);
  if (r == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  r->alloc = alloc;
  r->total = hits->total;
  if (hits->count > 0) {
    r->entities = (char **)gdefstor_mem0(alloc, hits->count * sizeof *r->entities);
    r->entity_lens = (size_t *)gdefstor_mem0(alloc,
        hits->count * sizeof *r->entity_lens);
    if (r->entities == NULL || r->entity_lens == NULL) {
      gdefstor_query_result_free(r);
      return GDEFSTOR_ERR_OOM;
    }
  }
  /* The result owns the public ids from here on. */
  for (i = 0; i < hits->count; i++) {
    r->entities[i] = hits->public_ids[i];
    r->entity_lens[i] = hits->public_id_lens[i];
    hits->public_ids[i] = NULL;
    r->count = i + 1;
  }
  *out = r;
  return GDEFSTOR_OK;
}
