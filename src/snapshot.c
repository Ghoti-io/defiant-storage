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
 * defiant-snapshot/1: a zip. The manifest member is named "manifest". The
 * members it lists under "model" hold the entity port's truth
 * (model_snapshot.c), and a member named "ledger" is a copy of an erasure
 * ledger. No name is ever joined to a path. A reader reads version 1 and
 * refuses every other (AD-34: formats stay at 1 until the first release).
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
#define KIND_MODEL 1

typedef struct SnapMem {
  char * name;
  size_t name_len;
  uint8_t * bytes;
  size_t len;
  int kind;
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
  }
  gcu_allocator_free(alloc, mems);
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

/* ---- what a data set says the tenant needs (AD-23) ------------------------ */

static const char EDITION_PREFIX[] = GDEFSTOR_EDITION_NAME "/";

/* A positive decimal integer without leading zeros that fits in 32 bits. */
static int u32_text(const char * p, size_t n, uint32_t * out) {
  uint64_t v = 0;
  size_t i;
  if (n == 0 || n > 10 || p[0] == '0') {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') {
      return 0;
    }
    v = v * 10u + (uint64_t)(p[i] - '0');
  }
  if (v > UINT32_MAX) {
    return 0;
  }
  if (out != NULL) {
    *out = (uint32_t)v;
  }
  return 1;
}

/* `defiant-edition/<n>`. */
static int edition_ok(const char * p, size_t n) {
  size_t k = strlen(EDITION_PREFIX);
  return p != NULL && n > k && memcmp(p, EDITION_PREFIX, k) == 0 &&
         u32_text(p + k, n - k, NULL);
}

/* A module's machine name: a lowercase letter, then lowercase letters,
 * digits and hyphens. */
static int module_name_ok(const char * p, size_t n) {
  size_t i;
  if (p == NULL || n == 0 || n > GDEFSTOR_SNAPSHOT_MODULE_NAME_MAX ||
      p[0] < 'a' || p[0] > 'z') {
    return 0;
  }
  for (i = 1; i < n; i++) {
    char c = p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
      return 0;
    }
  }
  return 1;
}

