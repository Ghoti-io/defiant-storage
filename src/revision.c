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
 * defiant-revision/1. Writers emit that version. A higher version is
 * refused; there is no older one yet, and the reader is shaped to keep
 * accepting this one when a later version is added.
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

static GDEFSTOR_Result check_tree(const GTEXT_JSON_Value * value,
    const GDEFSTOR_Limits * limits, size_t depth) {
  GTEXT_JSON_Type type;
  size_t i;
  size_t n;
  int has_blob = 0;
  if (depth > limits->max_json_depth) {
    return GDEFSTOR_ERR_LIMIT;
  }
  type = gtext_json_typeof(value);
  if (type == GTEXT_JSON_ARRAY) {
    n = gtext_json_array_size(value);
    for (i = 0; i < n; i++) {
      GDEFSTOR_Result rc =
          check_tree(gtext_json_array_get(value, i), limits, depth + 1);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    return GDEFSTOR_OK;
  }
  if (type != GTEXT_JSON_OBJECT) {
    return GDEFSTOR_OK;
  }
  n = gtext_json_object_size(value);
  for (i = 0; i < n; i++) {
    size_t key_len = 0;
    const char * key = gtext_json_object_key(value, i, &key_len);
    if (key != NULL && key_len == 4 && memcmp(key, "blob", 4) == 0) {
      has_blob = 1;
    }
  }
  if (has_blob) {
    const GTEXT_JSON_Value * blob;
    const char * text = NULL;
    size_t text_len = 0;
    if (n != 1) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    blob = gtext_json_object_get(value, "blob", 4);
    if (gtext_json_get_string(blob, &text, &text_len) != GTEXT_JSON_OK) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (text_len == 0) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (text_len > limits->max_hash_bytes) {
      return GDEFSTOR_ERR_LIMIT;
    }
    return GDEFSTOR_OK;
  }
  for (i = 0; i < n; i++) {
    GDEFSTOR_Result rc = check_tree(gtext_json_object_value(value, i), limits,
        depth + 1);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result split_format(const char * text, size_t len,
    const char * expect_name, unsigned * out_version) {
  size_t slash = len;
  size_t i;
  unsigned version = 0;
  size_t name_len = strlen(expect_name);
  for (i = 0; i < len; i++) {
    if (text[i] == '/') {
      slash = i;
    }
  }
  if (slash == len || slash == 0 || slash + 1 >= len) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (slash != name_len || memcmp(text, expect_name, name_len) != 0) {
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
  if (version == 0 || version > GDEFSTOR_REVISION_VERSION) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  *out_version = version;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result take_string(const GTEXT_JSON_Value * obj,
    const char * key, const GDEFSTOR_Allocator * alloc, size_t cap,
    int allow_empty, char ** out, size_t * out_len) {
  const GTEXT_JSON_Value * value = gtext_json_object_get(obj, key, strlen(key));
  const char * text = NULL;
  size_t len = 0;
  if (gtext_json_get_string(value, &text, &len) != GTEXT_JSON_OK) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (len > cap) {
    return GDEFSTOR_ERR_LIMIT;
  }
  if (!allow_empty && len == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out = gdefstor_dup(alloc, text, len);
  if (*out == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  *out_len = len;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_revision_parse(const uint8_t * bytes, size_t len,
    const GDEFSTOR_Limits * limits, const GDEFSTOR_Allocator * allocator,
    GDEFSTOR_Revision ** out_revision) {
  GDEFSTOR_Limits cap;
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GTEXT_JSON_Sink sink;
  GTEXT_JSON_Value * root = NULL;
  GDEFSTOR_Revision * rev = NULL;
  const GTEXT_JSON_Value * format;
  const GTEXT_JSON_Value * lang;
  const char * format_text = NULL;
  size_t format_len = 0;
  unsigned version = 0;
  GDEFSTOR_Result rc;
  const GDEFSTOR_Allocator * alloc = gdefstor_alloc(allocator);

  if (bytes == NULL || out_revision == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  gdefstor_limits_resolve(limits, &cap);
  if (gdefstor_plant_enforce_limit && len > cap.max_document_bytes) {
    return GDEFSTOR_ERR_LIMIT;
  }
  memset(&err, 0, sizeof err);
  opt = gtext_json_parse_options_default();
  opt.max_depth = cap.max_json_depth == 0 ? 1 : cap.max_json_depth;
  opt.max_string_bytes = cap.max_document_bytes;
  opt.max_total_bytes =
      gdefstor_plant_enforce_limit ? cap.max_document_bytes : (size_t)-1;
  opt.dupkeys = GTEXT_JSON_DUPKEY_ERROR;
  opt.allocator = (const GTEXT_Allocator *)alloc;
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
  format = gtext_json_object_get(root, "format", 6);
  if (gtext_json_get_string(format, &format_text, &format_len) !=
      GTEXT_JSON_OK) {
    gtext_json_free(root);
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = split_format(format_text, format_len, GDEFSTOR_REVISION_NAME, &version);
  if (rc != GDEFSTOR_OK) {
    gtext_json_free(root);
    return rc;
  }
  (void)version;
  rev = (GDEFSTOR_Revision *)gdefstor_mem0(alloc, sizeof *rev);
  if (rev == NULL) {
    gtext_json_free(root);
    return GDEFSTOR_ERR_OOM;
  }
  rev->alloc = alloc;
  if (gtext_json_get_u64(gtext_json_object_get(root, "seq", 3), &rev->seq) !=
      GTEXT_JSON_OK ||
      rev->seq == 0) {
    gdefstor_revision_free(rev);
    gtext_json_free(root);
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = take_string(root, "entity", alloc, cap.max_public_id_bytes, 0,
      &rev->entity, &rev->entity_len);
  if (rc != GDEFSTOR_OK) {
    gdefstor_revision_free(rev);
    gtext_json_free(root);
    return rc;
  }
  rc = take_string(root, "type", alloc, cap.max_type_bytes, 0, &rev->type_name,
      &rev->type_len);
  if (rc != GDEFSTOR_OK) {
    gdefstor_revision_free(rev);
    gtext_json_free(root);
    return rc;
  }
  rc = take_string(root, "body", alloc, cap.max_body_bytes, 1, &rev->body,
      &rev->body_len);
  if (rc != GDEFSTOR_OK) {
    gdefstor_revision_free(rev);
    gtext_json_free(root);
    return rc;
  }
  lang = gtext_json_object_get(root, "lang", 4);
  if (lang == NULL || gtext_json_typeof(lang) != GTEXT_JSON_OBJECT) {
    gdefstor_revision_free(rev);
    gtext_json_free(root);
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = check_tree(lang, &cap, 1);
  if (rc != GDEFSTOR_OK) {
    gdefstor_revision_free(rev);
    gtext_json_free(root);
    return rc;
  }
  memset(&sink, 0, sizeof sink);
  memset(&err, 0, sizeof err);
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
    gdefstor_revision_free(rev);
    gtext_json_free(root);
    return GDEFSTOR_ERR_OOM;
  }
  if (gtext_json_write_value(&sink, NULL, lang, &err) != GTEXT_JSON_OK) {
    rc = from_json(err.code);
    gtext_json_error_free(&err);
    gtext_json_sink_buffer_free(&sink);
    gdefstor_revision_free(rev);
    gtext_json_free(root);
    return rc;
  }
  gtext_json_error_free(&err);
  rev->lang_json_len = gtext_json_sink_buffer_size(&sink);
  rev->lang_json =
      gdefstor_dup(alloc, gtext_json_sink_buffer_data(&sink), rev->lang_json_len);
  gtext_json_sink_buffer_free(&sink);
  gtext_json_free(root);
  if (rev->lang_json == NULL) {
    gdefstor_revision_free(rev);
    return GDEFSTOR_ERR_OOM;
  }
  *out_revision = rev;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_revision_write(const GDEFSTOR_Revision * revision,
    const GDEFSTOR_Limits * limits, const GDEFSTOR_Allocator * allocator,
    uint8_t ** out_bytes, size_t * out_len) {
  GDEFSTOR_Limits cap;
  const GDEFSTOR_Allocator * alloc;
  char * buf = NULL;
  size_t len = 0;
  size_t buf_cap = 0;
  char seq[32];
  int n;
  GDEFSTOR_Result rc;

  if (revision == NULL || out_bytes == NULL || out_len == NULL ||
      revision->entity == NULL || revision->type_name == NULL ||
      revision->lang_json == NULL || revision->body == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  gdefstor_limits_resolve(limits, &cap);
  alloc = gdefstor_alloc(allocator);
  n = snprintf(seq, sizeof seq, "%llu", (unsigned long long)revision->seq);
  if (n <= 0 || (size_t)n >= sizeof seq) {
    return GDEFSTOR_ERR_LIMIT;
  }
  rc = gdefstor_buf_append(&buf, &len, &buf_cap, alloc,
      "{\"format\":\"defiant-revision/1\",\"seq\":",
      strlen("{\"format\":\"defiant-revision/1\",\"seq\":"));
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, alloc, seq, (size_t)n);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, alloc, ",\"entity\":",
        strlen(",\"entity\":"));
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_json_escape_append(&buf, &len, &buf_cap, alloc,
        revision->entity, revision->entity_len);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, alloc, ",\"type\":",
        strlen(",\"type\":"));
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_json_escape_append(&buf, &len, &buf_cap, alloc,
        revision->type_name, revision->type_len);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, alloc, ",\"lang\":",
        strlen(",\"lang\":"));
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, alloc, revision->lang_json,
        revision->lang_json_len);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, alloc, ",\"body\":",
        strlen(",\"body\":"));
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_json_escape_append(&buf, &len, &buf_cap, alloc, revision->body,
        revision->body_len);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &buf_cap, alloc, "}", 1);
  }
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(alloc, buf);
    return rc;
  }
  if (gdefstor_plant_enforce_limit && len > cap.max_document_bytes) {
    gcu_allocator_free(alloc, buf);
    return GDEFSTOR_ERR_LIMIT;
  }
  *out_bytes = (uint8_t *)buf;
  *out_len = len;
  return GDEFSTOR_OK;
}

void gdefstor_revision_free(GDEFSTOR_Revision * revision) {
  if (revision == NULL) {
    return;
  }
  gcu_allocator_free(revision->alloc, revision->entity);
  gcu_allocator_free(revision->alloc, revision->type_name);
  gcu_allocator_free(revision->alloc, revision->body);
  gcu_allocator_free(revision->alloc, revision->lang_json);
  gcu_allocator_free(revision->alloc, revision);
}

uint64_t gdefstor_revision_seq(const GDEFSTOR_Revision * revision) {
  return revision == NULL ? 0 : revision->seq;
}

const char * gdefstor_revision_entity(const GDEFSTOR_Revision * revision,
    size_t * out_len) {
  if (out_len != NULL) {
    *out_len = revision == NULL ? 0 : revision->entity_len;
  }
  return revision == NULL ? NULL : revision->entity;
}

const char * gdefstor_revision_body(const GDEFSTOR_Revision * revision,
    size_t * out_len) {
  if (out_len != NULL) {
    *out_len = revision == NULL ? 0 : revision->body_len;
  }
  return revision == NULL ? NULL : revision->body;
}

GDEFSTOR_Result gdefstor_revision_append(GDEFSTOR_Write * write,
    const uint8_t * bytes, size_t len) {
  GDEFSTOR_Revision * rev = NULL;
  GDEFSTOR_Pending * row = NULL;
  uint8_t * stored = NULL;
  size_t stored_len = 0;
  char seq[32];
  int n;
  size_t count = 0;
  GDEFSTOR_Result rc;
  uint8_t * existing = NULL;
  size_t existing_len = 0;

  if (write == NULL || !write->alive) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_revision_parse(bytes, len, &write->limits, write->store->alloc,
      &rev);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  n = snprintf(seq, sizeof seq, "%llu", (unsigned long long)rev->seq);
  if (n <= 0) {
    gdefstor_revision_free(rev);
    return GDEFSTOR_ERR_INTERNAL;
  }
  row = (GDEFSTOR_Pending *)gdefstor_mem0(write->store->alloc, sizeof *row);
  if (row == NULL) {
    gdefstor_revision_free(rev);
    return GDEFSTOR_ERR_OOM;
  }
  row->kind = GDEFSTOR_KIND_REV;
  row->encoded = 1;
  rc = gdefstor_key_hex(write->store->alloc, "r:", rev->entity, rev->entity_len,
      seq, (size_t)n, 1, &row->key, &row->key_len);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
    gdefstor_revision_free(rev);
    return rc;
  }
  rc = gdefstor_lookup(write->store, write->tenant, write->tenant_len, row->key,
      row->key_len, 1, &existing, &existing_len);
  if (rc == GDEFSTOR_OK) {
    gcu_allocator_free(write->store->alloc, existing);
    gdefstor_free_pending(write->store->alloc, row);
    gdefstor_revision_free(rev);
    return GDEFSTOR_ERR_INVALID;
  }
  if (rc != GDEFSTOR_ERR_INVALID) {
    gdefstor_free_pending(write->store->alloc, row);
    gdefstor_revision_free(rev);
    return rc;
  }
  rc = gdefstor_count(write->store, write, "r:", 2, &count);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
    gdefstor_revision_free(rev);
    return rc;
  }
  if (count >= write->limits.max_revisions) {
    gdefstor_free_pending(write->store->alloc, row);
    gdefstor_revision_free(rev);
    return GDEFSTOR_ERR_LIMIT;
  }
  rc = gdefstor_revision_write(rev, &write->limits, write->store->alloc, &stored,
      &stored_len);
  gdefstor_revision_free(rev);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
    return rc;
  }
  row->bytes = stored;
  row->len = stored_len;
  rc = gdefstor_stage(write, row);
  if (rc != GDEFSTOR_OK) {
    gdefstor_free_pending(write->store->alloc, row);
  }
  return rc;
}

GDEFSTOR_Result gdefstor_revision_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * entity,
    size_t entity_len, uint64_t seq, uint8_t ** out_bytes, size_t * out_len) {
  char seq_text[32];
  char * key = NULL;
  size_t key_len = 0;
  int n;
  GDEFSTOR_Result rc;
  if (store == NULL || !gdefstor_span_ok(tenant, tenant_len,
                            store->limits.max_tenant_bytes) ||
      !gdefstor_span_ok(entity, entity_len, store->limits.max_public_id_bytes) ||
      seq == 0 || out_bytes == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = snprintf(seq_text, sizeof seq_text, "%llu", (unsigned long long)seq);
  if (n <= 0) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  rc = gdefstor_key_hex(store->alloc, "r:", entity, entity_len, seq_text,
      (size_t)n, 1, &key, &key_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_lookup(store, tenant, tenant_len, key, key_len, 0, out_bytes,
      out_len);
  gcu_allocator_free(store->alloc, key);
  return rc;
}
