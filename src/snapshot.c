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
 * defiant-snapshot/2: a zip. The manifest member is named "manifest". The
 * members it lists under "entries" are named by storage keys (the key/bytes
 * rows), those under "model" hold the entity port's truth
 * (model_snapshot.c), and a member named "ledger" is a copy of an erasure
 * ledger. No name is ever joined to a path. A reader accepts
 * defiant-snapshot/1 as well, which has no "model".
 */

#include "internal.h"

#include <ghoti.io/archive/core.h>
#include <ghoti.io/archive/reader.h>
#include <ghoti.io/archive/sink.h>
#include <ghoti.io/archive/stream.h>
#include <ghoti.io/archive/writer.h>
#include <ghoti.io/archive/zip.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/json.h>
#include <stdio.h>
#include <string.h>

static const char MANIFEST_NAME[] = "manifest";
static const char LEDGER_NAME[] = "ledger";

/* SnapMem::kind of a member the manifest lists under "model". */
#define KIND_MODEL 100

typedef struct SnapMem {
  char * name;
  size_t name_len;
  uint8_t * bytes;
  size_t len;
  int kind;
  char * id;
  size_t id_len;
  char * stream;
  size_t stream_len;
  uint64_t seq;
  int have_seq;
} SnapMem;

static void free_mems(const GDEFSTOR_Allocator * alloc, SnapMem * mems,
    size_t count) {
  size_t i;
  if (mems == NULL) {
    return;
  }
  for (i = 0; i < count; i++) {
    gcu_allocator_free(alloc, mems[i].name);
    gcu_allocator_free(alloc, mems[i].bytes);
    gcu_allocator_free(alloc, mems[i].id);
    gcu_allocator_free(alloc, mems[i].stream);
  }
  gcu_allocator_free(alloc, mems);
}

static void free_keys(const GDEFSTOR_Allocator * alloc, char ** keys,
    size_t * lens, size_t count) {
  size_t i;
  for (i = 0; i < count; i++) {
    gcu_allocator_free(alloc, keys[i]);
  }
  gcu_allocator_free(alloc, keys);
  gcu_allocator_free(alloc, lens);
}

static GDEFSTOR_Result from_arc(GARC_Result gr) {
  if (gr == GARC_OK || gr == GARC_END) {
    return GDEFSTOR_OK;
  }
  if (garc_result_is_limit(gr)) {
    return GDEFSTOR_ERR_LIMIT;
  }
  if (gr == GARC_ERR_OOM) {
    return GDEFSTOR_ERR_OOM;
  }
  if (gr == GARC_ERR_FORMAT) {
    return GDEFSTOR_ERR_FORMAT;
  }
  if (gr == GARC_ERR_UNSUPPORTED) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  if (gr == GARC_ERR_IO) {
    return GDEFSTOR_ERR_IO;
  }
  return GDEFSTOR_ERR_CORRUPT;
}

static int bad_name(const char * name, size_t n) {
  size_t i;
  size_t start = 0;
  if (n == 0) {
    return 1;
  }
  for (i = 0; i <= n; i++) {
    if (i == n || name[i] == '/' || name[i] == '\\' || name[i] == '\0') {
      size_t seglen = i - start;
      if (i != n && (name[i] == '\\' || name[i] == '\0')) {
        return 1;
      }
      if (seglen == 0 || (seglen == 1 && name[start] == '.') ||
          (seglen == 2 && name[start] == '.' && name[start + 1] == '.')) {
        return 1;
      }
      start = i + 1;
    }
  }
  return 0;
}

static int ratio_over(uint64_t uncomp, uint64_t comp, uint32_t max_ratio) {
  if (uncomp == 0) {
    return 0;
  }
  if (comp == 0 || max_ratio == 0) {
    return 1;
  }
  if (comp > UINT64_MAX / (uint64_t)max_ratio) {
    return 0;
  }
  return uncomp > (uint64_t)max_ratio * comp;
}

static int prefix_kind(const char * key, size_t key_len) {
  if (key_len >= 2 && key[0] == 'r' && key[1] == ':') {
    return GDEFSTOR_KIND_REV;
  }
  if (key_len >= 2 && key[0] == 'b' && key[1] == ':') {
    return GDEFSTOR_KIND_BLOB;
  }
  if (key_len >= 2 && key[0] == 'p' && key[1] == ':') {
    return GDEFSTOR_KIND_PROJ;
  }
  if (key_len >= 2 && key[0] == 'o' && key[1] == ':') {
    return GDEFSTOR_KIND_OUT;
  }
  return 0;
}

static const char * kind_name(int kind) {
  if (kind == GDEFSTOR_KIND_REV) {
    return "revision";
  }
  if (kind == GDEFSTOR_KIND_BLOB) {
    return "blob";
  }
  if (kind == GDEFSTOR_KIND_PROJ) {
    return "projection";
  }
  if (kind == GDEFSTOR_KIND_OUT) {
    return "outbox";
  }
  return NULL;
}

static int kind_from_name(const char * text, size_t len) {
  if (len == 8 && memcmp(text, "revision", 8) == 0) {
    return GDEFSTOR_KIND_REV;
  }
  if (len == 4 && memcmp(text, "blob", 4) == 0) {
    return GDEFSTOR_KIND_BLOB;
  }
  if (len == 10 && memcmp(text, "projection", 10) == 0) {
    return GDEFSTOR_KIND_PROJ;
  }
  if (len == 6 && memcmp(text, "outbox", 6) == 0) {
    return GDEFSTOR_KIND_OUT;
  }
  return 0;
}