static int bytes_cmp(const char * a, size_t an, const char * b, size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = n == 0 ? 0 : memcmp(a, b, n);
  if (c != 0) {
    return c;
  }
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

/* The edition and modules a caller gives an export, as the manifest will
 * hold them. */
static int about_args_ok(const char * edition, size_t edition_len,
    const GDEFSTOR_SnapshotModule * modules, size_t count) {
  size_t i;
  if ((edition == NULL && edition_len != 0) ||
      (edition_len != 0 && !edition_ok(edition, edition_len)) ||
      (modules == NULL && count != 0) ||
      count > GDEFSTOR_SNAPSHOT_MODULES_MAX) {
    return 0;
  }
  for (i = 0; i < count; i++) {
    if (!module_name_ok(modules[i].name, modules[i].name_len) ||
        modules[i].schema == 0 ||
        (i > 0 && bytes_cmp(modules[i - 1].name, modules[i - 1].name_len,
                      modules[i].name, modules[i].name_len) >= 0)) {
      return 0;
    }
  }
  return 1;
}

typedef struct AboutModule {
  char name[GDEFSTOR_SNAPSHOT_MODULE_NAME_MAX + 1];
  size_t len;
  uint32_t schema;
} AboutModule;

struct GDEFSTOR_SnapshotAbout {
  const GDEFSTOR_Allocator * alloc;
  int found;
  char edition[48];
  size_t edition_len;
  AboutModule * mods;
  size_t n;
};

/* The manifest's `"edition"` and `"modules"`: both or neither, each as an
 * export writes it. With @p out NULL, only checked. */
static GDEFSTOR_Result read_about(const GDEFSTOR_Allocator * alloc,
    const GTEXT_JSON_Value * root, GDEFSTOR_SnapshotAbout * out,
    int * out_found) {
  const GTEXT_JSON_Value * edition = gtext_json_object_get(root, "edition", 7);
  const GTEXT_JSON_Value * modules = gtext_json_object_get(root, "modules", 7);
  size_t n;
  size_t i;
  *out_found = 0;
  if (edition == NULL && modules == NULL) {
    return GDEFSTOR_OK;
  }
  if (edition == NULL || modules == NULL ||
      gtext_json_typeof(modules) != GTEXT_JSON_ARRAY) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (gtext_json_typeof(edition) != GTEXT_JSON_NULL) {
    const char * p = NULL;
    size_t len = 0;
    if (gtext_json_get_string(edition, &p, &len) != GTEXT_JSON_OK ||
        !edition_ok(p, len) || (out != NULL && len >= sizeof out->edition)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (out != NULL) {
      memcpy(out->edition, p, len);
      out->edition[len] = '\0';
      out->edition_len = len;
    }
  }
  n = gtext_json_array_size(modules);
  if (n > GDEFSTOR_SNAPSHOT_MODULES_MAX) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (out != NULL && n > 0) {
    out->mods = (AboutModule *)gdefstor_mem0(alloc, n * sizeof *out->mods);
    if (out->mods == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
  }
  {
    const char * prev = NULL;
    size_t prev_len = 0;
    for (i = 0; i < n; i++) {
      const GTEXT_JSON_Value * m = gtext_json_array_get(modules, i);
      const char * name = NULL;
      size_t name_len = 0;
      const char * lex = NULL;
      size_t lex_len = 0;
      uint32_t schema = 0;
      if (gtext_json_typeof(m) != GTEXT_JSON_OBJECT ||
          gtext_json_object_size(m) != 2 ||
          gtext_json_get_string(gtext_json_object_get(m, "name", 4), &name,
              &name_len) != GTEXT_JSON_OK ||
          !module_name_ok(name, name_len) ||
          gtext_json_get_number_lexeme(gtext_json_object_get(m, "schema", 6),
              &lex, &lex_len) != GTEXT_JSON_OK ||
          !u32_text(lex, lex_len, &schema) ||
          (prev != NULL && bytes_cmp(prev, prev_len, name, name_len) >= 0)) {
        return GDEFSTOR_ERR_CORRUPT;
      }
      prev = name;
      prev_len = name_len;
      if (out != NULL) {
        memcpy(out->mods[i].name, name, name_len);
        out->mods[i].name[name_len] = '\0';
        out->mods[i].len = name_len;
        out->mods[i].schema = schema;
        out->n = i + 1;
      }
    }
  }
  *out_found = 1;
  if (out != NULL) {
    out->found = 1;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result export_zip(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const GDEFSTOR_Limits * limits, const uint8_t * ledger,
    size_t ledger_len, int with_ledger, int with_taken, int64_t taken_ms,
    int with_about, const char * edition, size_t edition_len,
    const GDEFSTOR_SnapshotModule * modules, size_t module_count,
    uint8_t ** out_bytes, size_t * out_len) {
  GDEFSTOR_Limits cap;
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
      (with_about &&
          !about_args_ok(edition, edition_len, modules, module_count)) ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (limits == NULL) {
    cap = store->limits;
  } else {
    cap = *limits;
  }
  /* The entity part: the truth the entity port holds, as of the last
   * commit. */
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
  if (nmodel + 1 < nmodel ||
      nmodel + 1 + (with_ledger ? (size_t)1 : (size_t)0) > cap.max_members) {
    free_model(store->alloc, model, nmodel);
    return GDEFSTOR_ERR_LIMIT;
  }
  rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
      store->alloc, "{\"format\":\"" GDEFSTOR_SNAPSHOT_NAME "/1\",",
      strlen("{\"format\":\"" GDEFSTOR_SNAPSHOT_NAME "/1\","));
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
  if (rc == GDEFSTOR_OK && with_about) {
    /* What the tenant needs to come back up (AD-23): its edition and its
     * enabled modules with their stored-data schema versions. An import
     * checks them and leaves acting on them to its caller. */
    rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
        store->alloc, "\"edition\":", strlen("\"edition\":"));
    if (rc == GDEFSTOR_OK) {
      rc = edition_len == 0
               ? gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
                     store->alloc, "null", 4)
               : gdefstor_json_escape_append(&manifest, &manifest_len,
                     &manifest_cap, store->alloc, edition, edition_len);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, ",\"modules\":[", strlen(",\"modules\":["));
    }
    for (i = 0; rc == GDEFSTOR_OK && i < module_count; i++) {
      char num[24];
      int kn = snprintf(num, sizeof num, ",\"schema\":%lu}",
          (unsigned long)modules[i].schema);
      rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, i > 0 ? ",{\"name\":" : "{\"name\":",
          i > 0 ? 9 : 8);
      if (rc == GDEFSTOR_OK) {
        rc = gdefstor_json_escape_append(&manifest, &manifest_len,
            &manifest_cap, store->alloc, modules[i].name, modules[i].name_len);
      }
      if (rc == GDEFSTOR_OK) {
        rc = kn > 0 && (size_t)kn < sizeof num
                 ? gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
                       store->alloc, num, (size_t)kn)
                 : GDEFSTOR_ERR_INTERNAL;
      }
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
          store->alloc, "],", 2);
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&manifest, &manifest_len, &manifest_cap,
        store->alloc, "\"model\":[", strlen("\"model\":["));
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
  free_model(store->alloc, model, nmodel);
  return rc;
}

