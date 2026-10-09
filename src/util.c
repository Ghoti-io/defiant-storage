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
 * Allocator helpers, storage keys and the planted-defect switches.
 */

#include "internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

int gdefstor_plant_see_uncommitted = 0; /* plant:isolation */
int gdefstor_plant_keep_outbox = 0; /* plant:outbox-abort */
int gdefstor_plant_enforce_limit = 1; /* plant:limit */

const GDEFSTOR_Allocator * gdefstor_alloc(const GDEFSTOR_Allocator * in) {
  if (in != NULL) {
    return in;
  }
  return gdefstor_allocator_default();
}

void * gdefstor_mem(const GDEFSTOR_Allocator * alloc, size_t n) {
  return gcu_allocator_malloc(gdefstor_alloc(alloc), n == 0 ? 1 : n);
}

void * gdefstor_mem0(const GDEFSTOR_Allocator * alloc, size_t n) {
  return gcu_allocator_calloc(gdefstor_alloc(alloc), 1, n == 0 ? 1 : n);
}

char * gdefstor_dup(const GDEFSTOR_Allocator * alloc, const char * p,
    size_t n) {
  char * out;

  out = (char *)gdefstor_mem(alloc, n + 1);
  if (out == NULL) {
    return NULL;
  }
  if (n > 0 && p != NULL) {
    memcpy(out, p, n);
  }
  out[n] = '\0';
  return out;
}

void gdefstor_limits_resolve(const GDEFSTOR_Limits * in,
    GDEFSTOR_Limits * out) {
  if (in == NULL) {
    gdefstor_limits_default(out);
  } else {
    *out = *in;
  }
}

int gdefstor_span_ok(const char * p, size_t n, size_t cap) {
  return p != NULL && n > 0 && n <= cap;
}

static char gdefstor_hex_digit(unsigned v) {
  return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

GDEFSTOR_Result gdefstor_key_hex(const GDEFSTOR_Allocator * alloc,
    const char * prefix, const char * a, size_t a_len, const char * b,
    size_t b_len, int with_b, char ** out, size_t * out_len) {
  size_t prefix_len;
  size_t need;
  size_t i;
  char * key;

  if (prefix == NULL || a == NULL || out == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  prefix_len = strlen(prefix);
  if (a_len > SIZE_MAX / 2 || prefix_len > SIZE_MAX - a_len * 2) {
    return GDEFSTOR_ERR_LIMIT;
  }
  need = prefix_len + a_len * 2;
  if (with_b) {
    if (b == NULL) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (b_len > SIZE_MAX / 2 || need > SIZE_MAX - 1 ||
        b_len * 2 > SIZE_MAX - 1 - need) {
      return GDEFSTOR_ERR_LIMIT;
    }
    need += 1 + b_len * 2;
  }
  if (need > SIZE_MAX - 1) {
    return GDEFSTOR_ERR_LIMIT;
  }
  key = (char *)gdefstor_mem(alloc, need + 1);
  if (key == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  memcpy(key, prefix, prefix_len);
  for (i = 0; i < a_len; i++) {
    unsigned char c = (unsigned char)a[i];
    key[prefix_len + i * 2] = gdefstor_hex_digit(c >> 4);
    key[prefix_len + i * 2 + 1] = gdefstor_hex_digit(c & 0x0fu);
  }
  if (with_b) {
    size_t at = prefix_len + a_len * 2;
    key[at] = ':';
    for (i = 0; i < b_len; i++) {
      unsigned char c = (unsigned char)b[i];
      key[at + 1 + i * 2] = gdefstor_hex_digit(c >> 4);
      key[at + 1 + i * 2 + 1] = gdefstor_hex_digit(c & 0x0fu);
    }
  }
  key[need] = '\0';
  *out = key;
  *out_len = need;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_buf_append(char ** buf, size_t * len, size_t * cap,
    const GDEFSTOR_Allocator * alloc, const char * s, size_t n) {
  size_t need;
  char * next;

  if (n == 0) {
    return GDEFSTOR_OK;
  }
  if (s == NULL || buf == NULL || len == NULL || cap == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (*len > SIZE_MAX - n - 1) {
    return GDEFSTOR_ERR_LIMIT;
  }
  need = *len + n + 1;
  if (need > *cap) {
    size_t grown = *cap == 0 ? 64 : *cap;
    while (grown < need) {
      if (grown > SIZE_MAX / 2) {
        grown = need;
        break;
      }
      grown *= 2;
    }
    next = (char *)gcu_allocator_realloc(gdefstor_alloc(alloc), *buf, grown);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    *buf = next;
    *cap = grown;
  }
  memcpy(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = '\0';
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_json_escape_append(char ** buf, size_t * len,
    size_t * cap, const GDEFSTOR_Allocator * alloc, const char * s,
    size_t n) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  GDEFSTOR_Result rc;

  rc = gdefstor_buf_append(buf, len, cap, alloc, "\"", 1);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  for (i = 0; i < n; i++) {
    unsigned char c = s == NULL ? 0 : (unsigned char)s[i];
    char tmp[8];
    size_t tn = 0;
    if (c == '"' || c == '\\') {
      tmp[0] = '\\';
      tmp[1] = (char)c;
      tn = 2;
    } else if (c == '\b') {
      tmp[0] = '\\';
      tmp[1] = 'b';
      tn = 2;
    } else if (c == '\f') {
      tmp[0] = '\\';
      tmp[1] = 'f';
      tn = 2;
    } else if (c == '\n') {
      tmp[0] = '\\';
      tmp[1] = 'n';
      tn = 2;
    } else if (c == '\r') {
      tmp[0] = '\\';
      tmp[1] = 'r';
      tn = 2;
    } else if (c == '\t') {
      tmp[0] = '\\';
      tmp[1] = 't';
      tn = 2;
    } else if (c < 0x20) {
      tmp[0] = '\\';
      tmp[1] = 'u';
      tmp[2] = '0';
      tmp[3] = '0';
      tmp[4] = hex[c >> 4];
      tmp[5] = hex[c & 0x0f];
      tn = 6;
    } else {
      tmp[0] = (char)c;
      tn = 1;
    }
    rc = gdefstor_buf_append(buf, len, cap, alloc, tmp, tn);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return gdefstor_buf_append(buf, len, cap, alloc, "\"", 1);
}
