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
 * defiant-outbox/1 and the runner that appends it after commit.
 */

#include "internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/allocator.h>
#include <stdio.h>
#include <string.h>

static GDEFSTOR_Result from_json(GTEXT_JSON_Status code) {
  if (code == GTEXT_JSON_OK) {
    return GDEFSTOR_OK;
  }
  if (code == GTEXT_JSON_E_OOM) {
    return GDEFSTOR_ERR_OOM;
  }
  if (code == GTEXT_JSON_E_LIMIT || code == GTEXT_JSON_E_DEPTH) {
    return GDEFSTOR_ERR_LIMIT;
  }
  return GDEFSTOR_ERR_CORRUPT;
}

static GDEFSTOR_Result split_format(const char * text, size_t len,
    unsigned * out_version) {
  size_t slash = len;
  size_t i;
  unsigned version = 0;
  size_t name_len = strlen(GDEFSTOR_OUTBOX_NAME);
  for (i = 0; i < len; i++) {
    if (text[i] == '/') {
      slash = i;
    }
  }
  if (slash == len || slash == 0 || slash + 1 >= len) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (slash != name_len || memcmp(text, GDEFSTOR_OUTBOX_NAME, name_len) != 0) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  for (i = slash + 1; i < len; i++) {
    if (text[i] < '0' || text[i] > '9') {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (version > 1000000u) {
      return GDEFSTOR_ERR_UNSUPPORTED;
    }
    version = version * 10u + (unsigned)(text[i] - '0');
  }
  if (version == 0 || version > GDEFSTOR_OUTBOX_VERSION) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  *out_version = version;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_check_payload(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Limits * limits, const uint8_t * payload, size_t len) {
  GDEFSTOR_Limits cap;
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GTEXT_JSON_Value * root;
  if (payload == NULL || len == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  gdefstor_limits_resolve(limits, &cap);
  if (len > cap.max_outbox_payload_bytes) {
    return GDEFSTOR_ERR_LIMIT;
  }
  memset(&err, 0, sizeof err);
  opt = gtext_json_parse_options_default();
  opt.max_depth = cap.max_json_depth == 0 ? 1 : cap.max_json_depth;
  opt.max_string_bytes = cap.max_outbox_payload_bytes;
  opt.max_total_bytes = cap.max_outbox_payload_bytes;
  opt.dupkeys = GTEXT_JSON_DUPKEY_ERROR;
  opt.allocator = (const GTEXT_Allocator *)gdefstor_alloc(alloc);
  root = gtext_json_parse((const char *)payload, len, &opt, &err);
  if (root == NULL) {
    GDEFSTOR_Result rc = from_json(err.code);
    gtext_json_error_free(&err);
    return rc;
  }
  gtext_json_error_free(&err);
  gtext_json_free(root);
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_encode_outbox(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Limits * limits, const char * id, size_t id_len,
    const char * stream, size_t stream_len, uint64_t seq,
    const uint8_t * payload, size_t payload_len, uint8_t ** out,
    size_t * out_len) {
  GDEFSTOR_Limits cap;
  const GDEFSTOR_Allocator * use;
  char * buf = NULL;
  size_t len = 0;
  size_t buf_cap = 0;
  char seq_text[32];
  int n;
  GDEFSTOR_Result rc;
  if (out == NULL || out_len == NULL || seq == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  gdefstor_limits_resolve(limits, &cap);
  use = gdefstor_alloc(alloc);
  rc = gdefstor_check_payload(use, &cap, payload, payload_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (!gdefstor_span_ok(id, id_len, cap.max_public_id_bytes) ||
      !gdefstor_span_ok(stream, stream_len, cap.max_stream_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = snprintf(seq_text, sizeof seq_text, "%llu", (unsigned long long)seq);
  if (n <= 0 || (size_t)n >= sizeof seq_text) {
    return GDEFSTOR_ERR_LIMIT;
  }
  rc = gdefstor_buf_append(&buf, &len, &buf_cap, use,
      "{\"format\":\"defiant-outbox/1\",\"id\":",
      strlen("{\"format\":\"defiant-outbox/1\",\"id\":"));
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_json_escape_append(&buf, &len, &buf_cap, use, id, id_len);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, use, ",\"stream\":",
        strlen(",\"stream\":"));
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_json_escape_append(&buf, &len, &buf_cap, use, stream,
        stream_len);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, use, ",\"seq\":",
        strlen(",\"seq\":"));
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, use, seq_text, (size_t)n);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, use, ",\"payload\":",
        strlen(",\"payload\":"));
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, use, (const char *)payload,
        payload_len);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, use, "}", 1);
  }
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(use, buf);
    return rc;
  }
  if (len > cap.max_document_bytes) {
    gcu_allocator_free(use, buf);
    return GDEFSTOR_ERR_LIMIT;
  }
  *out = (uint8_t *)buf;
  *out_len = len;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_decode_outbox(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Limits * limits, const uint8_t * bytes, size_t len,
    GDEFSTOR_Outbox_Row ** out_row) {
  GDEFSTOR_Limits cap;
  const GDEFSTOR_Allocator * use;
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GTEXT_JSON_Value * root = NULL;
  GTEXT_JSON_Sink sink;
  GDEFSTOR_Outbox_Row * row = NULL;
  const char * format_text = NULL;
  size_t format_len = 0;
  unsigned version = 0;
  const GTEXT_JSON_Value * payload;
  GDEFSTOR_Result rc;
  if (bytes == NULL || out_row == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  gdefstor_limits_resolve(limits, &cap);
  use = gdefstor_alloc(alloc);
  if (len > cap.max_document_bytes) {
    return GDEFSTOR_ERR_LIMIT;
  }
  memset(&err, 0, sizeof err);
  opt = gtext_json_parse_options_default();
  opt.max_depth = cap.max_json_depth == 0 ? 1 : cap.max_json_depth;
  opt.max_string_bytes = cap.max_document_bytes;
  opt.max_total_bytes = cap.max_document_bytes;
  opt.dupkeys = GTEXT_JSON_DUPKEY_ERROR;
  opt.allocator = (const GTEXT_Allocator *)use;
  root = gtext_json_parse((const char *)bytes, len, &opt, &err);
  if (root == NULL) {
    rc = from_json(err.code);
    gtext_json_error_free(&err);
    return rc;
  }
  gtext_json_error_free(&err);
  if (gtext_json_typeof(root) != GTEXT_JSON_OBJECT) {
    gtext_json_free(root);
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (gtext_json_get_string(gtext_json_object_get(root, "format", 6), &format_text,
          &format_len) != GTEXT_JSON_OK) {
    gtext_json_free(root);
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = split_format(format_text, format_len, &version);
  if (rc != GDEFSTOR_OK) {
    gtext_json_free(root);
    return rc;
  }
  (void)version;
  row = (GDEFSTOR_Outbox_Row *)gdefstor_mem0(use, sizeof *row);
  if (row == NULL) {
    gtext_json_free(root);
    return GDEFSTOR_ERR_OOM;
  }
  row->alloc = use;
  {
    const GTEXT_JSON_Value * idv = gtext_json_object_get(root, "id", 2);
    const char * text = NULL;
    size_t text_len = 0;
    if (gtext_json_get_string(idv, &text, &text_len) != GTEXT_JSON_OK ||
        !gdefstor_span_ok(text, text_len, cap.max_public_id_bytes)) {
      gdefstor_outbox_row_free(row);
      gtext_json_free(root);
      return GDEFSTOR_ERR_CORRUPT;
    }
    row->id = gdefstor_dup(use, text, text_len);
    row->id_len = text_len;
    if (row->id == NULL) {
      gdefstor_outbox_row_free(row);
      gtext_json_free(root);
      return GDEFSTOR_ERR_OOM;
    }
  }
  {
    const GTEXT_JSON_Value * sv = gtext_json_object_get(root, "stream", 6);
    const char * text = NULL;
    size_t text_len = 0;
    if (gtext_json_get_string(sv, &text, &text_len) != GTEXT_JSON_OK ||
        !gdefstor_span_ok(text, text_len, cap.max_stream_bytes)) {
      gdefstor_outbox_row_free(row);
      gtext_json_free(root);
      return GDEFSTOR_ERR_CORRUPT;
    }
    row->stream = gdefstor_dup(use, text, text_len);
    row->stream_len = text_len;
    if (row->stream == NULL) {
      gdefstor_outbox_row_free(row);
      gtext_json_free(root);
      return GDEFSTOR_ERR_OOM;
    }
  }
  if (gtext_json_get_u64(gtext_json_object_get(root, "seq", 3), &row->seq) !=
          GTEXT_JSON_OK ||
      row->seq == 0) {
    gdefstor_outbox_row_free(row);
    gtext_json_free(root);
    return GDEFSTOR_ERR_CORRUPT;
  }
  payload = gtext_json_object_get(root, "payload", 7);
  if (payload == NULL) {
    gdefstor_outbox_row_free(row);
    gtext_json_free(root);
    return GDEFSTOR_ERR_CORRUPT;
  }
  memset(&sink, 0, sizeof sink);
  memset(&err, 0, sizeof err);
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
    gdefstor_outbox_row_free(row);
    gtext_json_free(root);
    return GDEFSTOR_ERR_OOM;
  }
  if (gtext_json_write_value(&sink, NULL, payload, &err) != GTEXT_JSON_OK) {
    rc = from_json(err.code);
    gtext_json_error_free(&err);
    gtext_json_sink_buffer_free(&sink);
    gdefstor_outbox_row_free(row);
    gtext_json_free(root);
    return rc;
  }
  gtext_json_error_free(&err);
  row->payload_len = gtext_json_sink_buffer_size(&sink);
  if (row->payload_len > cap.max_outbox_payload_bytes) {
    gtext_json_sink_buffer_free(&sink);
    gdefstor_outbox_row_free(row);
    gtext_json_free(root);
    return GDEFSTOR_ERR_LIMIT;
  }
  row->payload = (uint8_t *)gdefstor_dup(use, gtext_json_sink_buffer_data(&sink),
      row->payload_len);
  gtext_json_sink_buffer_free(&sink);
  gtext_json_free(root);
  if (row->payload == NULL) {
    gdefstor_outbox_row_free(row);
    return GDEFSTOR_ERR_OOM;
  }
  *out_row = row;
  return GDEFSTOR_OK;
}

void gdefstor_outbox_row_free(GDEFSTOR_Outbox_Row * row) {
  if (row == NULL) {
    return;
  }
  gcu_allocator_free(row->alloc, row->id);
  gcu_allocator_free(row->alloc, row->stream);
  gcu_allocator_free(row->alloc, row->payload);
  gcu_allocator_free(row->alloc, row);
}

int gdefstor_outbox_row_parked(const GDEFSTOR_Outbox_Row * row) {
  return row != NULL && row->parked;
}

uint64_t gdefstor_outbox_row_seq(const GDEFSTOR_Outbox_Row * row) {
  return row == NULL ? 0 : row->seq;
}

const uint8_t * gdefstor_outbox_row_payload(const GDEFSTOR_Outbox_Row * row,
    size_t * out_len) {
  if (out_len != NULL) {
    *out_len = row == NULL ? 0 : row->payload_len;
  }
  return row == NULL ? NULL : row->payload;
}

static int id_pending(GDEFSTOR_Write * write, const char * id, size_t id_len) {
  GDEFSTOR_Pending * p;
  for (p = write->head; p != NULL; p = p->next) {
    if (p->kind == GDEFSTOR_KIND_OUT && p->id != NULL &&
        p->id_len == id_len && memcmp(p->id, id, id_len) == 0) {
      return 1;
    }
  }
  return 0;
}

GDEFSTOR_Result gdefstor_outbox_append(GDEFSTOR_Write * write, const char * id,
    size_t id_len, const char * stream, size_t stream_len,
    const uint8_t * payload, size_t payload_len) {
  GDEFSTOR_Pending * row;
  char * key = NULL;
  size_t key_len = 0;
  uint8_t * existing = NULL;
  size_t existing_len = 0;
  size_t count = 0;
  GDEFSTOR_Result rc;
  if (write == NULL || !write->alive) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (!gdefstor_span_ok(id, id_len, write->limits.max_public_id_bytes) ||
      !gdefstor_span_ok(stream, stream_len, write->limits.max_stream_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_check_payload(write->store->alloc, &write->limits, payload,
      payload_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (id_pending(write, id, id_len)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_key_hex(write->store->alloc, "o:", id, id_len, NULL, 0, 0, &key,
      &key_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_lookup(write->store, write->tenant, write->tenant_len, key,
      key_len, 1, &existing, &existing_len);
  gcu_allocator_free(write->store->alloc, key);
  if (rc == GDEFSTOR_OK) {
    gcu_allocator_free(write->store->alloc, existing);
    return GDEFSTOR_ERR_INVALID;
  }
  if (rc != GDEFSTOR_ERR_INVALID) {
    return rc;
  }
  rc = gdefstor_count(write->store, write, "o:", 2, &count);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (count >= write->limits.max_outbox_rows) {
    return GDEFSTOR_ERR_LIMIT;
  }
  row = (GDEFSTOR_Pending *)gdefstor_mem0(write->store->alloc, sizeof *row);
  if (row == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  row->kind = GDEFSTOR_KIND_OUT;
  row->id = gdefstor_dup(write->store->alloc, id, id_len);
  row->stream = gdefstor_dup(write->store->alloc, stream, stream_len);
  row->payload = (uint8_t *)gdefstor_mem(write->store->alloc, payload_len);
  if (row->id == NULL || row->stream == NULL || row->payload == NULL) {
    gdefstor_free_pending(write->store->alloc, row);
    return GDEFSTOR_ERR_OOM;
  }
  row->id_len = id_len;
  row->stream_len = stream_len;
  memcpy(row->payload, payload, payload_len);
  row->payload_len = payload_len;
  rc = gdefstor_stage(write, row);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
  }
  return rc;
}

static void note_parked(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, GDEFSTOR_Outbox_Row * row) {
  GDEFSTOR_Meta * meta =
      gdefstor_meta_find(store, tenant, tenant_len, row->id, row->id_len);
  row->parked = meta != NULL && meta->parked;
}

GDEFSTOR_Result gdefstor_outbox_get(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * id, size_t id_len,
    GDEFSTOR_Outbox_Row ** out_row) {
  char * key = NULL;
  size_t key_len = 0;
  uint8_t * bytes = NULL;
  size_t len = 0;
  GDEFSTOR_Result rc;
  if (store == NULL || out_row == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes) ||
      !gdefstor_span_ok(id, id_len, store->limits.max_public_id_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_key_hex(store->alloc, "o:", id, id_len, NULL, 0, 0, &key,
      &key_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_lookup(store, tenant, tenant_len, key, key_len, 0, &bytes, &len);
  gcu_allocator_free(store->alloc, key);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_decode_outbox(store->alloc, &store->limits, bytes, len, out_row);
  gcu_allocator_free(store->alloc, bytes);
  if (rc == GDEFSTOR_OK) {
    note_parked(store, tenant, tenant_len, *out_row);
  }
  return rc;
}

GDEFSTOR_Result gdefstor_outbox_claim(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * stream,
    size_t stream_len, int64_t lease_ms, const GDEFSTOR_Clock * clock,
    GDEFSTOR_Outbox_Row ** out_row) {
  char ** keys = NULL;
  size_t * lens = NULL;
  size_t count = 0;
  size_t i;
  uint64_t best_seq = 0;
  int have = 0;
  char * best_id = NULL;
  size_t best_id_len = 0;
  int64_t now;
  GDEFSTOR_Result rc;
  if (store == NULL || out_row == NULL || clock == NULL ||
      clock->now_ms == NULL || lease_ms < 0 ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes) ||
      !gdefstor_span_ok(stream, stream_len, store->limits.max_stream_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  now = clock->now_ms(clock->ctx);
  rc = store->backend.list(store->backend.ctx, tenant, tenant_len, "o:", 2,
      &keys, &lens, &count);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  for (i = 0; i < count; i++) {
    uint8_t * bytes = NULL;
    size_t len = 0;
    GDEFSTOR_Outbox_Row * row = NULL;
    GDEFSTOR_Meta * meta;
    rc = store->backend.get(store->backend.ctx, tenant, tenant_len, keys[i],
        lens[i], &bytes, &len);
    if (rc != GDEFSTOR_OK) {
      gcu_allocator_free(store->alloc, best_id);
      /* list free is below via a local walk */
      {
        size_t j;
        for (j = 0; j < count; j++) {
          gcu_allocator_free(store->alloc, keys[j]);
        }
        gcu_allocator_free(store->alloc, keys);
        gcu_allocator_free(store->alloc, lens);
      }
      return rc;
    }
    rc = gdefstor_decode_outbox(store->alloc, &store->limits, bytes, len, &row);
    gcu_allocator_free(store->alloc, bytes);
    if (rc != GDEFSTOR_OK) {
      gcu_allocator_free(store->alloc, best_id);
      {
        size_t j;
        for (j = 0; j < count; j++) {
          gcu_allocator_free(store->alloc, keys[j]);
        }
        gcu_allocator_free(store->alloc, keys);
        gcu_allocator_free(store->alloc, lens);
      }
      return rc;
    }
    meta = gdefstor_meta_find(store, tenant, tenant_len, row->id, row->id_len);
    if (row->stream_len == stream_len &&
        memcmp(row->stream, stream, stream_len) == 0 &&
        (meta == NULL || (!meta->parked && now >= meta->lease_until)) &&
        (!have || row->seq < best_seq)) {
      char * copy = gdefstor_dup(store->alloc, row->id, row->id_len);
      if (copy == NULL) {
        gdefstor_outbox_row_free(row);
        gcu_allocator_free(store->alloc, best_id);
        {
          size_t j;
          for (j = 0; j < count; j++) {
            gcu_allocator_free(store->alloc, keys[j]);
          }
          gcu_allocator_free(store->alloc, keys);
          gcu_allocator_free(store->alloc, lens);
        }
        return GDEFSTOR_ERR_OOM;
      }
      gcu_allocator_free(store->alloc, best_id);
      best_id = copy;
      best_id_len = row->id_len;
      best_seq = row->seq;
      have = 1;
    }
    gdefstor_outbox_row_free(row);
  }
  {
    size_t j;
    for (j = 0; j < count; j++) {
      gcu_allocator_free(store->alloc, keys[j]);
    }
    gcu_allocator_free(store->alloc, keys);
    gcu_allocator_free(store->alloc, lens);
  }
  if (!have) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (lease_ms > 0 && now > INT64_MAX - lease_ms) {
    gcu_allocator_free(store->alloc, best_id);
    return GDEFSTOR_ERR_LIMIT;
  }
  {
    GDEFSTOR_Meta * meta =
        gdefstor_meta_find(store, tenant, tenant_len, best_id, best_id_len);
    if (meta == NULL) {
      rc = gdefstor_meta_add(store, tenant, tenant_len, best_id, best_id_len);
      if (rc != GDEFSTOR_OK) {
        gcu_allocator_free(store->alloc, best_id);
        return rc;
      }
      meta = gdefstor_meta_find(store, tenant, tenant_len, best_id, best_id_len);
    }
    if (meta == NULL) {
      gcu_allocator_free(store->alloc, best_id);
      return GDEFSTOR_ERR_INTERNAL;
    }
    meta->lease_until = now + lease_ms;
  }
  rc = gdefstor_outbox_get(store, tenant, tenant_len, best_id, best_id_len,
      out_row);
  gcu_allocator_free(store->alloc, best_id);
  return rc;
}

GDEFSTOR_Result gdefstor_outbox_ack(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * id, size_t id_len) {
  char * key = NULL;
  size_t key_len = 0;
  uint8_t * bytes = NULL;
  size_t len = 0;
  GDEFSTOR_Result rc;
  if (store == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes) ||
      !gdefstor_span_ok(id, id_len, store->limits.max_public_id_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_key_hex(store->alloc, "o:", id, id_len, NULL, 0, 0, &key,
      &key_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = store->backend.get(store->backend.ctx, tenant, tenant_len, key, key_len,
      &bytes, &len);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(store->alloc, key);
    return rc == GDEFSTOR_ERR_INVALID ? rc : rc;
  }
  gcu_allocator_free(store->alloc, bytes);
  rc = store->backend.remove(store->backend.ctx, tenant, tenant_len, key,
      key_len);
  gcu_allocator_free(store->alloc, key);
  gdefstor_meta_drop(store, tenant, tenant_len, id, id_len);
  return rc;
}

GDEFSTOR_Result gdefstor_outbox_fail(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * id, size_t id_len) {
  GDEFSTOR_Meta * meta;
  GDEFSTOR_Outbox_Row * row = NULL;
  GDEFSTOR_Result rc;
  if (store == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes) ||
      !gdefstor_span_ok(id, id_len, store->limits.max_public_id_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_outbox_get(store, tenant, tenant_len, id, id_len, &row);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  gdefstor_outbox_row_free(row);
  meta = gdefstor_meta_find(store, tenant, tenant_len, id, id_len);
  if (meta == NULL) {
    rc = gdefstor_meta_add(store, tenant, tenant_len, id, id_len);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    meta = gdefstor_meta_find(store, tenant, tenant_len, id, id_len);
  }
  if (meta == NULL) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  meta->parked = 1;
  meta->lease_until = 0;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_outbox_run(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * stream, size_t stream_len, int64_t lease_ms,
    const GDEFSTOR_Clock * clock, const GDEFSTOR_Sink * sink,
    const GDEFSTOR_Limits * limits) {
  GDEFSTOR_Limits cap;
  if (store == NULL || sink == NULL || sink->append == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (limits == NULL) {
    cap = store->limits;
  } else {
    cap = *limits;
  }
  for (;;) {
    GDEFSTOR_Outbox_Row * row = NULL;
    char * key = NULL;
    size_t key_len = 0;
    uint8_t * bytes = NULL;
    size_t len = 0;
    uint8_t * line = NULL;
    GDEFSTOR_Result rc = gdefstor_outbox_claim(store, tenant, tenant_len, stream,
        stream_len, lease_ms, clock, &row);
    if (rc == GDEFSTOR_ERR_INVALID) {
      return GDEFSTOR_OK;
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    if (row->payload_len > cap.max_outbox_payload_bytes) {
      gdefstor_outbox_fail(store, tenant, tenant_len, row->id, row->id_len);
      gdefstor_outbox_row_free(row);
      return GDEFSTOR_ERR_LIMIT;
    }
    rc = gdefstor_key_hex(store->alloc, "o:", row->id, row->id_len, NULL, 0, 0,
        &key, &key_len);
    if (rc != GDEFSTOR_OK) {
      gdefstor_outbox_row_free(row);
      return rc;
    }
    rc = gdefstor_lookup(store, tenant, tenant_len, key, key_len, 0, &bytes,
        &len);
    gcu_allocator_free(store->alloc, key);
    if (rc != GDEFSTOR_OK) {
      gdefstor_outbox_row_free(row);
      return rc;
    }
    if (len > SIZE_MAX - 1) {
      gcu_allocator_free(store->alloc, bytes);
      gdefstor_outbox_row_free(row);
      return GDEFSTOR_ERR_LIMIT;
    }
    line = (uint8_t *)gdefstor_mem(store->alloc, len + 1);
    if (line == NULL) {
      gcu_allocator_free(store->alloc, bytes);
      gdefstor_outbox_row_free(row);
      return GDEFSTOR_ERR_OOM;
    }
    memcpy(line, bytes, len);
    line[len] = (uint8_t)'\n';
    gcu_allocator_free(store->alloc, bytes);
    rc = sink->append(sink->ctx, line, len + 1);
    gcu_allocator_free(store->alloc, line);
    if (rc != GDEFSTOR_OK) {
      gdefstor_outbox_fail(store, tenant, tenant_len, row->id, row->id_len);
      gdefstor_outbox_row_free(row);
      return rc;
    }
    rc = gdefstor_outbox_ack(store, tenant, tenant_len, row->id, row->id_len);
    gdefstor_outbox_row_free(row);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
}
