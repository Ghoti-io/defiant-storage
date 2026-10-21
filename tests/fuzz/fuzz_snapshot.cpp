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
 * Fuzz snapshot zips by importing them into a store whose entity backend
 * holds nothing and cannot restore: every member is read and checked, and an
 * archive that passes is refused at the end as one this store cannot hold.
 * The first byte selects tighter limits. Member names are never opened as
 * paths.
 */

#include <ghoti.io/defiant-storage/defiant-storage.h>

#include <cstring>
#include <stddef.h>
#include <stdint.h>

extern "C" {

static int g_txn;

static void nb_destroy(void * ctx) {
  (void)ctx;
}

static GDEFSTOR_Result nb_begin(void * ctx, const char * tenant,
    size_t tenant_len, void ** out_txn) {
  (void)ctx, (void)tenant, (void)tenant_len;
  *out_txn = &g_txn;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result nb_end(void * ctx, void * txn) {
  (void)ctx, (void)txn;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result nb_type_load(void * ctx, void * txn, const char * t,
    size_t tl, const char * type, size_t type_len, uint8_t ** out_text,
    size_t * out_len) {
  (void)ctx, (void)txn, (void)t, (void)tl, (void)type, (void)type_len;
  *out_text = NULL;
  *out_len = 0;
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result nb_type_store(void * ctx, void * txn,
    const GDEFSTOR_Type * decl, const uint8_t * text, size_t text_len) {
  (void)ctx, (void)txn, (void)decl, (void)text, (void)text_len;
  return GDEFSTOR_ERR_UNSUPPORTED;
}

static GDEFSTOR_Result nb_find(void * ctx, void * txn, const char * t,
    size_t tl, const char * pid, size_t pid_len,
    const GDEFSTOR_ModelVisitor * v) {
  (void)ctx, (void)txn, (void)t, (void)tl, (void)pid, (void)pid_len, (void)v;
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result nb_insert(void * ctx, void * txn,
    const GDEFSTOR_ModelEntity * e, uint64_t * out_row) {
  (void)ctx, (void)txn, (void)e, (void)out_row;
  return GDEFSTOR_ERR_UNSUPPORTED;
}

static GDEFSTOR_Result nb_state(void * ctx, void * txn, uint64_t row,
    const GDEFSTOR_ModelState * s) {
  (void)ctx, (void)txn, (void)row, (void)s;
  return GDEFSTOR_ERR_UNSUPPORTED;
}

static GDEFSTOR_Result nb_apply(void * ctx, void * txn,
    const GDEFSTOR_ModelApply * a) {
  (void)ctx, (void)txn, (void)a;
  return GDEFSTOR_ERR_UNSUPPORTED;
}

static GDEFSTOR_Result nb_history(void * ctx, void * txn, const char * t,
    size_t tl, uint64_t row, const GDEFSTOR_ModelVisitor * v) {
  (void)ctx, (void)txn, (void)t, (void)tl, (void)row, (void)v;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result nb_items(void * ctx, void * txn, const char * t,
    size_t tl, uint64_t row, uint64_t as_of, const char * lang,
    size_t lang_len, const GDEFSTOR_ModelVisitor * v) {
  (void)ctx, (void)txn, (void)t, (void)tl, (void)row, (void)as_of;
  (void)lang, (void)lang_len, (void)v;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result nb_draft_put(void * ctx, void * txn,
    const GDEFSTOR_ModelDraft * d) {
  (void)ctx, (void)txn, (void)d;
  return GDEFSTOR_ERR_UNSUPPORTED;
}

static GDEFSTOR_Result nb_draft_get(void * ctx, void * txn, const char * t,
    size_t tl, const char * pid, size_t pid_len,
    const GDEFSTOR_ModelVisitor * v) {
  (void)ctx, (void)txn, (void)t, (void)tl, (void)pid, (void)pid_len, (void)v;
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result nb_draft_delete(void * ctx, void * txn,
    const char * pid, size_t pid_len) {
  (void)ctx, (void)txn, (void)pid, (void)pid_len;
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result nb_query(void * ctx, const char * t, size_t tl,
    const GDEFSTOR_ModelPlan * plan, GDEFSTOR_ModelHits * out) {
  (void)ctx, (void)t, (void)tl, (void)plan, (void)out;
  return GDEFSTOR_ERR_UNSUPPORTED;
}

static GDEFSTOR_Result nb_rebuild(void * ctx, void * txn) {
  (void)ctx, (void)txn;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result nb_dump(void * ctx, const char * t, size_t tl,
    int which, const GDEFSTOR_ModelVisitor * v) {
  (void)ctx, (void)t, (void)tl, (void)which, (void)v;
  return GDEFSTOR_OK;
}

}  // extern "C"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Store * store = NULL;
  GDEFSTOR_ModelBackend b;
  unsigned opt;
  const uint8_t * zip;
  size_t zip_len;
  if (data == NULL || size < 2) {
    return 0;
  }
  opt = data[0];
  zip = data + 1;
  zip_len = size - 1;
  gdefstor_limits_default(&limits);
  if (opt & 1u) {
    limits.max_member_bytes = 128;
  }
  if (opt & 2u) {
    limits.max_members = 4;
  }
  if (opt & 4u) {
    limits.max_zip_ratio = 2;
  }
  if (opt & 8u) {
    limits.max_snapshot_bytes = 256;
  }
  memset(&b, 0, sizeof b);
  b.size = sizeof b;
  b.destroy = nb_destroy;
  b.begin = nb_begin;
  b.commit = nb_end;
  b.abort = nb_end;
  b.type_load = nb_type_load;
  b.type_store = nb_type_store;
  b.entity_find = nb_find;
  b.entity_insert = nb_insert;
  b.state_append = nb_state;
  b.apply = nb_apply;
  b.history = nb_history;
  b.items = nb_items;
  b.draft_put = nb_draft_put;
  b.draft_get = nb_draft_get;
  b.draft_delete = nb_draft_delete;
  b.query = nb_query;
  b.rebuild = nb_rebuild;
  b.dump = nb_dump;
  if (gdefstor_store_open(&b, NULL, &limits, &store) == GDEFSTOR_OK) {
    char * why = NULL;
    size_t why_len = 0;
    gdefstor_snapshot_import_explain(store, "t", 1, zip, zip_len, &limits,
        &why, &why_len);
    gdefstor_bytes_free(NULL, why);
    gdefstor_store_destroy(store);
  }
  return 0;
}
