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
 * Values of the entity port: what each logical type accepts, equality, order
 * and owned copies. The ranges are documentation/model.md's.
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/* 0001-01-01 and 9999-12-31, in days and in milliseconds from 1970. */
#define DATE_MIN INT64_C(-719162)
#define DATE_MAX INT64_C(2932896)
#define DATETIME_MIN INT64_C(-62135596800000)
#define DATETIME_MAX INT64_C(253402300799999)

int gdefstor_model_id_ok(const char * p, size_t n, size_t cap,
    int allow_empty) {
  size_t i;
  if (n == 0) {
    return allow_empty;
  }
  if (p == NULL || n > cap) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    char c = p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return 0;
    }
  }
  return 1;
}

int gdefstor_model_lang_ok(const char * p, size_t n) {
  size_t i;
  if (p == NULL || n == 0 || n > GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    char c = p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-')) {
      return 0;
    }
  }
  return 1;
}

int gdefstor_model_lang_cmp(const char * a, size_t an, const char * b,
    size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = n == 0 ? 0 : memcmp(a, b, n);
  if (c != 0) {
    return c < 0 ? -1 : 1;
  }
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

int gdefstor_model_name_ok(const char * p, size_t n) {
  size_t i;
  if (p == NULL || n == 0 || n > GDEFSTOR_MODEL_MAX_NAME_BYTES) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    char c = p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_')) {
      return 0;
    }
  }
  return 1;
}

/* defiant-core's machine-name rule: one dot; letters, digits and underscore;
 * a hyphen inside the local part, not first or last. */
int gdefstor_model_type_name_ok(const char * p, size_t n) {
  size_t dot = n;
  size_t i;
  if (p == NULL || n < 3 || n > 128) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    char c = p[i];
    if (c == '.') {
      if (dot != n) {
        return 0;
      }
      dot = i;
      continue;
    }
    if (c == '-' && dot != n && i > dot + 1 && i + 1 < n) {
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_')) {
      return 0;
    }
  }
  return dot > 0 && dot + 1 < n;
}

int gdefstor_model_utf8_ok(const uint8_t * p, size_t n) {
  size_t i = 0;
  if (n > 0 && p == NULL) {
    return 0;
  }
  while (i < n) {
    uint8_t c = p[i];
    uint32_t cp;
    size_t need;
    size_t k;
    if (c == 0) {
      return 0;
    }
    if (c < 0x80) {
      i += 1;
      continue;
    }
    if (c >= 0xC2 && c <= 0xDF) {
      need = 1;
      cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
      need = 2;
      cp = c & 0x0Fu;
    } else if (c >= 0xF0 && c <= 0xF4) {
      need = 3;
      cp = c & 0x07u;
    } else {
      return 0;
    }
    if (n - i <= need) {
      return 0;
    }
    for (k = 1; k <= need; k++) {
      uint8_t d = p[i + k];
      if ((d & 0xC0u) != 0x80u) {
        return 0;
      }
      cp = (cp << 6) | (d & 0x3Fu);
    }
    if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return 0;
    }
    i += need + 1;
  }
  return 1;
}

