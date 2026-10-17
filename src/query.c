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
 * The query form: checking a query, the meaning of its leaves and sort keys
 * (shared with the backends that compile it), the port's own scan for a
 * backend that has no engine, and the result.
 *
 * A backend must give the answer this file gives. The scan is the reference.
 */

#include "internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/* ---- an encoded row ------------------------------------------------------ */

static GDEFSTOR_Result row_u32(const uint8_t * p, size_t len, size_t * at,
    uint32_t * out) {
  if (*at > len || len - *at < 4) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  *out = (uint32_t)p[*at] | ((uint32_t)p[*at + 1] << 8) |
         ((uint32_t)p[*at + 2] << 16) | ((uint32_t)p[*at + 3] << 24);
  *at += 4;
  return GDEFSTOR_OK;
}

/*
 * One column of an encoded row. The whole row is walked, so a damaged row is
 * refused whichever column is asked for, which keeps every engine's answer to
 * a damaged row the same. The first column of a name is the one found.
 */
static GDEFSTOR_Result row_column(const GDEFSTOR_QueryRowRef * row,
    const char * name, size_t name_len, int * found, const uint8_t ** value,
    size_t * value_len) {
  size_t at = 0;
  uint32_t count = 0;
  uint32_t i;
  GDEFSTOR_Result rc;
  *found = 0;
  *value = NULL;
  *value_len = 0;
  rc = row_u32(row->bytes, row->len, &at, &count);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  for (i = 0; i < count; i++) {
    uint32_t nlen = 0;
    uint32_t vlen = 0;
    const uint8_t * np;
    const uint8_t * vp;
    rc = row_u32(row->bytes, row->len, &at, &nlen);
    if (rc != GDEFSTOR_OK || row->len - at < nlen) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    np = row->bytes + at;
    at += nlen;
    rc = row_u32(row->bytes, row->len, &at, &vlen);
    if (rc != GDEFSTOR_OK || row->len - at < vlen) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    vp = row->bytes + at;
    at += vlen;
    if (!*found && nlen == name_len &&
        (nlen == 0 || memcmp(np, name, nlen) == 0)) {
      *found = 1;
      *value = vp;
      *value_len = vlen;
    }
  }
  return at == row->len ? GDEFSTOR_OK : GDEFSTOR_ERR_CORRUPT;
}

/* ---- a set of texts ------------------------------------------------------ */

typedef struct Texts {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_QueryText * v;
  size_t n;
  size_t cap;
  uint8_t ** owned;
  size_t nowned;
  size_t owned_cap;
  uint8_t one; /* the empty text's address */
} Texts;

static void texts_init(Texts * t, const GDEFSTOR_Allocator * alloc) {
  memset(t, 0, sizeof *t);
  t->alloc = alloc;
}

static void texts_free(Texts * t) {
  size_t i;
  for (i = 0; i < t->nowned; i++) {
    gcu_allocator_free(t->alloc, t->owned[i]);
  }
  gcu_allocator_free(t->alloc, t->owned);
  gcu_allocator_free(t->alloc, t->v);
  memset(t, 0, sizeof *t);
}