static GDEFSTOR_Result validate_projection(const uint8_t * bytes, size_t len,
    const GDEFSTOR_Limits * limits) {
  size_t at = 0;
  uint32_t count;
  uint32_t i;
  if (len < 4) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  count = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
          ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
  at = 4;
  if (count > limits->max_columns) {
    return GDEFSTOR_ERR_LIMIT;
  }
  for (i = 0; i < count; i++) {
    uint32_t name_len;
    uint32_t value_len;
    if (at > len || len - at < 4) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    name_len = (uint32_t)bytes[at] | ((uint32_t)bytes[at + 1] << 8) |
               ((uint32_t)bytes[at + 2] << 16) | ((uint32_t)bytes[at + 3] << 24);
    at += 4;
    if (name_len == 0 || name_len > limits->max_column_name_bytes || at > len ||
        len - at < name_len) {
      return name_len > limits->max_column_name_bytes ? GDEFSTOR_ERR_LIMIT
                                                      : GDEFSTOR_ERR_CORRUPT;
    }
    at += name_len;
    if (at > len || len - at < 4) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    value_len = (uint32_t)bytes[at] | ((uint32_t)bytes[at + 1] << 8) |
                ((uint32_t)bytes[at + 2] << 16) |
                ((uint32_t)bytes[at + 3] << 24);
    at += 4;
    if (value_len > limits->max_column_value_bytes || at > len ||
        len - at < value_len) {
      return value_len > limits->max_column_value_bytes ? GDEFSTOR_ERR_LIMIT
                                                        : GDEFSTOR_ERR_CORRUPT;
    }
    at += value_len;
  }
  if (at != len) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  return GDEFSTOR_OK;
}

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

static void free_model(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_SnapMember * v, size_t n) {
  size_t i;
  for (i = 0; v != NULL && i < n; i++) {
    gcu_allocator_free(alloc, v[i].name);
    gcu_allocator_free(alloc, v[i].bytes);
  }
  gcu_allocator_free(alloc, v);
}

static GARC_Result add_file(GARC_Writer * writer, const char * name,
    size_t name_len, const void * bytes, size_t len) {
  GARC_Member member;
  GARC_Result gr;
  memset(&member, 0, sizeof member);
  member.name = name;
  member.name_length = name_len;
  member.type = GARC_MEMBER_FILE;
  member.size = len;
  gr = garc_writer_add(writer, &member);
  if (gr == GARC_OK && len > 0) {
    gr = garc_writer_write(writer, bytes, len);
  }
  return gr;
}