GDEFSTOR_Result gdefstor_model_value_check(const GDEFSTOR_MField * f,
    const GDEFSTOR_Value * v, size_t id_cap) {
  if (f == NULL || v == NULL || v->type != f->type) {
    return GDEFSTOR_ERR_INVALID;
  }
  if ((f->flags & GDEFSTOR_FIELD_PERSONAL) != 0) {
    /* Ciphertext, whatever the logical type: the port cannot look inside. */
    if (v->len == 0 || v->bytes == NULL) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (v->len > GDEFSTOR_MODEL_MAX_VALUE_BYTES) {
      return GDEFSTOR_ERR_LIMIT;
    }
    if (!gdefstor_model_id_ok((const char *)v->subject, v->subject_len, id_cap,
            0)) {
      return GDEFSTOR_ERR_INVALID;
    }
    return GDEFSTOR_OK;
  }
  if (v->subject_len != 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  switch (v->type) {
  case GDEFSTOR_FIELD_TEXT:
    if (v->len > GDEFSTOR_MODEL_MAX_VALUE_BYTES) {
      return GDEFSTOR_ERR_LIMIT;
    }
    return gdefstor_model_utf8_ok(v->bytes, v->len) ? GDEFSTOR_OK
                                                    : GDEFSTOR_ERR_INVALID;
  case GDEFSTOR_FIELD_REFERENCE:
    return gdefstor_model_id_ok((const char *)v->bytes, v->len, id_cap, 0)
               ? GDEFSTOR_OK
               : GDEFSTOR_ERR_INVALID;
  case GDEFSTOR_FIELD_INTEGER:
    return GDEFSTOR_OK;
  case GDEFSTOR_FIELD_DECIMAL:
    return (v->integer >= -GDEFSTOR_MODEL_MAX_DECIMAL &&
               v->integer <= GDEFSTOR_MODEL_MAX_DECIMAL)
               ? GDEFSTOR_OK
               : GDEFSTOR_ERR_INVALID;
  case GDEFSTOR_FIELD_FLOAT:
    /* NaN and the infinities compare and store differently per engine. */
    return (v->real == v->real && v->real - v->real == 0.0)
               ? GDEFSTOR_OK
               : GDEFSTOR_ERR_INVALID;
  case GDEFSTOR_FIELD_BOOLEAN:
    return (v->integer == 0 || v->integer == 1) ? GDEFSTOR_OK
                                                : GDEFSTOR_ERR_INVALID;
  case GDEFSTOR_FIELD_DATE:
    return (v->integer >= DATE_MIN && v->integer <= DATE_MAX)
               ? GDEFSTOR_OK
               : GDEFSTOR_ERR_INVALID;
  case GDEFSTOR_FIELD_DATETIME:
    return (v->integer >= DATETIME_MIN && v->integer <= DATETIME_MAX)
               ? GDEFSTOR_OK
               : GDEFSTOR_ERR_INVALID;
  default:
    return GDEFSTOR_ERR_INVALID;
  }
}

static int bytes_equal(const uint8_t * a, size_t an, const uint8_t * b,
    size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

static int bytes_compare(const uint8_t * a, size_t an, const uint8_t * b,
    size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = n == 0 ? 0 : memcmp(a, b, n);
  if (c != 0) {
    return c < 0 ? -1 : 1;
  }
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

int gdefstor_model_value_equal(const GDEFSTOR_Value * a,
    const GDEFSTOR_Value * b) {
  if (a->type != b->type) {
    return 0;
  }
  if (a->type == 0) {
    return 1;
  }
  if (!bytes_equal(a->subject, a->subject_len, b->subject, b->subject_len)) {
    return 0;
  }
  switch (a->type) {
  case GDEFSTOR_FIELD_TEXT:
  case GDEFSTOR_FIELD_REFERENCE:
    return bytes_equal(a->bytes, a->len, b->bytes, b->len);
  case GDEFSTOR_FIELD_FLOAT:
    return a->real == b->real && bytes_equal(a->bytes, a->len, b->bytes, b->len);
  default:
    return a->integer == b->integer &&
           bytes_equal(a->bytes, a->len, b->bytes, b->len);
  }
}

int gdefstor_model_value_compare(const GDEFSTOR_Value * a,
    const GDEFSTOR_Value * b) {
  if (a->type == 0 || b->type == 0) {
    return (a->type != 0) - (b->type != 0);
  }
  switch (a->type) {
  case GDEFSTOR_FIELD_TEXT:
  case GDEFSTOR_FIELD_REFERENCE:
    return bytes_compare(a->bytes, a->len, b->bytes, b->len);
  case GDEFSTOR_FIELD_FLOAT:
    return a->real < b->real ? -1 : (a->real > b->real ? 1 : 0);
  default:
    return a->integer < b->integer ? -1 : (a->integer > b->integer ? 1 : 0);
  }
}

static uint8_t * copy_bytes(const GDEFSTOR_Allocator * alloc,
    const uint8_t * p, size_t n, int * oom) {
  uint8_t * out;
  if (n == 0) {
    return NULL;
  }
  out = (uint8_t *)gdefstor_mem(alloc, n);
  if (out == NULL) {
    *oom = 1;
    return NULL;
  }
  memcpy(out, p, n);
  return out;
}

GDEFSTOR_Result gdefstor_model_value_copy(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_Value * dst, const GDEFSTOR_Value * src) {
  int oom = 0;
  GDEFSTOR_Value v = *src;
  v.bytes = copy_bytes(alloc, src->bytes, src->len, &oom);
  v.subject = copy_bytes(alloc, src->subject, src->subject_len, &oom);
  if (oom) {
    gcu_allocator_free(alloc, (void *)v.bytes);
    gcu_allocator_free(alloc, (void *)v.subject);
    return GDEFSTOR_ERR_OOM;
  }
  if (v.type == GDEFSTOR_FIELD_FLOAT && v.real == 0.0) {
    v.real = 0.0;
  }
  *dst = v;
  return GDEFSTOR_OK;
}

void gdefstor_model_value_clear(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_Value * v) {
  if (v == NULL) {
    return;
  }
  gcu_allocator_free(alloc, (void *)v->bytes);
  gcu_allocator_free(alloc, (void *)v->subject);
  memset(v, 0, sizeof *v);
}