GDEFSTOR_Result gdefstor_snapshot_export(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    uint8_t ** out_bytes, size_t * out_len) {
  return export_zip(store, tenant, tenant_len, limits, NULL, 0, 0, 0, 0, 0,
      NULL, 0, NULL, 0, out_bytes, out_len);
}

GDEFSTOR_Result gdefstor_snapshot_export_at(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    int64_t taken_ms, uint8_t ** out_bytes, size_t * out_len) {
  if (taken_ms < 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  return export_zip(store, tenant, tenant_len, limits, NULL, 0, 0, 1, taken_ms,
      0, NULL, 0, NULL, 0, out_bytes, out_len);
}

GDEFSTOR_Result gdefstor_snapshot_export_tenant(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    int64_t taken_ms, const char * edition, size_t edition_len,
    const GDEFSTOR_SnapshotModule * modules, size_t module_count,
    uint8_t ** out_bytes, size_t * out_len) {
  if (taken_ms < 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  return export_zip(store, tenant, tenant_len, limits, NULL, 0, 0, 1, taken_ms,
      1, edition, edition_len, modules, module_count, out_bytes, out_len);
}

GDEFSTOR_Result gdefstor_snapshot_export_ledger(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * limits,
    const uint8_t * ledger, size_t ledger_len, uint8_t ** out_bytes,
    size_t * out_len) {
  return export_zip(store, tenant, tenant_len, limits, ledger, ledger_len, 1, 0,
      0, 0, NULL, 0, NULL, 0, out_bytes, out_len);
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

/* The manifest's format: GDEFSTOR_SNAPSHOT_NAME and version 1, and no
 * other (AD-34). */
static GDEFSTOR_Result check_format(const char * text, size_t len, Why * why) {
  size_t name_len = strlen(GDEFSTOR_SNAPSHOT_NAME);
  size_t i;
  char said[200];
  if (len <= name_len + 1 || memcmp(text, GDEFSTOR_SNAPSHOT_NAME, name_len) != 0 ||
      text[name_len] != '/') {
    why_set(why, "the manifest does not name the defiant-snapshot format, so "
                 "this is not a snapshot this build reads");
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  for (i = name_len + 1; i < len; i++) {
    if (text[i] < '0' || text[i] > '9') {
      return GDEFSTOR_ERR_CORRUPT;
    }
  }
  if (len == name_len + 2 && text[name_len + 1] == '1') {
    return GDEFSTOR_OK;
  }
  snprintf(said, sizeof said,
      "the snapshot is defiant-snapshot/%.*s, and this build reads "
      "defiant-snapshot/1 and no other version, so nothing was written",
      (int)(len - name_len - 1 > 24 ? 24 : len - name_len - 1),
      text + name_len + 1);
  why_set(why, said);
  return GDEFSTOR_ERR_UNSUPPORTED;
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
  const GTEXT_JSON_Value * model;
  const char * format_text = NULL;
  size_t format_len = 0;
  size_t model_n = 0;
  size_t keys;
  size_t i;
  size_t extra = 0;
  GDEFSTOR_SnapModel * sm = NULL;
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Result rc;
  int empty = 1;
  int about_found = 0;
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
  if (gtext_json_typeof(root) != GTEXT_JSON_OBJECT ||
      gtext_json_get_string(gtext_json_object_get(root, "format", 6),
          &format_text, &format_len) != GTEXT_JSON_OK) {
    rc = GDEFSTOR_ERR_CORRUPT;
    goto done;
  }
  rc = check_format(format_text, format_len, why);
  if (rc != GDEFSTOR_OK) {
    goto done;
  }
  /* The manifest holds its format, the entity part's member names and,
   * from gdefstor_snapshot_export_at(), when it was taken, and from
   * gdefstor_snapshot_export_tenant() the tenant's edition and modules as
   * well: nothing else. */
  model = gtext_json_object_get(root, "model", 5);
  rc = read_about(store->alloc, root, NULL, &about_found);
  if (rc != GDEFSTOR_OK) {
    why_set(why, "the manifest's edition or modules are not what "
                 "defiant-snapshot/1 holds there (an edition "
                 "defiant-edition/<n> or null, and modules by name, each "
                 "with a schema version of 1 or more, both or neither), so "
                 "nothing was written");
    goto done;
  }
  keys = 2 + (gtext_json_object_get(root, "taken_ms", 8) != NULL ? 1 : 0) +
         (about_found ? 2 : 0);
  if (model == NULL || gtext_json_typeof(model) != GTEXT_JSON_ARRAY ||
      gtext_json_object_size(root) != keys) {
    why_set(why, "the manifest is not what defiant-snapshot/1 holds (its "
                 "format, \"model\", \"taken_ms\", \"edition\" and "
                 "\"modules\"), so nothing was written");
    rc = GDEFSTOR_ERR_CORRUPT;
    goto done;
  }
  model_n = gtext_json_array_size(model);
  extra = find_mem(mems, count, LEDGER_NAME, strlen(LEDGER_NAME)) == NULL ? 0
                                                                         : 1;
  if (model_n + 1 + extra != count) {
    why_set(why, "the archive holds members its manifest does not list, or "
                 "lists members it does not hold, so nothing was written");
    rc = GDEFSTOR_ERR_CORRUPT;
    goto done;
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
        (name_len == strlen(LEDGER_NAME) &&
            memcmp(name, LEDGER_NAME, name_len) == 0)) {
      rc = GDEFSTOR_ERR_CORRUPT;
      goto done;
    }
    mem->kind = KIND_MODEL;
    rc = gdefstor_snap_model_member(&sm, store->alloc, &cap, mem->name,
        mem->name_len, mem->bytes, mem->len);
    if (rc == GDEFSTOR_ERR_UNSUPPORTED) {
      why_member(why, mem->name, mem->name_len,
          "is at a version this build does not read (it reads "
          "defiant-revision/1, defiant-type/1 and defiant-draft/1, and no "
          "other version), so nothing was written");
    } else if (rc == GDEFSTOR_ERR_CORRUPT) {
      why_member(why, mem->name, mem->name_len,
          "is not what defiant-snapshot/1 holds there, so nothing was "
          "written");
    }
    if (rc != GDEFSTOR_OK) {
      goto done;
    }
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
                 "backend cannot restore one, so nothing was written");
    rc = GDEFSTOR_ERR_UNSUPPORTED;
    goto done;
  }
  rc = gdefstor_write_begin(store, tenant, tenant_len, &cap, &write);
  if (rc != GDEFSTOR_OK) {
    goto done;
  }
  /* Only into an empty tenant. The entity part is written in the entity
   * transaction: open it first, so that a backend that makes a tenant's
   * tables in its first write has made them before they are read. */
  if (!gdefstor_snap_model_none(sm)) {
    rc = gdefstor_snap_model_open(write);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_snap_model_empty(store, tenant, tenant_len, &empty);
  }
  if (rc == GDEFSTOR_OK && !empty) {
    why_set(why, "the tenant already holds data, and a snapshot is imported "
                 "only into an empty tenant, so nothing was written");
    rc = GDEFSTOR_ERR_INVALID;
  }
  if (rc == GDEFSTOR_OK && !gdefstor_snap_model_none(sm)) {
    rc = gdefstor_snap_model_import(write, sm);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_write_commit(write);
      write = NULL;
    }
  }
  if (write != NULL) {
    gdefstor_write_abort(write);
    write = NULL;
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

GDEFSTOR_Result gdefstor_snapshot_about(GDEFSTOR_Store * store,
    const uint8_t * bytes, size_t len, const GDEFSTOR_Limits * limits,
    GDEFSTOR_SnapshotAbout ** out_about) {
  GDEFSTOR_Limits cap;
  SnapMem * mems = NULL;
  size_t count = 0;
  SnapMem * manifest;
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GTEXT_JSON_Value * root;
  GDEFSTOR_SnapshotAbout * about;
  int found = 0;
  GDEFSTOR_Result rc;
  if (store == NULL || out_about == NULL || (bytes == NULL && len != 0)) {
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
  about = (GDEFSTOR_SnapshotAbout *)gdefstor_mem0(store->alloc, sizeof *about);
  if (about == NULL) {
    rc = GDEFSTOR_ERR_OOM;
  } else {
    about->alloc = store->alloc;
    rc = gtext_json_typeof(root) == GTEXT_JSON_OBJECT
             ? read_about(store->alloc, root, about, &found)
             : GDEFSTOR_ERR_CORRUPT;
  }
  gtext_json_free(root);
  free_mems(store->alloc, mems, count);
  if (rc != GDEFSTOR_OK) {
    gdefstor_snapshot_about_free(about);
    return rc;
  }
  *out_about = about;
  return GDEFSTOR_OK;
}

int gdefstor_snapshot_about_found(const GDEFSTOR_SnapshotAbout * about) {
  return about != NULL && about->found;
}

void gdefstor_snapshot_about_edition(const GDEFSTOR_SnapshotAbout * about,
    const char ** out_edition, size_t * out_len) {
  if (out_edition != NULL) {
    *out_edition = about == NULL ? "" : about->edition;
  }
  if (out_len != NULL) {
    *out_len = about == NULL ? 0 : about->edition_len;
  }
}

size_t gdefstor_snapshot_about_module_count(
    const GDEFSTOR_SnapshotAbout * about) {
  return about == NULL ? 0 : about->n;
}

GDEFSTOR_Result gdefstor_snapshot_about_module(
    const GDEFSTOR_SnapshotAbout * about, size_t index,
    GDEFSTOR_SnapshotModule * out) {
  if (about == NULL || out == NULL || index >= about->n) {
    return GDEFSTOR_ERR_INVALID;
  }
  out->name = about->mods[index].name;
  out->name_len = about->mods[index].len;
  out->schema = about->mods[index].schema;
  return GDEFSTOR_OK;
}

void gdefstor_snapshot_about_free(GDEFSTOR_SnapshotAbout * about) {
  if (about == NULL) {
    return;
  }
  gcu_allocator_free(about->alloc, about->mods);
  gcu_allocator_free(about->alloc, about);
}