static GDEFSTOR_Result export_zip(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const GDEFSTOR_Limits * limits, const uint8_t * ledger,
    size_t ledger_len, int with_ledger, int with_taken, int64_t taken_ms,
    uint8_t ** out_bytes, size_t * out_len) {
  GDEFSTOR_Limits cap;
  char ** keys = NULL;
  size_t * lens = NULL;
  size_t count = 0;
  SnapMem * mems = NULL;
  GDEFSTOR_SnapMember * model = NULL;
  size_t nmodel = 0;
  size_t i;
  char * manifest = NULL;
  size_t manifest_len = 0;
  size_t manifest_cap = 0;
  size_t total = 0;
  GARC_Sink * sink = NULL;
  GARC_Writer * writer = NULL;
  GARC_Writer_Options opt;
  const void * zip = NULL;
  size_t zip_len = 0;
  GDEFSTOR_Result rc;
  GARC_Result gr = GARC_OK;
  if (store == NULL || out_bytes == NULL || out_len == NULL ||
      (with_ledger && ledger == NULL && ledger_len != 0) ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (limits == NULL) {
    cap = store->limits;
  } else {
    cap = *limits;
  }
  /* The entity part: the truth the entity port holds, as of the last
   * commit; none on a store without the port. */
  rc = gdefstor_snap_model_export(store, tenant, tenant_len, &cap, &model,
      &nmodel);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  for (i = 0; i < nmodel; i++) {
    if (total > SIZE_MAX - model[i].len) {
      free_model(store->alloc, model, nmodel);
      return GDEFSTOR_ERR_LIMIT;
    }
    total += model[i].len;
  }
  rc = store->backend.list(store->backend.ctx, tenant, tenant_len, "", 0, &keys,
      &lens, &count);
  if (rc != GDEFSTOR_OK) {
    free_model(store->alloc, model, nmodel);
    return rc;
  }
  if (count + 1 < count || count + 1 + nmodel < nmodel ||
      count + 1 + nmodel + (with_ledger ? (size_t)1 : (size_t)0) >
          cap.max_members) {
    free_keys(store->alloc, keys, lens, count);
    free_model(store->alloc, model, nmodel);
    return GDEFSTOR_ERR_LIMIT;
  }
  mems = (SnapMem *)gdefstor_mem0(store->alloc,
      count == 0 ? sizeof *mems : count * sizeof *mems);
  if (mems == NULL) {
    free_keys(store->alloc, keys, lens, count);
    free_model(store->alloc, model, nmodel);
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; rc == GDEFSTOR_OK && i < count; i++) {
    int kind = prefix_kind(keys[i], lens[i]);
    if (kind == 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
      break;
    }
    rc = store->backend.get(store->backend.ctx, tenant, tenant_len, keys[i],
        lens[i], &mems[i].bytes, &mems[i].len);
    if (rc != GDEFSTOR_OK) {
      break;
    }
    if (mems[i].len > cap.max_member_bytes || total > SIZE_MAX - mems[i].len) {
      rc = GDEFSTOR_ERR_LIMIT;
      break;
    }
    mems[i].kind = kind;
    mems[i].name = keys[i];
    mems[i].name_len = lens[i];
    keys[i] = NULL;
    total += mems[i].len;
  }
  free_keys(store->alloc, keys, lens, count);
  keys = NULL;
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
        store->alloc, "{\"format\":\"defiant-snapshot/2\",",
        strlen("{\"format\":\"defiant-snapshot/2\","));
  }
  if (rc == GDEFSTOR_OK && with_taken) {
    /* When the set was taken, so a restore can pair it with a key set at
     * least as new (keystore.h). An import does not read it. */
    char at[48];
    int kn = snprintf(at, sizeof at, "\"taken_ms\":%lld,", (long long)taken_ms);
    rc = kn > 0 && (size_t)kn < sizeof at
             ? gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
                   store->alloc, at, (size_t)kn)
             : GDEFSTOR_ERR_INTERNAL;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
        store->alloc, "\"entries\":[", strlen("\"entries\":["));
  }
  for (i = 0; rc == GDEFSTOR_OK && i < count; i++) {
    const char * kn = kind_name(mems[i].kind);
    if (i > 0) {
      rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, ",", 1);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, "{\"key\":", strlen("{\"key\":"));
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_json_escape_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, mems[i].name, mems[i].name_len);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, ",\"kind\":", strlen(",\"kind\":"));
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_json_escape_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, kn, strlen(kn));
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, "}", 1);
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
        store->alloc, "],\"model\":[", strlen("],\"model\":["));
  }
  for (i = 0; rc == GDEFSTOR_OK && i < nmodel; i++) {
    if (i > 0) {
      rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, ",", 1);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_json_escape_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, model[i].name, model[i].name_len);
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
        store->alloc, "]}", 2);
  }
  if (rc == GDEFSTOR_OK) {
    size_t extra_bytes = with_ledger ? ledger_len : 0;
    if ((with_ledger && ledger_len > cap.max_member_bytes) ||
        manifest_len > cap.max_member_bytes ||
        total > SIZE_MAX - manifest_len ||
        total + manifest_len > SIZE_MAX - extra_bytes ||
        total + manifest_len + extra_bytes > cap.max_snapshot_bytes) {
      rc = GDEFSTOR_ERR_LIMIT;
    }
  }
  if (rc == GDEFSTOR_OK) {
    gr = garc_sink_create_memory_with_allocator(
        (const GARC_Allocator *)store->alloc, &sink);
    if (gr == GARC_OK) {
      garc_writer_options_default(&opt);
      gr = garc_writer_create_with_allocator(sink, GARC_FORMAT_ZIP, &opt,
          (const GARC_Allocator *)store->alloc, &writer);
    }
    if (gr == GARC_OK) {
      gr = add_file(writer, MANIFEST_NAME, strlen(MANIFEST_NAME), manifest,
          manifest_len);
    }
    for (i = 0; gr == GARC_OK && i < count; i++) {
      gr = add_file(writer, mems[i].name, mems[i].name_len, mems[i].bytes,
          mems[i].len);
    }
    for (i = 0; gr == GARC_OK && i < nmodel; i++) {
      gr = add_file(writer, model[i].name, model[i].name_len, model[i].bytes,
          model[i].len);
    }
    if (gr == GARC_OK && with_ledger) {
      gr = add_file(writer, LEDGER_NAME, strlen(LEDGER_NAME), ledger,
          ledger_len);
    }
    if (gr == GARC_OK) {
      gr = garc_writer_finish(writer);
    }
    if (writer != NULL) {
      garc_writer_destroy(writer);
    }
    if (gr == GARC_OK) {
      gr = garc_sink_data(sink, &zip, &zip_len);
    }
    rc = from_arc(gr);
  }
  if (rc == GDEFSTOR_OK) {
    uint8_t * copy = (uint8_t *)gdefstor_mem(store->alloc, zip_len);
    if (copy == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    } else {
      if (zip_len > 0) {
        memcpy(copy, zip, zip_len);
      }
      *out_bytes = copy;
      *out_len = zip_len;
    }
  }
  if (sink != NULL) {
    garc_sink_destroy(sink);
  }
  gcu_allocator_free(store->alloc, manifest);
  free_mems(store->alloc, mems, count);
  free_model(store->alloc, model, nmodel);
  return rc;
}

GDEFSTOR_Result gdefstor_snapshot_export(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    uint8_t ** out_bytes, size_t * out_len) {
  return export_zip(store, tenant, tenant_len, limits, NULL, 0, 0, 0, 0,
      out_bytes, out_len);
}