static GDEFSTOR_Result texts_add(Texts * t, const uint8_t * p, size_t n) {
  if (t->n == t->cap) {
    size_t cap = t->cap == 0 ? 4 : t->cap * 2;
    GDEFSTOR_QueryText * grown = (GDEFSTOR_QueryText *)gcu_allocator_realloc(
        t->alloc, t->v, cap * sizeof *grown);
    if (grown == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    t->v = grown;
    t->cap = cap;
  }
  t->v[t->n].bytes = p;
  t->v[t->n].len = n;
  t->n += 1;
  return GDEFSTOR_OK;
}

/* Keep @p buf (a fresh block) until the set is freed. */
static GDEFSTOR_Result texts_own(Texts * t, uint8_t * buf) {
  if (t->nowned == t->owned_cap) {
    size_t cap = t->owned_cap == 0 ? 4 : t->owned_cap * 2;
    uint8_t ** grown = (uint8_t **)gcu_allocator_realloc(t->alloc, t->owned,
        cap * sizeof *grown);
    if (grown == NULL) {
      gcu_allocator_free(t->alloc, buf);
      return GDEFSTOR_ERR_OOM;
    }
    t->owned = grown;
    t->owned_cap = cap;
  }
  t->owned[t->nowned] = buf;
  t->nowned += 1;
  return GDEFSTOR_OK;
}

/* ---- the JSON column ----------------------------------------------------- */

typedef struct Json {
  const uint8_t * p;
  size_t n;
  size_t i;
} Json;

enum { kJsonDepth = 64 };

static void json_ws(Json * j) {
  while (j->i < j->n &&
         (j->p[j->i] == ' ' || j->p[j->i] == '\t' || j->p[j->i] == '\n' ||
             j->p[j->i] == '\r')) {
    j->i += 1;
  }
}

static int json_hex4(const uint8_t * p, uint32_t * out) {
  uint32_t v = 0;
  int k;
  for (k = 0; k < 4; k++) {
    uint8_t c = p[k];
    v <<= 4;
    if (c >= '0' && c <= '9') {
      v |= (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v |= (uint32_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      v |= (uint32_t)(c - 'A' + 10);
    } else {
      return 0;
    }
  }
  *out = v;
  return 1;
}

static size_t json_utf8(uint32_t cp, uint8_t * out) {
  if (cp < 0x80u) {
    out[0] = (uint8_t)cp;
    return 1;
  }
  if (cp < 0x800u) {
    out[0] = (uint8_t)(0xc0u | (cp >> 6));
    out[1] = (uint8_t)(0x80u | (cp & 0x3fu));
    return 2;
  }
  if (cp < 0x10000u) {
    out[0] = (uint8_t)(0xe0u | (cp >> 12));
    out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
    out[2] = (uint8_t)(0x80u | (cp & 0x3fu));
    return 3;
  }
  out[0] = (uint8_t)(0xf0u | (cp >> 18));
  out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
  out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
  out[3] = (uint8_t)(0x80u | (cp & 0x3fu));
  return 4;
}

/*
 * A string, unescaped into a fresh block (never longer than the source).
 * @p out may be NULL to only step over it.
 */
static GDEFSTOR_Result json_string(const GDEFSTOR_Allocator * alloc, Json * j,
    uint8_t ** out, size_t * out_len) {
  size_t start;
  size_t w = 0;
  uint8_t * buf = NULL;
  if (j->i >= j->n || j->p[j->i] != '"') {
    return GDEFSTOR_ERR_CORRUPT;
  }
  j->i += 1;
  start = j->i;
  if (out != NULL) {
    buf = (uint8_t *)gdefstor_mem(alloc, j->n - start + 1);
    if (buf == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
  }
  for (;;) {
    uint8_t c;
    if (j->i >= j->n) {
      goto corrupt;
    }
    c = j->p[j->i];
    if (c == '"') {
      j->i += 1;
      break;
    }
    if (c < 0x20u) {
      goto corrupt;
    }
    if (c != '\\') {
      if (buf != NULL) {
        buf[w] = c;
      }
      w += 1;
      j->i += 1;
      continue;
    }
    j->i += 1;
    if (j->i >= j->n) {
      goto corrupt;
    }
    c = j->p[j->i];
    j->i += 1;
    switch (c) {
      case '"': case '\\': case '/':
        break;
      case 'b': c = '\b'; break;
      case 'f': c = '\f'; break;
      case 'n': c = '\n'; break;
      case 'r': c = '\r'; break;
      case 't': c = '\t'; break;
      case 'u': {
        uint32_t cp = 0;
        uint32_t lo = 0;
        uint8_t enc[4];
        size_t en;
        if (j->n - j->i < 4 || !json_hex4(j->p + j->i, &cp)) {
          goto corrupt;
        }
        j->i += 4;
        if (cp >= 0xdc00u && cp <= 0xdfffu) {
          goto corrupt;
        }
        if (cp >= 0xd800u && cp <= 0xdbffu) {
          if (j->n - j->i < 6 || j->p[j->i] != '\\' || j->p[j->i + 1] != 'u' ||
              !json_hex4(j->p + j->i + 2, &lo) || lo < 0xdc00u || lo > 0xdfffu) {
            goto corrupt;
          }
          j->i += 6;
          cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
        }
        en = json_utf8(cp, enc);
        if (buf != NULL) {
          memcpy(buf + w, enc, en);
        }
        w += en;
        continue;
      }
      default:
        goto corrupt;
    }
    if (buf != NULL) {
      buf[w] = c;
    }
    w += 1;
  }
  if (out != NULL) {
    *out = buf;
    *out_len = w;
  }
  return GDEFSTOR_OK;
corrupt:
  gcu_allocator_free(alloc, buf);
  return GDEFSTOR_ERR_CORRUPT;
}

static GDEFSTOR_Result json_skip(const GDEFSTOR_Allocator * alloc, Json * j,
    int depth) {
  GDEFSTOR_Result rc;
  uint8_t c;
  if (depth > kJsonDepth) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  json_ws(j);
  if (j->i >= j->n) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  c = j->p[j->i];
  if (c == '"') {
    return json_string(alloc, j, NULL, NULL);
  }
  if (c == '{' || c == '[') {
    uint8_t close = c == '{' ? '}' : ']';
    j->i += 1;
    json_ws(j);
    if (j->i < j->n && j->p[j->i] == close) {
      j->i += 1;
      return GDEFSTOR_OK;
    }
    for (;;) {
      if (c == '{') {
        rc = json_string(alloc, j, NULL, NULL);
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
        json_ws(j);
        if (j->i >= j->n || j->p[j->i] != ':') {
          return GDEFSTOR_ERR_CORRUPT;
        }
        j->i += 1;
      }
      rc = json_skip(alloc, j, depth + 1);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      json_ws(j);
      if (j->i >= j->n) {
        return GDEFSTOR_ERR_CORRUPT;
      }
      if (j->p[j->i] == ',') {
        j->i += 1;
        continue;
      }
      if (j->p[j->i] == close) {
        j->i += 1;
        return GDEFSTOR_OK;
      }
      return GDEFSTOR_ERR_CORRUPT;
    }
  }
  {
    size_t start = j->i;
    while (j->i < j->n &&
           ((j->p[j->i] >= '0' && j->p[j->i] <= '9') ||
               (j->p[j->i] >= 'a' && j->p[j->i] <= 'z') ||
               j->p[j->i] == '-' || j->p[j->i] == '+' || j->p[j->i] == '.' ||
               j->p[j->i] == 'E')) {
      j->i += 1;
    }
    return j->i > start ? GDEFSTOR_OK : GDEFSTOR_ERR_CORRUPT;
  }
}

/* The members of an object: calls @p on_member with the unescaped key. */
typedef GDEFSTOR_Result (*MemberFn)(const GDEFSTOR_Allocator * alloc, Json * j,
    const uint8_t * key, size_t key_len, void * arg, int depth);

static GDEFSTOR_Result json_object(const GDEFSTOR_Allocator * alloc, Json * j,
    MemberFn fn, void * arg, int depth) {
  GDEFSTOR_Result rc;
  json_ws(j);
  if (j->i >= j->n || j->p[j->i] != '{') {
    return GDEFSTOR_ERR_CORRUPT;
  }
  j->i += 1;
  json_ws(j);
  if (j->i < j->n && j->p[j->i] == '}') {
    j->i += 1;
    return GDEFSTOR_OK;
  }
  for (;;) {
    uint8_t * key = NULL;
    size_t key_len = 0;
    json_ws(j);
    rc = json_string(alloc, j, &key, &key_len);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    json_ws(j);
    if (j->i >= j->n || j->p[j->i] != ':') {
      gcu_allocator_free(alloc, key);
      return GDEFSTOR_ERR_CORRUPT;
    }
    j->i += 1;
    json_ws(j);
    rc = fn(alloc, j, key, key_len, arg, depth);
    gcu_allocator_free(alloc, key);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    json_ws(j);
    if (j->i >= j->n) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (j->p[j->i] == ',') {
      j->i += 1;
      continue;
    }
    if (j->p[j->i] == '}') {
      j->i += 1;
      return GDEFSTOR_OK;
    }
    return GDEFSTOR_ERR_CORRUPT;
  }
}

typedef struct PathArg {
  const char * key1;
  size_t key1_len;
  const char * key2;
  size_t key2_len;
  int found1;
  int found2;
  Texts * out;
} PathArg;

static int key_is(const uint8_t * key, size_t key_len, const char * want,
    size_t want_len) {
  return key_len == want_len && (key_len == 0 || memcmp(key, want, key_len) == 0);
}

/* The array of strings at the second key. */
static GDEFSTOR_Result json_leaf_array(const GDEFSTOR_Allocator * alloc,
    Json * j, Texts * out) {
  GDEFSTOR_Result rc;
  if (j->i >= j->n || j->p[j->i] != '[') {
    return GDEFSTOR_ERR_CORRUPT;
  }
  j->i += 1;
  json_ws(j);
  if (j->i < j->n && j->p[j->i] == ']') {
    j->i += 1;
    return GDEFSTOR_OK;
  }
  for (;;) {
    uint8_t * s = NULL;
    size_t s_len = 0;
    json_ws(j);
    rc = json_string(alloc, j, &s, &s_len);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    rc = texts_own(out, s);
    if (rc == GDEFSTOR_OK) {
      rc = texts_add(out, s, s_len);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    json_ws(j);
    if (j->i >= j->n) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (j->p[j->i] == ',') {
      j->i += 1;
      continue;
    }
    if (j->p[j->i] == ']') {
      j->i += 1;
      return GDEFSTOR_OK;
    }
    return GDEFSTOR_ERR_CORRUPT;
  }
}

static GDEFSTOR_Result member2(const GDEFSTOR_Allocator * alloc, Json * j,
    const uint8_t * key, size_t key_len, void * arg, int depth) {
  PathArg * pa = (PathArg *)arg;
  if (!pa->found2 && key_is(key, key_len, pa->key2, pa->key2_len)) {
    pa->found2 = 1;
    return json_leaf_array(alloc, j, pa->out);
  }
  return json_skip(alloc, j, depth + 1);
}

static GDEFSTOR_Result member1(const GDEFSTOR_Allocator * alloc, Json * j,
    const uint8_t * key, size_t key_len, void * arg, int depth) {
  PathArg * pa = (PathArg *)arg;
  if (!pa->found1 && key_is(key, key_len, pa->key1, pa->key1_len)) {
    pa->found1 = 1;
    return json_object(alloc, j, member2, pa, depth + 1);
  }
  return json_skip(alloc, j, depth + 1);
}

/* The array at path1 then path2 of a JSON document. */
static GDEFSTOR_Result json_path(const GDEFSTOR_Allocator * alloc,
    const uint8_t * doc, size_t len, const GDEFSTOR_QueryOperand * o,
    Texts * out) {
  Json j;
  PathArg pa;
  GDEFSTOR_Result rc;
  j.p = doc;
  j.n = len;
  j.i = 0;
  memset(&pa, 0, sizeof pa);
  pa.key1 = o->path1;
  pa.key1_len = o->path1_len;
  pa.key2 = o->path2;
  pa.key2_len = o->path2_len;
  pa.out = out;
  rc = json_object(alloc, &j, member1, &pa, 1);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  json_ws(&j);
  return j.i == j.n ? GDEFSTOR_OK : GDEFSTOR_ERR_CORRUPT;
}

/* ---- an operand ---------------------------------------------------------- */

static GDEFSTOR_Result operand_texts(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_QueryOperand * o, const GDEFSTOR_QueryEnv * env,
    const GDEFSTOR_QueryRowRef * join, Texts * t) {
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  size_t i;
  if (o->kind == GDEFSTOR_QUERY_CONST) {
    for (i = 0; i < o->text_count && rc == GDEFSTOR_OK; i++) {
      rc = texts_add(t, o->texts[i].bytes, o->texts[i].len);
    }
    return rc;
  }
  if (o->kind == GDEFSTOR_QUERY_ENTITY) {
    return texts_add(t, env->entity, env->entity_len);
  }
  if (o->kind != GDEFSTOR_QUERY_COLUMN) {
    return GDEFSTOR_ERR_INVALID;
  }
  {
    const GDEFSTOR_QueryRowRef * row = o->join_type != NULL ? join : &env->main;
    int found = 0;
    const uint8_t * val = NULL;
    size_t vlen = 0;
    if (row != NULL && row->bytes != NULL) {
      rc = row_column(row, o->column, o->column_len, &found, &val, &vlen);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      if (found && o->path_depth == 2) {
        rc = json_path(alloc, val, vlen, o, t);
      } else if (found) {
        rc = texts_add(t, val, vlen);
      }
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  if (t->n == 0 && (o->flags & GDEFSTOR_QUERY_EMPTY_IF_NONE) != 0) {
    rc = texts_add(t, &t->one, 0);
  }
  return rc;
}

/* An integer: -?[0-9]{1,18}. */
static int text_int(const uint8_t * p, size_t n, int64_t * out) {
  size_t i = 0;
  int64_t v = 0;
  int neg = 0;
  if (n > 0 && p[0] == '-') {
    neg = 1;
    i = 1;
  }
  if (n - i < 1 || n - i > 18) {
    return 0;
  }
  for (; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') {
      return 0;
    }
    v = v * 10 + (p[i] - '0');
  }
  *out = neg ? -v : v;
  return 1;
}

static int text_same(const GDEFSTOR_QueryText * a, const GDEFSTOR_QueryText * b) {
  return a->len == b->len && (a->len == 0 || memcmp(a->bytes, b->bytes, a->len) == 0);
}

static int any_same(const Texts * l, const Texts * r) {
  size_t i;
  size_t j;
  for (i = 0; i < l->n; i++) {
    for (j = 0; j < r->n; j++) {
      if (text_same(&l->v[i], &r->v[j])) {
        return 1;
      }
    }
  }
  return 0;
}

/* IN is its own loop on purpose: it is the one a constant list reaches. */
static int any_listed(const Texts * l, const Texts * list) {
  size_t i;
  size_t k;
  for (k = 0; k < list->n; k++) {
    for (i = 0; i < l->n; i++) {
      if (text_same(&l->v[i], &list->v[k])) {
        return 1;
      }
    }
  }
  return 0;
}

static int ordered(int op, int64_t x, int64_t y) {
  switch (op) {
    case GDEFSTOR_QUERY_LT:
      return x < y;
    case GDEFSTOR_QUERY_LE:
      return x <= y;
    case GDEFSTOR_QUERY_GT:
      return x > y;
    case GDEFSTOR_QUERY_GE:
      return x >= y;
    default:
      return 0;
  }
}

static int any_ordered(int op, const Texts * l, const Texts * r) {
  size_t i;
  size_t j;
  for (i = 0; i < l->n; i++) {
    int64_t x;
    if (!text_int(l->v[i].bytes, l->v[i].len, &x)) {
      continue;
    }
    for (j = 0; j < r->n; j++) {
      int64_t y;
      if (text_int(r->v[j].bytes, r->v[j].len, &y) && ordered(op, x, y)) {
        return 1;
      }
    }
  }
  return 0;
}

GDEFSTOR_Result gdefstor_query_leaf(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_QueryNode * node, const GDEFSTOR_QueryEnv * env,
    int * out_holds) {
  Texts l;
  Texts r;
  GDEFSTOR_Result rc;
  int holds = 0;
  if (node == NULL || env == NULL || out_holds == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  alloc = gdefstor_alloc(alloc);
  texts_init(&l, alloc);
  texts_init(&r, alloc);
  rc = operand_texts(alloc, &node->left, env, &env->left_join, &l);
  if (rc == GDEFSTOR_OK) {
    rc = operand_texts(alloc, &node->right, env, &env->right_join, &r);
  }
  if (rc == GDEFSTOR_OK) {
    switch (node->op) {
      case GDEFSTOR_QUERY_EQ:
        holds = any_same(&l, &r);
        break;
      case GDEFSTOR_QUERY_NE:
        holds = !any_same(&l, &r);
        break;
      case GDEFSTOR_QUERY_IN:
        holds = any_listed(&l, &r);
        break;
      case GDEFSTOR_QUERY_LT:
      case GDEFSTOR_QUERY_LE:
      case GDEFSTOR_QUERY_GT:
      case GDEFSTOR_QUERY_GE:
        holds = any_ordered(node->op, &l, &r);
        break;
      default:
        rc = GDEFSTOR_ERR_INVALID;
        break;
    }
  }
  texts_free(&l);
  texts_free(&r);
  *out_holds = rc == GDEFSTOR_OK ? holds : 0;
  return rc;
}

/* ---- keys ---------------------------------------------------------------- */

GDEFSTOR_Result gdefstor_query_key_make(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_QuerySort * sort, const GDEFSTOR_QueryEnv * env,
    GDEFSTOR_QueryKey * out) {
  Texts t;
  GDEFSTOR_Result rc;
  if (sort == NULL || env == NULL || out == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  alloc = gdefstor_alloc(alloc);
  memset(out, 0, sizeof *out);
  texts_init(&t, alloc);
  rc = operand_texts(alloc, &sort->key, env, NULL, &t);
  if (rc == GDEFSTOR_OK && t.n > 0) {
    if (sort->numeric) {
      int64_t v = 0;
      if (text_int(t.v[0].bytes, t.v[0].len, &v)) {
        out->present = 1;
        out->is_int = 1;
        out->value = v;
      }
    } else {
      out->bytes = (uint8_t *)gdefstor_mem(alloc, t.v[0].len);
      if (out->bytes == NULL) {
        rc = GDEFSTOR_ERR_OOM;
      } else {
        if (t.v[0].len > 0) {
          memcpy(out->bytes, t.v[0].bytes, t.v[0].len);
        }
        out->len = t.v[0].len;
        out->present = 1;
      }
    }
  }
  texts_free(&t);
  return rc;
}

int gdefstor_query_key_compare(const GDEFSTOR_QuerySort * sort,
    const GDEFSTOR_QueryKey * a, const GDEFSTOR_QueryKey * b) {
  int c = 0;
  if (!a->present || !b->present) {
    c = (a->present ? 1 : 0) - (b->present ? 1 : 0);
  } else if (sort->numeric) {
    c = a->value < b->value ? -1 : (a->value > b->value ? 1 : 0);
  } else {
    size_t n = a->len < b->len ? a->len : b->len;
    c = n == 0 ? 0 : memcmp(a->bytes, b->bytes, n);
    if (c == 0) {
      c = a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
    }
  }
  return sort->descending ? -c : c;
}

void gdefstor_query_key_free(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_QueryKey * key) {
  if (key == NULL) {
    return;
  }
  gcu_allocator_free(gdefstor_alloc(alloc), key->bytes);
  memset(key, 0, sizeof *key);
}

GDEFSTOR_Result gdefstor_query_first_text(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_QueryOperand * operand, const GDEFSTOR_QueryEnv * env,
    int * out_present, uint8_t ** out_bytes, size_t * out_len) {
  Texts t;
  GDEFSTOR_Result rc;
  if (operand == NULL || env == NULL || out_present == NULL ||
      out_bytes == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  alloc = gdefstor_alloc(alloc);
  *out_present = 0;
  *out_bytes = NULL;
  *out_len = 0;
  texts_init(&t, alloc);
  rc = operand_texts(alloc, operand, env, NULL, &t);
  if (rc == GDEFSTOR_OK && t.n > 0) {
    uint8_t * copy = (uint8_t *)gdefstor_mem(alloc, t.v[0].len);
    if (copy == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    } else {
      if (t.v[0].len > 0) {
        memcpy(copy, t.v[0].bytes, t.v[0].len);
      }
      *out_present = 1;
      *out_bytes = copy;
      *out_len = t.v[0].len;
    }
  }
  texts_free(&t);
  return rc;
}

/* ---- keys of rows -------------------------------------------------------- */

GDEFSTOR_Result gdefstor_projection_key_prefix(const GDEFSTOR_Allocator * alloc,
    const char * type, size_t type_len, char ** out_prefix, size_t * out_len) {
  if (type == NULL || type_len == 0 || out_prefix == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  /* "p:<hex type>:", with the colon, so that a type whose hex is the start of
   * another's is not matched by it. */
  return gdefstor_key_hex(gdefstor_alloc(alloc), "p:", type, type_len, "", 0, 1,
      out_prefix, out_len);
}

GDEFSTOR_Result gdefstor_projection_key(const GDEFSTOR_Allocator * alloc,
    const char * type, size_t type_len, const char * entity, size_t entity_len,
    char ** out_key, size_t * out_len) {
  if (type == NULL || type_len == 0 || entity == NULL || entity_len == 0 ||
      out_key == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  return gdefstor_key_hex(gdefstor_alloc(alloc), "p:", type, type_len, entity,
      entity_len, 1, out_key, out_len);
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

/* The key is "p:<hex type>:<hex entity>"; the prefix is "p:<hex type>:". */
GDEFSTOR_Result gdefstor_projection_key_entity(const GDEFSTOR_Allocator * alloc,
    const char * key, size_t key_len, size_t prefix_len, char ** out_entity,
    size_t * out_len) {
  size_t hex_at;
  size_t n;
  size_t i;
  char * entity;
  if (key == NULL || out_entity == NULL || out_len == NULL ||
      prefix_len >= key_len) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  hex_at = prefix_len;
  n = key_len - hex_at;
  if (n == 0 || n % 2 != 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  entity = (char *)gdefstor_mem(gdefstor_alloc(alloc), n / 2 + 1);
  if (entity == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < n / 2; i++) {
    int hi = hex_val(key[hex_at + i * 2]);
    int lo = hex_val(key[hex_at + i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      gcu_allocator_free(gdefstor_alloc(alloc), entity);
      return GDEFSTOR_ERR_CORRUPT;
    }
    entity[i] = (char)((hi << 4) | lo);
  }
  entity[n / 2] = '\0';
  *out_entity = entity;
  *out_len = n / 2;
  return GDEFSTOR_OK;
}

/* ---- checking a query ---------------------------------------------------- */

typedef struct Check {
  const GDEFSTOR_Limits * limits;
  size_t nodes;
} Check;

static GDEFSTOR_Result check_operand(const Check * c,
    const GDEFSTOR_QueryOperand * o, int allow_join, int allow_const) {
  size_t i;
  if (o->kind == GDEFSTOR_QUERY_CONST) {
    if (!allow_const) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (o->text_count > GDEFSTOR_QUERY_MAX_TEXTS) {
      return GDEFSTOR_ERR_LIMIT;
    }
    if (o->texts == NULL && o->text_count != 0) {
      return GDEFSTOR_ERR_INVALID;
    }
    for (i = 0; i < o->text_count; i++) {
      if (o->texts[i].len > GDEFSTOR_QUERY_MAX_TEXT_BYTES) {
        return GDEFSTOR_ERR_LIMIT;
      }
      if (o->texts[i].bytes == NULL && o->texts[i].len != 0) {
        return GDEFSTOR_ERR_INVALID;
      }
    }
    return GDEFSTOR_OK;
  }
  if (o->kind == GDEFSTOR_QUERY_ENTITY) {
    return GDEFSTOR_OK;
  }
  if (o->kind != GDEFSTOR_QUERY_COLUMN) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (!gdefstor_span_ok(o->column, o->column_len,
          c->limits->max_column_name_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (o->join_type != NULL) {
    if (!allow_join ||
        !gdefstor_span_ok(o->join_type, o->join_type_len,
            c->limits->max_type_bytes)) {
      return GDEFSTOR_ERR_INVALID;
    }
  }
  if (o->path_depth != 0 && o->path_depth != 2) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (o->path_depth == 2 &&
      (o->path1 == NULL || o->path2 == NULL ||
          o->path1_len > c->limits->max_column_name_bytes ||
          o->path2_len > c->limits->max_column_name_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result check_node(Check * c, const GDEFSTOR_QueryNode * n,
    size_t depth) {
  size_t i;
  GDEFSTOR_Result rc;
  if (n == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (depth > GDEFSTOR_QUERY_MAX_DEPTH) {
    return GDEFSTOR_ERR_LIMIT;
  }
  c->nodes += 1;
  if (c->nodes > GDEFSTOR_QUERY_MAX_NODES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  switch (n->op) {
    case GDEFSTOR_QUERY_TRUE:
    case GDEFSTOR_QUERY_FALSE:
      return n->child_count == 0 ? GDEFSTOR_OK : GDEFSTOR_ERR_INVALID;
    case GDEFSTOR_QUERY_AND:
    case GDEFSTOR_QUERY_OR:
    case GDEFSTOR_QUERY_NOT:
      if ((n->op == GDEFSTOR_QUERY_NOT && n->child_count != 1) ||
          (n->child_count > 0 && n->children == NULL)) {
        return GDEFSTOR_ERR_INVALID;
      }
      if (n->child_count > GDEFSTOR_QUERY_MAX_NODES) {
        return GDEFSTOR_ERR_LIMIT;
      }
      for (i = 0; i < n->child_count; i++) {
        rc = check_node(c, n->children[i], depth + 1);
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
      }
      return GDEFSTOR_OK;
    case GDEFSTOR_QUERY_EQ:
    case GDEFSTOR_QUERY_NE:
    case GDEFSTOR_QUERY_IN:
    case GDEFSTOR_QUERY_LT:
    case GDEFSTOR_QUERY_LE:
    case GDEFSTOR_QUERY_GT:
    case GDEFSTOR_QUERY_GE:
      if (n->child_count != 0) {
        return GDEFSTOR_ERR_INVALID;
      }
      if (n->op == GDEFSTOR_QUERY_IN &&
          n->right.kind != GDEFSTOR_QUERY_CONST) {
        return GDEFSTOR_ERR_INVALID;
      }
      rc = check_operand(c, &n->left, 1, 1);
      if (rc == GDEFSTOR_OK) {
        rc = check_operand(c, &n->right, 1, 1);
      }
      return rc;
    default:
      return GDEFSTOR_ERR_INVALID;
  }
}

static GDEFSTOR_Result check_where(const GDEFSTOR_Limits * limits,
    const GDEFSTOR_QueryNode * where) {
  Check c;
  c.limits = limits;
  c.nodes = 0;
  if (where == NULL) {
    return GDEFSTOR_OK;
  }
  return check_node(&c, where, 1);
}

static GDEFSTOR_Result check_query(const GDEFSTOR_Limits * limits,
    const GDEFSTOR_Query * q) {
  Check c;
  size_t i;
  GDEFSTOR_Result rc;
  if (q == NULL || q->size < sizeof(GDEFSTOR_Query)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if ((q->sorts == NULL && q->sort_count != 0) ||
      (q->select == NULL && q->select_count != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (q->sort_count > GDEFSTOR_QUERY_MAX_SORTS ||
      q->select_count > GDEFSTOR_QUERY_MAX_SELECT) {
    return GDEFSTOR_ERR_LIMIT;
  }
  rc = check_where(limits, q->where);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  c.limits = limits;
  c.nodes = 0;
  for (i = 0; i < q->sort_count; i++) {
    if (q->sorts[i].key.kind == GDEFSTOR_QUERY_CONST) {
      return GDEFSTOR_ERR_INVALID;
    }
    rc = check_operand(&c, &q->sorts[i].key, 0, 0);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (i = 0; i < q->select_count; i++) {
    rc = check_operand(&c, &q->select[i], 0, 0);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

/* ---- the scan ------------------------------------------------------------ */

typedef struct JoinSlot {
  const char * type;
  size_t type_len;
  int loaded;
  uint8_t * bytes;
  size_t len;
} JoinSlot;

enum { kJoinSlots = 16 };

typedef struct Scan {
  GDEFSTOR_Store * store;
  const char * tenant;
  size_t tenant_len;
  GDEFSTOR_QueryEnv env;
  JoinSlot slots[kJoinSlots];
  size_t nslots;
  char * entity;
  size_t entity_len;
} Scan;

static void scan_row_reset(Scan * s) {
  size_t i;
  for (i = 0; i < s->nslots; i++) {
    gcu_allocator_free(s->store->alloc, s->slots[i].bytes);
  }
  s->nslots = 0;
}

/* The row of @p type with the current entity, or absent. */
static GDEFSTOR_Result scan_join(Scan * s, const char * type, size_t type_len,
    GDEFSTOR_QueryRowRef * ref) {
  size_t i;
  JoinSlot * slot = NULL;
  for (i = 0; i < s->nslots; i++) {
    if (s->slots[i].type_len == type_len &&
        memcmp(s->slots[i].type, type, type_len) == 0) {
      slot = &s->slots[i];
      break;
    }
  }
  if (slot == NULL) {
    char * key = NULL;
    size_t key_len = 0;
    GDEFSTOR_Result rc;
    if (s->nslots >= kJoinSlots) {
      return GDEFSTOR_ERR_LIMIT;
    }
    slot = &s->slots[s->nslots];
    memset(slot, 0, sizeof *slot);
    slot->type = type;
    slot->type_len = type_len;
    s->nslots += 1;
    rc = gdefstor_projection_key(s->store->alloc, type, type_len, s->entity,
        s->entity_len, &key, &key_len);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    rc = s->store->backend.get(s->store->backend.ctx, s->tenant, s->tenant_len,
        key, key_len, &slot->bytes, &slot->len);
    gcu_allocator_free(s->store->alloc, key);
    if (rc == GDEFSTOR_ERR_INVALID) {
      slot->bytes = NULL;
      slot->len = 0;
    } else if (rc != GDEFSTOR_OK) {
      return rc;
    }
    slot->loaded = 1;
  }
  ref->bytes = slot->bytes;
  ref->len = slot->len;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result scan_eval(Scan * s, const GDEFSTOR_QueryNode * n,
    int * out) {
  size_t i;
  GDEFSTOR_Result rc;
  int r;
  switch (n->op) {
    case GDEFSTOR_QUERY_TRUE:
      *out = 1;
      return GDEFSTOR_OK;
    case GDEFSTOR_QUERY_FALSE:
      *out = 0;
      return GDEFSTOR_OK;
    case GDEFSTOR_QUERY_AND:
      for (i = 0; i < n->child_count; i++) {
        rc = scan_eval(s, n->children[i], &r);
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
        if (!r) {
          *out = 0;
          return GDEFSTOR_OK;
        }
      }
      *out = 1;
      return GDEFSTOR_OK;
    case GDEFSTOR_QUERY_OR:
      for (i = 0; i < n->child_count; i++) {
        rc = scan_eval(s, n->children[i], &r);
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
        if (r) {
          *out = 1;
          return GDEFSTOR_OK;
        }
      }
      *out = 0;
      return GDEFSTOR_OK;
    case GDEFSTOR_QUERY_NOT:
      rc = scan_eval(s, n->children[0], &r);
      *out = rc == GDEFSTOR_OK ? !r : 0;
      return rc;
    default:
      break;
  }
  s->env.left_join.bytes = NULL;
  s->env.left_join.len = 0;
  s->env.right_join.bytes = NULL;
  s->env.right_join.len = 0;
  if (n->left.kind == GDEFSTOR_QUERY_COLUMN && n->left.join_type != NULL) {
    rc = scan_join(s, n->left.join_type, n->left.join_type_len,
        &s->env.left_join);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  if (n->right.kind == GDEFSTOR_QUERY_COLUMN && n->right.join_type != NULL) {
    rc = scan_join(s, n->right.join_type, n->right.join_type_len,
        &s->env.right_join);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return gdefstor_query_leaf(s->store->alloc, n, &s->env, out);
}

typedef struct Hit {
  char * entity;
  size_t entity_len;
  uint8_t * row;
  size_t row_len;
  GDEFSTOR_QueryKey * keys;
} Hit;

typedef struct Order {
  GDEFSTOR_QuerySort sorts[GDEFSTOR_QUERY_MAX_SORTS + 2];
  size_t n;
} Order;

static int hit_cmp(const Order * o, const Hit * a, const Hit * b) {
  size_t i;
  for (i = 0; i < o->n; i++) {
    int c = gdefstor_query_key_compare(&o->sorts[i], &a->keys[i], &b->keys[i]);
    if (c != 0) {
      return c;
    }
  }
  return 0;
}

/* A merge sort: stable, and needs no comparator context from the C library. */
static GDEFSTOR_Result hits_sort(const GDEFSTOR_Allocator * alloc,
    const Order * o, Hit * hits, size_t n) {
  Hit * tmp;
  size_t width;
  if (n < 2) {
    return GDEFSTOR_OK;
  }
  tmp = (Hit *)gdefstor_mem(alloc, n * sizeof *tmp);
  if (tmp == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (width = 1; width < n; width *= 2) {
    size_t lo;
    for (lo = 0; lo < n; lo += 2 * width) {
      size_t mid = lo + width < n ? lo + width : n;
      size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t a = lo;
      size_t b = mid;
      size_t k = lo;
      while (a < mid && b < hi) {
        if (hit_cmp(o, &hits[b], &hits[a]) < 0) {
          tmp[k++] = hits[b++];
        } else {
          tmp[k++] = hits[a++];
        }
      }
      while (a < mid) {
        tmp[k++] = hits[a++];
      }
      while (b < hi) {
        tmp[k++] = hits[b++];
      }
    }
    memcpy(hits, tmp, n * sizeof *hits);
  }
  gcu_allocator_free(alloc, tmp);
  return GDEFSTOR_OK;
}

static void hit_free(const GDEFSTOR_Allocator * alloc, const Order * o,
    Hit * h) {
  size_t i;
  if (h->keys != NULL) {
    for (i = 0; i < o->n; i++) {
      gdefstor_query_key_free(alloc, &h->keys[i]);
    }
  }
  gcu_allocator_free(alloc, h->keys);
  gcu_allocator_free(alloc, h->entity);
  gcu_allocator_free(alloc, h->row);
}

static void order_build(Order * o, const GDEFSTOR_Query * q) {
  size_t i;
  GDEFSTOR_QuerySort tie;
  memset(o, 0, sizeof *o);
  for (i = 0; i < q->sort_count; i++) {
    o->sorts[o->n++] = q->sorts[i];
  }
  /* The entity argument, ascending, is the final tiebreak. */
  memset(&tie, 0, sizeof tie);
  tie.key.kind = GDEFSTOR_QUERY_ENTITY;
  if (q->entity_numeric) {
    tie.numeric = 1;
    o->sorts[o->n++] = tie;
    tie.numeric = 0;
  }
  o->sorts[o->n++] = tie;
}

static GDEFSTOR_Result scan_rows(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const GDEFSTOR_QueryPlan * plan,
    GDEFSTOR_QueryRows * out) {
  const GDEFSTOR_Allocator * alloc = store->alloc;
  char ** keys = NULL;
  size_t * lens = NULL;
  size_t nkeys = 0;
  size_t k;
  size_t i;
  Hit * hits = NULL;
  size_t nhits = 0;
  Order order;
  Scan s;
  GDEFSTOR_Result rc;
  uint64_t total = 0;
  uint64_t first;
  size_t take;
  memset(out, 0, sizeof *out);
  memset(&s, 0, sizeof s);
  s.store = store;
  s.tenant = tenant;
  s.tenant_len = tenant_len;
  order_build(&order, plan->query);
  rc = store->backend.list(store->backend.ctx, tenant, tenant_len, plan->prefix,
      plan->prefix_len, &keys, &lens, &nkeys);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (nkeys > 0) {
    hits = (Hit *)gdefstor_mem0(alloc, nkeys * sizeof *hits);
    if (hits == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  for (k = 0; rc == GDEFSTOR_OK && k < nkeys; k++) {
    uint8_t * bytes = NULL;
    size_t blen = 0;
    int match = 1;
    /* The listing is by prefix; a key with nothing after it is not a row. */
    if (lens[k] <= plan->prefix_len) {
      continue;
    }
    rc = gdefstor_projection_key_entity(alloc, keys[k], lens[k],
        plan->prefix_len, &s.entity, &s.entity_len);
    if (rc != GDEFSTOR_OK) {
      break;
    }
    rc = store->backend.get(store->backend.ctx, tenant, tenant_len, keys[k],
        lens[k], &bytes, &blen);
    if (rc == GDEFSTOR_ERR_INVALID) {
      /* Gone between the listing and the read. */
      gcu_allocator_free(alloc, s.entity);
      s.entity = NULL;
      rc = GDEFSTOR_OK;
      continue;
    }
    if (rc != GDEFSTOR_OK) {
      gcu_allocator_free(alloc, s.entity);
      s.entity = NULL;
      break;
    }
    s.env.main.bytes = bytes;
    s.env.main.len = blen;
    s.env.entity = (const uint8_t *)s.entity;
    s.env.entity_len = s.entity_len;
    scan_row_reset(&s);
    if (plan->query->where != NULL) {
      rc = scan_eval(&s, plan->query->where, &match);
    }
    if (rc == GDEFSTOR_OK && match) {
      total += 1;
      if (!plan->count_only) {
        Hit * h = &hits[nhits];
        h->keys = (GDEFSTOR_QueryKey *)gdefstor_mem0(alloc,
            order.n * sizeof *h->keys);
        if (h->keys == NULL) {
          rc = GDEFSTOR_ERR_OOM;
        }
        for (i = 0; rc == GDEFSTOR_OK && i < order.n; i++) {
          rc = gdefstor_query_key_make(alloc, &order.sorts[i], &s.env,
              &h->keys[i]);
        }
        if (rc == GDEFSTOR_OK) {
          h->entity = s.entity;
          h->entity_len = s.entity_len;
          h->row = bytes;
          h->row_len = blen;
          s.entity = NULL;
          bytes = NULL;
          nhits += 1;
        } else {
          hit_free(alloc, &order, h);
          memset(h, 0, sizeof *h);
        }
      }
    }
    gcu_allocator_free(alloc, bytes);
    gcu_allocator_free(alloc, s.entity);
    s.entity = NULL;
    scan_row_reset(&s);
  }
  for (k = 0; k < nkeys; k++) {
    gcu_allocator_free(alloc, keys[k]);
  }
  gcu_allocator_free(alloc, keys);
  gcu_allocator_free(alloc, lens);
  if (rc == GDEFSTOR_OK && !plan->count_only) {
    rc = hits_sort(alloc, &order, hits, nhits);
  }
  if (rc == GDEFSTOR_OK) {
    out->total = total;
    first = plan->query->offset;
    if (plan->count_only || first >= nhits) {
      take = 0;
    } else {
      uint64_t room = (uint64_t)nhits - first;
      take = (size_t)(plan->limit < room ? plan->limit : room);
    }
    if (take > 0) {
      out->entities = (char **)gdefstor_mem0(alloc, take * sizeof *out->entities);
      out->entity_lens = (size_t *)gdefstor_mem0(alloc,
          take * sizeof *out->entity_lens);
      out->rows = (uint8_t **)gdefstor_mem0(alloc, take * sizeof *out->rows);
      out->row_lens = (size_t *)gdefstor_mem0(alloc,
          take * sizeof *out->row_lens);
      if (out->entities == NULL || out->entity_lens == NULL ||
          out->rows == NULL || out->row_lens == NULL) {
        rc = GDEFSTOR_ERR_OOM;
      }
    }
    for (i = 0; rc == GDEFSTOR_OK && i < take; i++) {
      Hit * h = &hits[(size_t)first + i];
      out->entities[i] = h->entity;
      out->entity_lens[i] = h->entity_len;
      out->rows[i] = h->row;
      out->row_lens[i] = h->row_len;
      h->entity = NULL;
      h->row = NULL;
      out->count = i + 1;
    }
  }
  for (i = 0; i < nhits; i++) {
    hit_free(alloc, &order, &hits[i]);
  }
  gcu_allocator_free(alloc, hits);
  if (rc != GDEFSTOR_OK) {
    for (i = 0; i < out->count; i++) {
      gcu_allocator_free(alloc, out->entities[i]);
      gcu_allocator_free(alloc, out->rows[i]);
    }
    gcu_allocator_free(alloc, out->entities);
    gcu_allocator_free(alloc, out->entity_lens);
    gcu_allocator_free(alloc, out->rows);
    gcu_allocator_free(alloc, out->row_lens);
    memset(out, 0, sizeof *out);
  }
  return rc;
}

/* ---- the result ---------------------------------------------------------- */

typedef struct ResultValue {
  int present;
  uint8_t * bytes;
  size_t len;
} ResultValue;

struct GDEFSTOR_QueryResult {
  const GDEFSTOR_Allocator * alloc;
  uint64_t total;
  size_t count;
  size_t nselect;
  char ** entities;
  size_t * entity_lens;
  ResultValue * values; /* count * nselect */
};

void gdefstor_query_result_free(GDEFSTOR_QueryResult * r) {
  size_t i;
  if (r == NULL) {
    return;
  }
  for (i = 0; i < r->count; i++) {
    gcu_allocator_free(r->alloc, r->entities[i]);
  }
  for (i = 0; i < r->count * r->nselect; i++) {
    gcu_allocator_free(r->alloc, r->values[i].bytes);
  }
  gcu_allocator_free(r->alloc, r->entities);
  gcu_allocator_free(r->alloc, r->entity_lens);
  gcu_allocator_free(r->alloc, r->values);
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

GDEFSTOR_Result gdefstor_query_result_value(const GDEFSTOR_QueryResult * r,
    size_t row, size_t select, int * out_present, const uint8_t ** out_bytes,
    size_t * out_len) {
  const ResultValue * v;
  if (r == NULL || out_present == NULL || out_bytes == NULL ||
      out_len == NULL || row >= r->count || select >= r->nselect) {
    return GDEFSTOR_ERR_INVALID;
  }
  v = &r->values[row * r->nselect + select];
  *out_present = v->present;
  *out_bytes = v->bytes;
  *out_len = v->len;
  return GDEFSTOR_OK;
}

/* Take what a backend wrote and the select columns of each row. */
static GDEFSTOR_Result result_build(GDEFSTOR_Store * store,
    const GDEFSTOR_Query * q, GDEFSTOR_QueryRows * rows,
    GDEFSTOR_QueryResult ** out) {
  const GDEFSTOR_Allocator * alloc = store->alloc;
  GDEFSTOR_QueryResult * r;
  size_t i;
  size_t j;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  r = (GDEFSTOR_QueryResult *)gdefstor_mem0(alloc, sizeof *r);
  if (r == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  r->alloc = alloc;
  r->total = rows->total;
  r->nselect = q != NULL ? q->select_count : 0;
  if (rows->count > 0) {
    r->entities = (char **)gdefstor_mem0(alloc, rows->count * sizeof *r->entities);
    r->entity_lens = (size_t *)gdefstor_mem0(alloc,
        rows->count * sizeof *r->entity_lens);
    if (r->nselect > 0) {
      r->values = (ResultValue *)gdefstor_mem0(alloc,
          rows->count * r->nselect * sizeof *r->values);
    }
    if (r->entities == NULL || r->entity_lens == NULL ||
        (r->nselect > 0 && r->values == NULL)) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  /* The result owns the entities from here on. */
  for (i = 0; rc == GDEFSTOR_OK && i < rows->count; i++) {
    r->entities[i] = rows->entities[i];
    r->entity_lens[i] = rows->entity_lens[i];
    rows->entities[i] = NULL;
    r->count = i + 1;
  }
  for (i = 0; rc == GDEFSTOR_OK && i < r->count; i++) {
    GDEFSTOR_QueryEnv env;
    memset(&env, 0, sizeof env);
    env.main.bytes = rows->rows[i];
    env.main.len = rows->row_lens[i];
    env.entity = (const uint8_t *)r->entities[i];
    env.entity_len = r->entity_lens[i];
    for (j = 0; rc == GDEFSTOR_OK && j < r->nselect; j++) {
      ResultValue * v = &r->values[i * r->nselect + j];
      rc = gdefstor_query_first_text(alloc, &q->select[j], &env, &v->present,
          &v->bytes, &v->len);
    }
  }
  if (rc != GDEFSTOR_OK) {
    gdefstor_query_result_free(r);
    return rc;
  }
  *out = r;
  return GDEFSTOR_OK;
}

static void rows_release(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_QueryRows * rows) {
  size_t i;
  for (i = 0; i < rows->count; i++) {
    gcu_allocator_free(alloc, rows->entities != NULL ? rows->entities[i] : NULL);
    gcu_allocator_free(alloc, rows->rows != NULL ? rows->rows[i] : NULL);
  }
  gcu_allocator_free(alloc, rows->entities);
  gcu_allocator_free(alloc, rows->entity_lens);
  gcu_allocator_free(alloc, rows->rows);
  gcu_allocator_free(alloc, rows->row_lens);
  memset(rows, 0, sizeof *rows);
}

/* ---- the entry points ---------------------------------------------------- */

static GDEFSTOR_Result run(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_Query * q, int count_only, GDEFSTOR_QueryRows * rows) {
  GDEFSTOR_QueryPlan plan;
  char * prefix = NULL;
  size_t prefix_len = 0;
  GDEFSTOR_Result rc;
  rc = gdefstor_projection_key_prefix(store->alloc, type, type_len, &prefix,
      &prefix_len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&plan, 0, sizeof plan);
  plan.type = type;
  plan.type_len = type_len;
  plan.prefix = prefix;
  plan.prefix_len = prefix_len;
  plan.query = q;
  plan.count_only = count_only;
  plan.limit = q->limit == 0 ? UINT64_MAX
               : (q->limit > GDEFSTOR_QUERY_LIMIT_MAX
                         ? GDEFSTOR_QUERY_LIMIT_MAX
                         : q->limit);
  memset(rows, 0, sizeof *rows);
  rc = GDEFSTOR_ERR_UNSUPPORTED;
  if (store->backend.query != NULL) {
    rc = store->backend.query(store->backend.ctx, tenant, tenant_len, &plan,
        rows);
  }
  /* A backend with no engine, or one that cannot carry this query (more
   * values than its statement can bind), is answered by the scan. */
  if (rc == GDEFSTOR_ERR_UNSUPPORTED) {
    memset(rows, 0, sizeof *rows);
    rc = scan_rows(store, tenant, tenant_len, &plan, rows);
  }
  gcu_allocator_free(store->alloc, prefix);
  return rc;
}

GDEFSTOR_Result gdefstor_projection_query(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_Query * query, GDEFSTOR_QueryResult ** out_result) {
  GDEFSTOR_QueryRows rows;
  GDEFSTOR_Result rc;
  if (store == NULL || out_result == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes) ||
      !gdefstor_span_ok(type, type_len, store->limits.max_type_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = check_query(&store->limits, query);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = run(store, tenant, tenant_len, type, type_len, query, 0, &rows);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = result_build(store, query, &rows, out_result);
  rows_release(store->alloc, &rows);
  return rc;
}

GDEFSTOR_Result gdefstor_projection_count(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_QueryNode * where, uint64_t * out_count) {
  GDEFSTOR_Query q;
  GDEFSTOR_QueryRows rows;
  GDEFSTOR_Result rc;
  if (store == NULL || out_count == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes) ||
      !gdefstor_span_ok(type, type_len, store->limits.max_type_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = check_where(&store->limits, where);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.where = where;
  rc = run(store, tenant, tenant_len, type, type_len, &q, 1, &rows);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  *out_count = rows.total;
  rows_release(store->alloc, &rows);
  return GDEFSTOR_OK;
}