GDEFSTOR_Result gdefstor_snapshot_export_at(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    int64_t taken_ms, uint8_t ** out_bytes, size_t * out_len) {
  if (taken_ms < 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  return export_zip(store, tenant, tenant_len, limits, NULL, 0, 0, 1, taken_ms,
      out_bytes, out_len);
}

GDEFSTOR_Result gdefstor_snapshot_export_ledger(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    const uint8_t * ledger, size_t ledger_len, uint8_t ** out_bytes,
    size_t * out_len) {
  return export_zip(store, tenant, tenant_len, limits, ledger, ledger_len, 1, 0,
      0, out_bytes, out_len);
}

static GDEFSTOR_Result read_members(GDEFSTOR_Store * store,
    const GDEFSTOR_Limits * cap, const uint8_t * bytes, size_t len,
    SnapMem ** out_mems, size_t * out_count) {
  GARC_Stream * stream = NULL;
  GARC_Archive * archive = NULL;
  GARC_Limits alimits;
  SnapMem * mems = NULL;
  size_t count = 0;
  size_t cap_n = 0;
  GARC_Result gr;
  GDEFSTOR_Result rc;
  gr = garc_stream_create_memory_with_allocator(bytes, len,
      (const GARC_Allocator *)store->alloc, &stream);
  if (gr != GARC_OK) {
    return from_arc(gr);
  }
  garc_limits_default(&alimits);
  alimits.max_members = cap->max_members;
  alimits.max_member_bytes = cap->max_member_bytes;
  alimits.max_total_bytes = cap->max_snapshot_bytes;
  gr = garc_open_with_allocator(stream, &alimits,
      (const GARC_Allocator *)store->alloc, &archive);
  if (gr != GARC_OK) {
    garc_stream_destroy(stream);
    return from_arc(gr);
  }
  for (;;) {
    const GARC_Member * member = NULL;
    SnapMem * grown;
    uint8_t * body = NULL;
    size_t got_total = 0;
    gr = garc_next(archive, &member);
    if (gr == GARC_END) {
      break;
    }
    if (gr != GARC_OK) {
      rc = from_arc(gr);
      goto fail;
    }
    if (member->type != GARC_MEMBER_FILE ||
        bad_name(member->name, member->name_length)) {
      rc = GDEFSTOR_ERR_CORRUPT;
      goto fail;
    }
    if (member->size > cap->max_member_bytes || member->size > SIZE_MAX) {
      rc = GDEFSTOR_ERR_LIMIT;
      goto fail;
    }
    if (ratio_over(member->size, garc_zip_member_compressed_size(archive),
            cap->max_zip_ratio)) {
      rc = GDEFSTOR_ERR_LIMIT;
      goto fail;
    }
    body = (uint8_t *)gdefstor_mem(store->alloc, (size_t)member->size);
    if (body == NULL) {
      rc = GDEFSTOR_ERR_OOM;
      goto fail;
    }
    while (got_total < (size_t)member->size) {
      size_t got = 0;
      size_t chunk = (size_t)member->size - got_total;
      gr = garc_read_member(archive, body + got_total, chunk, &got);
      if (gr != GARC_OK) {
        gcu_allocator_free(store->alloc, body);
        rc = from_arc(gr);
        goto fail;
      }
      if (got == 0) {
        gcu_allocator_free(store->alloc, body);
        rc = GDEFSTOR_ERR_CORRUPT;
        goto fail;
      }
      got_total += got;
    }
    if (count == cap_n) {
      size_t ncap = cap_n == 0 ? 4 : cap_n * 2;
      grown = (SnapMem *)gcu_allocator_realloc(store->alloc, mems,
          ncap * sizeof *grown);
      if (grown == NULL) {
        gcu_allocator_free(store->alloc, body);
        rc = GDEFSTOR_ERR_OOM;
        goto fail;
      }
      memset(grown + cap_n, 0, (ncap - cap_n) * sizeof *grown);
      mems = grown;
      cap_n = ncap;
    }
    mems[count].name = gdefstor_dup(store->alloc, member->name,
        member->name_length);
    if (mems[count].name == NULL) {
      gcu_allocator_free(store->alloc, body);
      rc = GDEFSTOR_ERR_OOM;
      goto fail;
    }
    mems[count].name_len = member->name_length;
    mems[count].bytes = body;
    mems[count].len = (size_t)member->size;
    count += 1;
  }
  garc_close(archive);
  garc_stream_destroy(stream);
  *out_mems = mems;
  *out_count = count;
  return GDEFSTOR_OK;
fail:
  garc_close(archive);
  garc_stream_destroy(stream);
  free_mems(store->alloc, mems, count);
  return rc;
}

static SnapMem * find_mem(SnapMem * mems, size_t count, const char * name,
    size_t name_len) {
  size_t i;
  for (i = 0; i < count; i++) {
    if (mems[i].name_len == name_len &&
        memcmp(mems[i].name, name, name_len) == 0) {
      return &mems[i];
    }
  }
  return NULL;
}

/* Why an import refused, for gdefstor_snapshot_import_explain(): a sentence
 * the caller can show as it is. */
typedef struct Why {
  const GDEFSTOR_Allocator * alloc;
  char * text;
  size_t len;
} Why;

static void why_set(Why * w, const char * text) {
  if (w == NULL || w->text != NULL) {
    return;
  }
  w->text = gdefstor_dup(w->alloc, text, strlen(text));
  w->len = w->text == NULL ? 0 : strlen(text);
}

/* "the member <name> ..." with the name's bytes outside printable ASCII
 * shown as '?': a name is the archive's, not ours. */
static void why_member(Why * w, const char * name, size_t name_len,
    const char * what) {
  char buf[512];
  size_t at = 0;
  size_t i;
  const char * head = "the member \"";
  if (w == NULL || w->text != NULL) {
    return;
  }
  memcpy(buf, head, strlen(head));
  at = strlen(head);
  for (i = 0; i < name_len && at + 1 < 160; i++) {
    unsigned char c = (unsigned char)name[i];
    buf[at++] = c >= 0x20 && c < 0x7f && c != '"' ? (char)c : '?';
  }
  if (i < name_len) {
    buf[at++] = '.';
    buf[at++] = '.';
    buf[at++] = '.';
  }
  buf[at++] = '"';
  buf[at++] = ' ';
  for (i = 0; what[i] != '\0' && at + 1 < sizeof buf; i++) {
    buf[at++] = what[i];
  }
  buf[at] = '\0';
  why_set(w, buf);
}

/* A sentence for an error that has none of its own yet. */
static void why_default(Why * w, GDEFSTOR_Result rc) {
  if (rc == GDEFSTOR_ERR_LIMIT) {
    why_set(w, "the snapshot is over one of the import's limits (members, "
               "bytes, ratio, rows or depth), so nothing was written");
  } else if (rc == GDEFSTOR_ERR_CORRUPT || rc == GDEFSTOR_ERR_FORMAT) {
    why_set(w, "the snapshot is not a well-formed defiant-snapshot archive, "
               "so nothing was written");
  } else if (rc == GDEFSTOR_ERR_OOM) {
    why_set(w, "memory ran out during the import, so nothing was written");
  } else if (rc != GDEFSTOR_OK) {
    why_set(w, "the import failed in the store, so nothing was written");
  }
}

static GDEFSTOR_Result import_zip(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const uint8_t * bytes, size_t len,
    const GDEFSTOR_Limits * limits, Why * why) {
  GDEFSTOR_Limits cap;
  SnapMem * mems = NULL;
  size_t count = 0;
  SnapMem * manifest;
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GTEXT_JSON_Value * root = NULL;
  const GTEXT_JSON_Value * entries;
  const GTEXT_JSON_Value * model;
  const GTEXT_JSON_Value * format;
  const char * format_text = NULL;
  size_t format_len = 0;
  size_t entries_n;
  size_t model_n = 0;
  size_t i;
  size_t listed = 0;
  size_t nkv = 0;
  char ** keys = NULL;
  size_t * lens = NULL;
  size_t have = 0;
  size_t extra = 0;
  GDEFSTOR_SnapModel * sm = NULL;
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Result rc;
  unsigned version = 0;
  size_t slash;
  if (store == NULL || (bytes == NULL && len != 0) ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (limits == NULL) {
    cap = store->limits;
  } else {
    cap = *limits;
  }
  rc = read_members(store, &cap, bytes, len, &mems, &count);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  manifest = find_mem(mems, count, MANIFEST_NAME, strlen(MANIFEST_NAME));
  if (manifest == NULL) {
    free_mems(store->alloc, mems, count);
    why_set(why, "the archive has no manifest member, so it is not a "
                 "defiant-snapshot");
    return GDEFSTOR_ERR_CORRUPT;
  }
  memset(&err, 0, sizeof err);
  opt = gtext_json_parse_options_default();
  opt.max_depth = cap.max_json_depth == 0 ? 1 : cap.max_json_depth;
  opt.max_string_bytes = cap.max_member_bytes;
  opt.max_total_bytes = cap.max_member_bytes;
  opt.dupkeys = GTEXT_JSON_DUPKEY_ERROR;
  opt.allocator = (const GTEXT_Allocator *)store->alloc;
  root = gtext_json_parse((const char *)manifest->bytes, manifest->len, &opt,
      &err);
  if (root == NULL) {
    rc = from_json(err.code);
    gtext_json_error_free(&err);
    free_mems(store->alloc, mems, count);
    return rc;
  }
  gtext_json_error_free(&err);
  if (gtext_json_typeof(root) != GTEXT_JSON_OBJECT) {
    rc = GDEFSTOR_ERR_CORRUPT;
    goto done;
  }
  format = gtext_json_object_get(root, "format", 6);
  if (gtext_json_get_string(format, &format_text, &format_len) !=
      GTEXT_JSON_OK) {
    rc = GDEFSTOR_ERR_CORRUPT;
    goto done;
  }
  slash = format_len;
  for (i = 0; i < format_len; i++) {
    if (format_text[i] == '/') {
      slash = i;
    }
  }
  if (slash == format_len ||
      slash != strlen(GDEFSTOR_SNAPSHOT_NAME) ||
      memcmp(format_text, GDEFSTOR_SNAPSHOT_NAME, slash) != 0) {
    why_set(why, "the manifest does not name the defiant-snapshot format, so "
                 "this is not a snapshot this build reads");
    rc = GDEFSTOR_ERR_UNSUPPORTED;
    goto done;
  }
  for (i = slash + 1; i < format_len; i++) {
    if (format_text[i] < '0' || format_text[i] > '9') {
      rc = GDEFSTOR_ERR_CORRUPT;
      goto done;
    }
    if (version > 1000000u) {
      break;
    }
    version = version * 10u + (unsigned)(format_text[i] - '0');
  }
  if (version == 0 || version > GDEFSTOR_SNAPSHOT_VERSION) {
    char text[200];
    if (version == 0) {
      rc = GDEFSTOR_ERR_UNSUPPORTED;
      why_set(why, "the manifest names defiant-snapshot version 0, which "
                   "was never written");
      goto done;
    }
    snprintf(text, sizeof text,
        "the snapshot is defiant-snapshot/%s%u, newer than this build, which "
        "reads defiant-snapshot/1 to defiant-snapshot/%d; nothing was written",
        version > 1000000u ? ">" : "", version > 1000000u ? 1000000u : version,
        GDEFSTOR_SNAPSHOT_VERSION);
    why_set(why, text);
    rc = GDEFSTOR_ERR_UNSUPPORTED;
    goto done;
  }
  entries = gtext_json_object_get(root, "entries", 7);
  if (entries == NULL || gtext_json_typeof(entries) != GTEXT_JSON_ARRAY) {
    rc = GDEFSTOR_ERR_CORRUPT;
    goto done;
  }
  /* The entity part: version 2 lists its members under "model". */
  model = gtext_json_object_get(root, "model", 5);
  if (model != NULL &&
      (version < 2 || gtext_json_typeof(model) != GTEXT_JSON_ARRAY)) {
    rc = GDEFSTOR_ERR_CORRUPT;
    goto done;
  }
  model_n = model == NULL ? 0 : gtext_json_array_size(model);
  entries_n = gtext_json_array_size(entries);
  extra = find_mem(mems, count, LEDGER_NAME, strlen(LEDGER_NAME)) == NULL ? 0
                                                                         : 1;
  if (entries_n + model_n + 1 + extra != count) {
    why_set(why, "the archive holds members its manifest does not list, or "
                 "lists members it does not hold, so nothing was written");
    rc = GDEFSTOR_ERR_CORRUPT;
    goto done;
  }
  for (i = 0; i < entries_n; i++) {
    const GTEXT_JSON_Value * entry = gtext_json_array_get(entries, i);
    const char * key = NULL;
    size_t key_len = 0;
    const char * kind_text = NULL;
    size_t kind_len = 0;
    int kind;
    SnapMem * mem;
    if (gtext_json_typeof(entry) != GTEXT_JSON_OBJECT ||
        gtext_json_get_string(gtext_json_object_get(entry, "key", 3), &key,
            &key_len) != GTEXT_JSON_OK ||
        gtext_json_get_string(gtext_json_object_get(entry, "kind", 4), &kind_text,
            &kind_len) != GTEXT_JSON_OK) {
      rc = GDEFSTOR_ERR_CORRUPT;
      goto done;
    }
    kind = kind_from_name(kind_text, kind_len);
    mem = find_mem(mems, count, key, key_len);
    if (kind == 0 || mem == NULL || mem == manifest ||
        prefix_kind(key, key_len) != kind) {
      rc = GDEFSTOR_ERR_CORRUPT;
      goto done;
    }
    if (mem->kind != 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
      goto done;
    }
    mem->kind = kind;
    if (kind == GDEFSTOR_KIND_REV) {
      GDEFSTOR_Revision * rev = NULL;
      rc = gdefstor_revision_parse(mem->bytes, mem->len, &cap, store->alloc,
          &rev);
      if (rc == GDEFSTOR_OK) {
        char seq[32];
        char row_text[32];
        char * expect = NULL;
        size_t expect_len = 0;
        int n = snprintf(seq, sizeof seq, "%llu",
            (unsigned long long)gdefstor_revision_seq(rev));
        int nr = snprintf(row_text, sizeof row_text, "%llu",
            (unsigned long long)gdefstor_revision_row(rev));
        if (n <= 0 || nr <= 0 || gdefstor_revision_row(rev) == 0) {
          rc = GDEFSTOR_ERR_CORRUPT;
        } else {
          rc = gdefstor_key_hex(store->alloc, "r:", row_text, (size_t)nr, seq,
              (size_t)n, 1, &expect, &expect_len);
        }
        if (rc == GDEFSTOR_OK &&
            (expect_len != key_len || memcmp(expect, key, key_len) != 0)) {
          rc = GDEFSTOR_ERR_CORRUPT;
        }
        gcu_allocator_free(store->alloc, expect);
        gdefstor_revision_free(rev);
      }
    } else if (kind == GDEFSTOR_KIND_BLOB) {
      if (mem->len > cap.max_blob_bytes) {
        rc = GDEFSTOR_ERR_LIMIT;
      }
    } else if (kind == GDEFSTOR_KIND_PROJ) {
      rc = validate_projection(mem->bytes, mem->len, &cap);
    } else {
      GDEFSTOR_Outbox_Row * row = NULL;
      char * expect = NULL;
      size_t expect_len = 0;
      rc = gdefstor_decode_outbox(store->alloc, &cap, mem->bytes, mem->len,
          &row);
      if (rc == GDEFSTOR_OK) {
        rc = gdefstor_key_hex(store->alloc, "o:", row->id, row->id_len, NULL, 0,
            0, &expect, &expect_len);
      }
      if (rc == GDEFSTOR_OK &&
          (expect_len != key_len || memcmp(expect, key, key_len) != 0)) {
        rc = GDEFSTOR_ERR_CORRUPT;
      }
      if (rc == GDEFSTOR_OK) {
        size_t j;
        for (j = 0; j < count; j++) {
          if (&mems[j] == mem || !mems[j].have_seq) {
            continue;
          }
          if (mems[j].seq == row->seq &&
              mems[j].stream_len == row->stream_len &&
              memcmp(mems[j].stream, row->stream, row->stream_len) == 0) {
            rc = GDEFSTOR_ERR_CORRUPT;
            break;
          }
        }
      }
      if (rc == GDEFSTOR_OK) {
        mem->id = gdefstor_dup(store->alloc, row->id, row->id_len);
        mem->stream = gdefstor_dup(store->alloc, row->stream, row->stream_len);
        mem->id_len = row->id_len;
        mem->stream_len = row->stream_len;
        mem->seq = row->seq;
        mem->have_seq = 1;
        if (mem->id == NULL || mem->stream == NULL) {
          rc = GDEFSTOR_ERR_OOM;
        }
      }
      gcu_allocator_free(store->alloc, expect);
      gdefstor_outbox_row_free(row);
    }
    if (rc != GDEFSTOR_OK) {
      if (rc == GDEFSTOR_ERR_CORRUPT) {
        why_member(why, mem->name, mem->name_len,
            "does not hold the row its name says, so nothing was written");
      }
      goto done;
    }
    listed += 1;
    nkv += 1;
  }
  /* Each listed once, in the order an export writes them: declarations
   * before the entities of their type. */
  for (i = 0; i < model_n; i++) {
    const char * name = NULL;
    size_t name_len = 0;
    SnapMem * mem;
    if (gtext_json_get_string(gtext_json_array_get(model, i), &name,
            &name_len) != GTEXT_JSON_OK) {
      rc = GDEFSTOR_ERR_CORRUPT;
      goto done;
    }
    mem = find_mem(mems, count, name, name_len);
    if (mem == NULL || mem == manifest || mem->kind != 0 ||
        prefix_kind(name, name_len) != 0 ||
        (name_len == strlen(LEDGER_NAME) &&
            memcmp(name, LEDGER_NAME, name_len) == 0)) {
      rc = GDEFSTOR_ERR_CORRUPT;
      goto done;
    }
    mem->kind = KIND_MODEL;
    rc = gdefstor_snap_model_member(&sm, store->alloc, &cap, mem->name,
        mem->name_len, mem->bytes, mem->len);
    if (rc != GDEFSTOR_OK) {
      if (rc == GDEFSTOR_ERR_CORRUPT || rc == GDEFSTOR_ERR_UNSUPPORTED) {
        why_member(why, mem->name, mem->name_len,
            "is not what defiant-snapshot/2 holds there, so nothing was "
            "written");
      }
      goto done;
    }
    listed += 1;
  }
  if (listed + 1 + extra != count) {
    rc = GDEFSTOR_ERR_CORRUPT;
    goto done;
  }
  rc = gdefstor_snap_model_check(sm);
  if (rc != GDEFSTOR_OK) {
    if (rc == GDEFSTOR_ERR_CORRUPT) {
      why_set(why, "the snapshot's entity part does not fit together (an "
                   "entity, revision, version, draft, counter or outbox row "
                   "contradicts another), so nothing was written");
    }
    goto done;
  }
  if (!gdefstor_snap_model_none(sm) && !gdefstor_snap_model_can_hold(store)) {
    why_set(why, "this store cannot hold the snapshot's entity part: its "
                 "backend has no entity port or cannot restore one, so "
                 "nothing was written");
    rc = GDEFSTOR_ERR_UNSUPPORTED;
    goto done;
  }
  {
    size_t nrev = 0;
    size_t nblob = 0;
    size_t nproj = 0;
    size_t nout = 0;
    for (i = 0; i < count; i++) {
      if (mems[i].kind == GDEFSTOR_KIND_REV) {
        nrev += 1;
      } else if (mems[i].kind == GDEFSTOR_KIND_BLOB) {
        nblob += 1;
      } else if (mems[i].kind == GDEFSTOR_KIND_PROJ) {
        nproj += 1;
      } else if (mems[i].kind == GDEFSTOR_KIND_OUT) {
        nout += 1;
      }
    }
    if (nrev > cap.max_revisions || nblob > cap.max_blobs ||
        nproj > cap.max_projections || nout > cap.max_outbox_rows) {
      rc = GDEFSTOR_ERR_LIMIT;
      goto done;
    }
  }
  rc = gdefstor_write_begin(store, tenant, tenant_len, &cap, &write);
  if (rc != GDEFSTOR_OK) {
    goto done;
  }
  /* Only into an empty tenant: no key/bytes row, and no entity truth. */
  rc = store->backend.list(store->backend.ctx, tenant, tenant_len, "", 0, &keys,
      &lens, &have);
  if (rc != GDEFSTOR_OK) {
    gdefstor_write_abort(write);
    write = NULL;
    goto done;
  }
  free_keys(store->alloc, keys, lens, have);
  keys = NULL;
  if (have != 0) {
    gdefstor_write_abort(write);
    write = NULL;
    why_set(why, "the tenant already holds data, and a snapshot is imported "
                 "only into an empty tenant, so nothing was written");
    rc = GDEFSTOR_ERR_INVALID;
    goto done;
  }
  {
    int empty = 1;
    /* An entity part is written in the entity transaction: open it first,
     * so that a backend that makes a tenant's tables in its first write
     * has made them before they are read. */
    if (!gdefstor_snap_model_none(sm)) {
      rc = gdefstor_snap_model_open(write);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_snap_model_empty(store, tenant, tenant_len, &empty);
    }
    if (rc == GDEFSTOR_OK && !empty) {
      why_set(why, "the tenant already holds entity data, and a snapshot is "
                   "imported only into an empty tenant, so nothing was "
                   "written");
      rc = GDEFSTOR_ERR_INVALID;
    }
    if (rc != GDEFSTOR_OK) {
      gdefstor_write_abort(write);
      write = NULL;
      goto done;
    }
  }
  /* The entity part commits in the entity transaction, and the key/bytes
   * rows in a write of their own after it: a write holds one kind or the
   * other (model.h). */
  if (!gdefstor_snap_model_none(sm)) {
    rc = gdefstor_snap_model_import(write, sm);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_write_commit(write);
    } else {
      gdefstor_write_abort(write);
    }
    write = NULL;
    if (rc != GDEFSTOR_OK || nkv == 0) {
      goto done;
    }
    rc = gdefstor_write_begin(store, tenant, tenant_len, &cap, &write);
    if (rc != GDEFSTOR_OK) {
      why_set(why, "the snapshot's entity part was imported, and its "
                   "key/bytes rows could not be: the tenant holds the "
                   "entity part only");
      goto done;
    }
  }
  for (i = 0; i < count; i++) {
    GDEFSTOR_Pending * row;
    if (mems[i].kind < GDEFSTOR_KIND_REV || mems[i].kind > GDEFSTOR_KIND_OUT) {
      continue;
    }
    row = (GDEFSTOR_Pending *)gdefstor_mem0(store->alloc, sizeof *row);
    if (row == NULL) {
      rc = GDEFSTOR_ERR_OOM;
      break;
    }
    row->kind = mems[i].kind;
    row->encoded = 1;
    row->key = gdefstor_dup(store->alloc, mems[i].name, mems[i].name_len);
    row->bytes = (uint8_t *)gdefstor_mem(store->alloc, mems[i].len);
    if (row->key == NULL || row->bytes == NULL) {
      gdefstor_free_pending(store->alloc, row);
      rc = GDEFSTOR_ERR_OOM;
      break;
    }
    row->key_len = mems[i].name_len;
    if (mems[i].len > 0) {
      memcpy(row->bytes, mems[i].bytes, mems[i].len);
    }
    row->len = mems[i].len;
    if (mems[i].id != NULL) {
      row->id = gdefstor_dup(store->alloc, mems[i].id, mems[i].id_len);
      row->id_len = mems[i].id_len;
      if (row->id == NULL) {
        gdefstor_free_pending(store->alloc, row);
        rc = GDEFSTOR_ERR_OOM;
        break;
      }
    }
    rc = gdefstor_stage(write, row);
    if (rc != GDEFSTOR_OK) {
      gdefstor_free_pending(store->alloc, row);
      break;
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_write_commit(write);
    write = NULL;
  } else {
    gdefstor_write_abort(write);
    write = NULL;
  }
  if (rc != GDEFSTOR_OK && !gdefstor_snap_model_none(sm)) {
    why_set(why, "the snapshot's entity part was imported, and its key/bytes "
                 "rows could not be: the tenant holds the entity part only");
  }
done:
  if (root != NULL) {
    gtext_json_free(root);
  }
  gdefstor_snap_model_free(sm);
  free_mems(store->alloc, mems, count);
  why_default(why, rc);
  return rc;
}

GDEFSTOR_Result gdefstor_snapshot_import(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const uint8_t * bytes, size_t len,
    const GDEFSTOR_Limits * limits) {
  return import_zip(store, tenant, tenant_len, bytes, len, limits, NULL);
}

GDEFSTOR_Result gdefstor_snapshot_import_explain(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const uint8_t * bytes, size_t len,
    const GDEFSTOR_Limits * limits, char ** out_reason,
    size_t * out_reason_len) {
  Why why;
  GDEFSTOR_Result rc;
  if (out_reason == NULL || out_reason_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_reason = NULL;
  *out_reason_len = 0;
  if (store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&why, 0, sizeof why);
  why.alloc = store->alloc;
  rc = import_zip(store, tenant, tenant_len, bytes, len, limits, &why);
  if (rc == GDEFSTOR_OK) {
    gcu_allocator_free(store->alloc, why.text);
    return rc;
  }
  *out_reason = why.text;
  *out_reason_len = why.len;
  return rc;
}

GDEFSTOR_Result gdefstor_snapshot_ledger(GDEFSTOR_Store * store,
    const uint8_t * bytes, size_t len, const GDEFSTOR_Limits * limits,
    uint8_t ** out_bytes, size_t * out_len) {
  GDEFSTOR_Limits cap;
  SnapMem * mems = NULL;
  size_t count = 0;
  SnapMem * ledger;
  uint8_t * copy;
  GDEFSTOR_Result rc;
  if (store == NULL || out_bytes == NULL || out_len == NULL ||
      (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (limits == NULL) {
    cap = store->limits;
  } else {
    cap = *limits;
  }
  rc = read_members(store, &cap, bytes, len, &mems, &count);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  ledger = find_mem(mems, count, LEDGER_NAME, strlen(LEDGER_NAME));
  copy = (uint8_t *)gdefstor_mem(NULL, ledger == NULL ? 0 : ledger->len);
  if (copy == NULL) {
    free_mems(store->alloc, mems, count);
    return GDEFSTOR_ERR_OOM;
  }
  if (ledger != NULL && ledger->len > 0) {
    memcpy(copy, ledger->bytes, ledger->len);
  }
  *out_bytes = copy;
  *out_len = ledger == NULL ? 0 : ledger->len;
  free_mems(store->alloc, mems, count);
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_snapshot_taken(GDEFSTOR_Store * store,
    const uint8_t * bytes, size_t len, const GDEFSTOR_Limits * limits,
    int * out_found, int64_t * out_taken_ms) {
  GDEFSTOR_Limits cap;
  SnapMem * mems = NULL;
  size_t count = 0;
  SnapMem * manifest;
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GTEXT_JSON_Value * root;
  const GTEXT_JSON_Value * taken;
  int64_t v = 0;
  GDEFSTOR_Result rc;
  if (store == NULL || out_found == NULL || out_taken_ms == NULL ||
      (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_found = 0;
  *out_taken_ms = 0;
  if (limits == NULL) {
    cap = store->limits;
  } else {
    cap = *limits;
  }
  rc = read_members(store, &cap, bytes, len, &mems, &count);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  manifest = find_mem(mems, count, MANIFEST_NAME, strlen(MANIFEST_NAME));
  if (manifest == NULL) {
    free_mems(store->alloc, mems, count);
    return GDEFSTOR_ERR_CORRUPT;
  }
  memset(&err, 0, sizeof err);
  opt = gtext_json_parse_options_default();
  opt.max_depth = cap.max_json_depth == 0 ? 1 : cap.max_json_depth;
  opt.max_string_bytes = cap.max_member_bytes;
  opt.max_total_bytes = cap.max_member_bytes;
  opt.dupkeys = GTEXT_JSON_DUPKEY_ERROR;
  opt.allocator = (const GTEXT_Allocator *)store->alloc;
  opt.parse_int64 = 1;
  root = gtext_json_parse((const char *)manifest->bytes, manifest->len, &opt,
      &err);
  if (root == NULL) {
    rc = from_json(err.code);
    gtext_json_error_free(&err);
    free_mems(store->alloc, mems, count);
    return rc;
  }
  gtext_json_error_free(&err);
  rc = GDEFSTOR_OK;
  taken = gtext_json_typeof(root) == GTEXT_JSON_OBJECT
              ? gtext_json_object_get(root, "taken_ms", 8)
              : NULL;
  if (gtext_json_typeof(root) != GTEXT_JSON_OBJECT) {
    rc = GDEFSTOR_ERR_CORRUPT;
  } else if (taken != NULL) {
    if (gtext_json_get_i64(taken, &v) != GTEXT_JSON_OK || v < 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
    } else {
      *out_found = 1;
      *out_taken_ms = v;
    }
  }
  gtext_json_free(root);
  free_mems(store->alloc, mems, count);
  return rc;
}
