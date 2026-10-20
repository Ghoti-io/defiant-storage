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
 * Field trees: building one, reading one, and their JSON form
 * (`defiant-tree/1`, documentation/model.md), which drafts are stored in.
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/allocator.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#define TREE_FORMAT "defiant-tree/1"

static int same_span(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

/* ---- building ------------------------------------------------------------ */

static void node_release(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_TreeNode * n) {
  size_t k;
  for (k = 0; k < n->nvals; k++) {
    gcu_allocator_free(alloc, n->vals[k].field);
    gdefstor_model_value_clear(alloc, &n->vals[k].v);
  }
  gcu_allocator_free(alloc, n->vals);
  gcu_allocator_free(alloc, n->kids);
  gcu_allocator_free(alloc, n->field);
}

void gdefstor_tree_free(GDEFSTOR_Tree * tree) {
  size_t i;
  if (tree == NULL) {
    return;
  }
  for (i = 0; i < tree->n; i++) {
    node_release(tree->alloc, &tree->items[i]);
  }
  gcu_allocator_free(tree->alloc, tree->items);
  gcu_allocator_free(tree->alloc, tree);
}

GDEFSTOR_Result gdefstor_tree_create(const GDEFSTOR_Allocator * allocator,
    GDEFSTOR_Tree ** out_tree) {
  const GDEFSTOR_Allocator * alloc = gdefstor_alloc(allocator);
  GDEFSTOR_Tree * t;
  if (out_tree == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  t = (GDEFSTOR_Tree *)gdefstor_mem0(alloc, sizeof *t);
  if (t == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  t->alloc = alloc;
  t->items = (GDEFSTOR_TreeNode *)gdefstor_mem0(alloc, 8 * sizeof *t->items);
  if (t->items == NULL) {
    gcu_allocator_free(alloc, t);
    return GDEFSTOR_ERR_OOM;
  }
  t->cap = 8;
  t->n = 1;
  t->items[0].parent = GDEFSTOR_NONE;
  *out_tree = t;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_model_tree_push(GDEFSTOR_Tree * tree, size_t parent,
    const char * field, size_t field_len, uint64_t id, double order,
    size_t * out_item) {
  GDEFSTOR_TreeNode * p;
  GDEFSTOR_TreeNode * n;
  if (tree == NULL || parent >= tree->n) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (tree->n >= GDEFSTOR_MODEL_MAX_ITEMS) {
    return GDEFSTOR_ERR_LIMIT;
  }
  if (tree->n == tree->cap) {
    size_t grown = tree->cap * 2;
    GDEFSTOR_TreeNode * next = (GDEFSTOR_TreeNode *)gcu_allocator_realloc(
        tree->alloc, tree->items, grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    tree->items = next;
    tree->cap = grown;
  }
  p = &tree->items[parent];
  if (p->nkids == p->capkids) {
    size_t grown = p->capkids == 0 ? 4 : p->capkids * 2;
    size_t * next = (size_t *)gcu_allocator_realloc(tree->alloc, p->kids,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    p->kids = next;
    p->capkids = grown;
  }
  n = &tree->items[tree->n];
  memset(n, 0, sizeof *n);
  n->field = gdefstor_dup(tree->alloc, field, field_len);
  if (n->field == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  n->field_len = field_len;
  n->id = id;
  n->parent = parent;
  n->order = order;
  p->kids[p->nkids] = tree->n;
  p->nkids += 1;
  if (out_item != NULL) {
    *out_item = tree->n;
  }
  tree->n += 1;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_tree_add(GDEFSTOR_Tree * tree, size_t parent,
    const char * field, size_t field_len, uint64_t id, size_t * out_item) {
  if (tree == NULL || !gdefstor_model_name_ok(field, field_len)) {
    return GDEFSTOR_ERR_INVALID;
  }
  return gdefstor_model_tree_push(tree, parent, field, field_len, id, 0.0,
      out_item);
}

GDEFSTOR_Result gdefstor_model_tree_put(GDEFSTOR_Tree * tree, size_t item,
    const char * field, size_t field_len, const GDEFSTOR_Value * value) {
  GDEFSTOR_TreeNode * n;
  GDEFSTOR_Value copy;
  size_t k;
  GDEFSTOR_Result rc;
  if (tree == NULL || item >= tree->n || value == NULL ||
      (field == NULL && field_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = &tree->items[item];
  for (k = 0; k < n->nvals; k++) {
    if (same_span(n->vals[k].field, n->vals[k].field_len, field, field_len)) {
      break;
    }
  }
  if (value->type == 0) {
    if (k < n->nvals) {
      tree->bytes -= n->vals[k].v.len + n->vals[k].v.subject_len;
      gcu_allocator_free(tree->alloc, n->vals[k].field);
      gdefstor_model_value_clear(tree->alloc, &n->vals[k].v);
      memmove(&n->vals[k], &n->vals[k + 1],
          (n->nvals - k - 1) * sizeof *n->vals);
      n->nvals -= 1;
    }
    return GDEFSTOR_OK;
  }
  rc = gdefstor_model_value_copy(tree->alloc, &copy, value);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (k < n->nvals) {
    tree->bytes -= n->vals[k].v.len + n->vals[k].v.subject_len;
    gdefstor_model_value_clear(tree->alloc, &n->vals[k].v);
    n->vals[k].v = copy;
    /* A MAC was of the value it replaces. */
    memset(&n->vals[k].blind, 0, sizeof n->vals[k].blind);
    tree->bytes += copy.len + copy.subject_len;
    return GDEFSTOR_OK;
  }
  if (n->nvals == n->capvals) {
    size_t grown = n->capvals == 0 ? 4 : n->capvals * 2;
    GDEFSTOR_TreeVal * next = (GDEFSTOR_TreeVal *)gcu_allocator_realloc(
        tree->alloc, n->vals, grown * sizeof *next);
    if (next == NULL) {
      gdefstor_model_value_clear(tree->alloc, &copy);
      return GDEFSTOR_ERR_OOM;
    }
    n->vals = next;
    n->capvals = grown;
  }
  n->vals[n->nvals].field = gdefstor_dup(tree->alloc, field, field_len);
  if (n->vals[n->nvals].field == NULL) {
    gdefstor_model_value_clear(tree->alloc, &copy);
    return GDEFSTOR_ERR_OOM;
  }
  n->vals[n->nvals].field_len = field_len;
  n->vals[n->nvals].v = copy;
  memset(&n->vals[n->nvals].blind, 0, sizeof n->vals[n->nvals].blind);
  n->nvals += 1;
  tree->bytes += copy.len + copy.subject_len;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_tree_set(GDEFSTOR_Tree * tree, size_t item,
    const char * field, size_t field_len, const GDEFSTOR_Value * value) {
  if (tree == NULL || value == NULL ||
      (field_len != 0 && !gdefstor_model_name_ok(field, field_len)) ||
      value->type < 0 || value->type >= GDEFSTOR_FIELD_GROUP ||
      (value->bytes == NULL && value->len != 0) ||
      (value->subject == NULL && value->subject_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (value->len > GDEFSTOR_MODEL_MAX_VALUE_BYTES ||
      value->subject_len > GDEFSTOR_MODEL_MAX_VALUE_BYTES ||
      tree->bytes + value->len + value->subject_len >
          GDEFSTOR_MODEL_MAX_TREE_BYTES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  return gdefstor_model_tree_put(tree, item, field, field_len, value);
}

GDEFSTOR_Result gdefstor_tree_set_blind(GDEFSTOR_Tree * tree, size_t item,
    const char * field, size_t field_len, const GDEFSTOR_BlindMac * mac) {
  GDEFSTOR_TreeNode * n;
  size_t k;
  if (tree == NULL || mac == NULL || mac->size != sizeof *mac ||
      item >= tree->n || (field == NULL && field_len != 0) ||
      mac->mac == NULL || mac->mac_len != GDEFSTOR_BLIND_MAC_BYTES) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = &tree->items[item];
  for (k = 0; k < n->nvals; k++) {
    if (same_span(n->vals[k].field, n->vals[k].field_len, field, field_len)) {
      GDEFSTOR_BlindVal * b = &n->vals[k].blind;
      b->set = 1;
      memcpy(b->mac, mac->mac, GDEFSTOR_BLIND_MAC_BYTES);
      b->key_id = mac->key_id;
      b->version = mac->version;
      return GDEFSTOR_OK;
    }
  }
  /* A MAC is of a value: there is none here to give it to. */
  return GDEFSTOR_ERR_INVALID;
}

/* ---- reading ------------------------------------------------------------- */

size_t gdefstor_tree_item_count(const GDEFSTOR_Tree * tree) {
  return tree == NULL ? 0 : tree->n;
}

GDEFSTOR_Result gdefstor_tree_item(const GDEFSTOR_Tree * tree, size_t index,
    GDEFSTOR_TreeItem * out) {
  const GDEFSTOR_TreeNode * n;
  if (tree == NULL || out == NULL || out->size != sizeof *out ||
      index >= tree->n) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = &tree->items[index];
  out->id = n->id;
  out->parent = n->parent == GDEFSTOR_NONE ? GDEFSTOR_TYPE_ROOT : n->parent;
  out->field = n->field == NULL ? "" : n->field;
  out->field_len = n->field_len;
  out->order = n->order;
  out->value_count = n->nvals;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_tree_value(const GDEFSTOR_Tree * tree, size_t item,
    size_t k, const char ** out_field, size_t * out_field_len,
    GDEFSTOR_Value * out_value) {
  const GDEFSTOR_TreeNode * n;
  if (tree == NULL || item >= tree->n || out_field == NULL ||
      out_field_len == NULL || out_value == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = &tree->items[item];
  if (k >= n->nvals) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_field = n->vals[k].field;
  *out_field_len = n->vals[k].field_len;
  *out_value = n->vals[k].v;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_tree_get(const GDEFSTOR_Tree * tree, size_t item,
    const char * field, size_t field_len, GDEFSTOR_Value * out_value) {
  const GDEFSTOR_TreeNode * n;
  size_t k;
  if (tree == NULL || item >= tree->n || out_value == NULL ||
      (field == NULL && field_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = &tree->items[item];
  memset(out_value, 0, sizeof *out_value);
  for (k = 0; k < n->nvals; k++) {
    if (same_span(n->vals[k].field, n->vals[k].field_len, field, field_len)) {
      *out_value = n->vals[k].v;
      break;
    }
  }
  return GDEFSTOR_OK;
}

/* ---- dates --------------------------------------------------------------- */

static int leap(int64_t y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const int month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31,
    30, 31};

/* Days from 0001-01-01 to y-m-d, for y >= 1. */
static int64_t days_from_0001(int64_t y, int m, int d) {
  int64_t p = y - 1;
  int64_t days = 365 * p + p / 4 - p / 100 + p / 400;
  int k;
  for (k = 1; k < m; k++) {
    days += month_days[k - 1] + (k == 2 && leap(y) ? 1 : 0);
  }
  return days + d - 1;
}

#define EPOCH_FROM_0001 INT64_C(719162)

static void civil(int64_t days, int64_t * y, int * m, int * d) {
  int64_t n = days + EPOCH_FROM_0001; /* days from 0001-01-01, >= 0 */
  int64_t year = 1 + n / 366;
  int k;
  while (days_from_0001(year + 1, 1, 1) <= n) {
    year += 1;
  }
  n -= days_from_0001(year, 1, 1);
  for (k = 1; k <= 12; k++) {
    int len = month_days[k - 1] + (k == 2 && leap(year) ? 1 : 0);
    if (n < len) {
      break;
    }
    n -= len;
  }
  *y = year;
  *m = k;
  *d = (int)n + 1;
}

static int digits(const char * s, size_t n, int64_t * out) {
  size_t i;
  int64_t v = 0;
  if (n == 0) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return 0;
    }
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return 1;
}

static int parse_date(const char * s, size_t n, int64_t * out) {
  int64_t y;
  int64_t m;
  int64_t d;
  int len;
  if (n != 10 || s[4] != '-' || s[7] != '-' || !digits(s, 4, &y) ||
      !digits(s + 5, 2, &m) || !digits(s + 8, 2, &d) || y < 1 || m < 1 ||
      m > 12) {
    return 0;
  }
  len = month_days[m - 1] + (m == 2 && leap(y) ? 1 : 0);
  if (d < 1 || d > len) {
    return 0;
  }
  *out = days_from_0001(y, (int)m, (int)d) - EPOCH_FROM_0001;
  return 1;
}

/* YYYY-MM-DDTHH:MM:SS(.mmm)?Z */
static int parse_datetime(const char * s, size_t n, int64_t * out) {
  int64_t days;
  int64_t h;
  int64_t mi;
  int64_t sec;
  int64_t ms = 0;
  if (n < 20 || !parse_date(s, 10, &days) || s[10] != 'T' || s[13] != ':' ||
      s[16] != ':' || s[n - 1] != 'Z' || !digits(s + 11, 2, &h) ||
      !digits(s + 14, 2, &mi) || !digits(s + 17, 2, &sec) || h > 23 ||
      mi > 59 || sec > 59) {
    return 0;
  }
  if (n == 24) {
    if (s[19] != '.' || !digits(s + 20, 3, &ms)) {
      return 0;
    }
  } else if (n != 20) {
    return 0;
  }
  *out = ((days * 24 + h) * 60 + mi) * 60000 + sec * 1000 + ms;
  return 1;
}

static int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) {
    q -= 1;
  }
  return q;
}

/* Write @p v, 0 <= v < 10^width, as exactly @p width digits. */
static void put_digits(char * out, int64_t v, int width) {
  int i;
  for (i = width - 1; i >= 0; i--) {
    out[i] = (char)('0' + (int)(v % 10));
    v /= 10;
  }
}

/* YYYY-MM-DD, 10 bytes and a NUL. */
static void write_date(char * out, int64_t days) {
  int64_t y;
  int m;
  int d;
  civil(days, &y, &m, &d);
  put_digits(out, y, 4);
  out[4] = '-';
  put_digits(out + 5, m, 2);
  out[7] = '-';
  put_digits(out + 8, d, 2);
  out[10] = '\0';
}

/* YYYY-MM-DDTHH:MM:SS.mmmZ, 24 bytes and a NUL. */
static void write_datetime(char * out, int64_t ms) {
  int64_t days = floor_div(ms, INT64_C(86400000));
  int64_t rem = ms - days * INT64_C(86400000);
  write_date(out, days);
  out[10] = 'T';
  put_digits(out + 11, rem / 3600000, 2);
  out[13] = ':';
  put_digits(out + 14, rem / 60000 % 60, 2);
  out[16] = ':';
  put_digits(out + 17, rem / 1000 % 60, 2);
  out[19] = '.';
  put_digits(out + 20, rem % 1000, 3);
  out[23] = 'Z';
  out[24] = '\0';
}

/* -?digits(.digits)? with at most @p scale digits after the point. */
static int parse_decimal(const char * s, size_t n, int scale, int64_t * out) {
  size_t i = 0;
  int neg = 0;
  int64_t v = 0;
  int frac = -1;
  int any = 0;
  if (n > 0 && s[0] == '-') {
    neg = 1;
    i = 1;
  }
  for (; i < n; i++) {
    char c = s[i];
    if (c == '.') {
      if (frac >= 0 || !any) {
        return 0;
      }
      frac = 0;
      continue;
    }
    if (c < '0' || c > '9') {
      return 0;
    }
    if (frac >= 0) {
      frac += 1;
      if (frac > scale) {
        return 0;
      }
    }
    if (v > (GDEFSTOR_MODEL_MAX_DECIMAL - (c - '0')) / 10) {
      return 0;
    }
    v = v * 10 + (c - '0');
    any = 1;
  }
  if (!any || frac == 0) {
    return 0;
  }
  for (frac = frac < 0 ? 0 : frac; frac < scale; frac++) {
    if (v > GDEFSTOR_MODEL_MAX_DECIMAL / 10) {
      return 0;
    }
    v *= 10;
  }
  *out = neg ? -v : v;
  return 1;
}

static void write_decimal(char * out, size_t cap, int64_t v, int scale) {
  char tmp[48];
  uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
  int len;
  int at = 0;
  len = snprintf(tmp, sizeof tmp, "%0*llu", scale + 1, (unsigned long long)mag);
  if (v < 0) {
    out[at++] = '-';
  }
  memcpy(out + at, tmp, (size_t)(len - scale));
  at += len - scale;
  if (scale > 0) {
    out[at++] = '.';
    memcpy(out + at, tmp + len - scale, (size_t)scale);
    at += scale;
  }
  out[at] = '\0';
  (void)cap;
}

/* ---- the JSON form: parsing ---------------------------------------------- */

typedef struct Parse {
  const GDEFSTOR_Type * type;
  GDEFSTOR_Tree * tree;
  const GDEFSTOR_Allocator * alloc;
} Parse;

static int hex_val(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

static GDEFSTOR_Result parse_id(const GTEXT_JSON_Value * v, uint64_t * out) {
  int64_t id;
  const char * lex;
  size_t lex_len;
  size_t i;
  if (gtext_json_get_number_lexeme(v, &lex, &lex_len) != GTEXT_JSON_OK ||
      lex_len == 0 || lex_len > 18) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; i < lex_len; i++) {
    if (lex[i] < '0' || lex[i] > '9') {
      return GDEFSTOR_ERR_INVALID;
    }
  }
  if (gtext_json_get_i64(v, &id) != GTEXT_JSON_OK || id < 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out = (uint64_t)id;
  return GDEFSTOR_OK;
}

/* Set item @p item's value for field @p f (@p own: the item's own value). */
static GDEFSTOR_Result parse_scalar(Parse * p, size_t item,
    const GDEFSTOR_MField * f, int own, const GTEXT_JSON_Value * j) {
  GDEFSTOR_Value v;
  const char * s;
  size_t s_len;
  uint8_t * buf = NULL;
  GDEFSTOR_Result rc;
  memset(&v, 0, sizeof v);
  v.type = f->type;
  if (gtext_json_typeof(j) == GTEXT_JSON_NULL) {
    return GDEFSTOR_OK;
  }
  if ((f->flags & GDEFSTOR_FIELD_PERSONAL) != 0) {
    const GTEXT_JSON_Value * c = gtext_json_object_get(j, "@c", 2);
    const GTEXT_JSON_Value * subj = gtext_json_object_get(j, "@s", 2);
    const char * subj_text = NULL;
    size_t i;
    if (gtext_json_typeof(j) != GTEXT_JSON_OBJECT ||
        gtext_json_object_size(j) != 2 ||
        gtext_json_get_string(c, &s, &s_len) != GTEXT_JSON_OK ||
        s_len % 2 != 0 ||
        gtext_json_get_string(subj, &subj_text, &v.subject_len) !=
            GTEXT_JSON_OK) {
      return GDEFSTOR_ERR_INVALID;
    }
    v.subject = (const uint8_t *)subj_text;
    if (s_len / 2 > GDEFSTOR_MODEL_MAX_VALUE_BYTES) {
      return GDEFSTOR_ERR_LIMIT;
    }
    buf = (uint8_t *)gdefstor_mem(p->alloc, s_len / 2 + 1);
    if (buf == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    for (i = 0; i < s_len / 2; i++) {
      int hi = hex_val(s[2 * i]);
      int lo = hex_val(s[2 * i + 1]);
      if (hi < 0 || lo < 0) {
        gcu_allocator_free(p->alloc, buf);
        return GDEFSTOR_ERR_INVALID;
      }
      buf[i] = (uint8_t)(hi * 16 + lo);
    }
    v.bytes = buf;
    v.len = s_len / 2;
  } else {
    switch (f->type) {
    case GDEFSTOR_FIELD_TEXT:
    case GDEFSTOR_FIELD_REFERENCE:
      if (gtext_json_get_string(j, &s, &s_len) != GTEXT_JSON_OK) {
        return GDEFSTOR_ERR_INVALID;
      }
      v.bytes = (const uint8_t *)s;
      v.len = s_len;
      break;
    case GDEFSTOR_FIELD_INTEGER: {
      const char * lex;
      size_t lex_len;
      size_t i = 0;
      uint64_t mag = 0;
      int neg = 0;
      if (gtext_json_get_number_lexeme(j, &lex, &lex_len) != GTEXT_JSON_OK ||
          lex_len == 0) {
        return GDEFSTOR_ERR_INVALID;
      }
      if (lex[0] == '-') {
        neg = 1;
        i = 1;
      }
      if (i == lex_len) {
        return GDEFSTOR_ERR_INVALID;
      }
      for (; i < lex_len; i++) {
        if (lex[i] < '0' || lex[i] > '9' ||
            mag > (UINT64_C(9223372036854775808) - (uint64_t)(lex[i] - '0')) /
                      10) {
          return GDEFSTOR_ERR_INVALID;
        }
        mag = mag * 10 + (uint64_t)(lex[i] - '0');
      }
      if (!neg && mag > (uint64_t)INT64_MAX) {
        return GDEFSTOR_ERR_INVALID;
      }
      v.integer = neg ? (mag == UINT64_C(9223372036854775808)
                             ? INT64_MIN
                             : -(int64_t)mag)
                      : (int64_t)mag;
      break;
    }
    case GDEFSTOR_FIELD_FLOAT:
      if (gtext_json_typeof(j) != GTEXT_JSON_NUMBER ||
          gtext_json_get_double(j, &v.real) != GTEXT_JSON_OK) {
        return GDEFSTOR_ERR_INVALID;
      }
      break;
    case GDEFSTOR_FIELD_BOOLEAN: {
      bool b;
      if (gtext_json_get_bool(j, &b) != GTEXT_JSON_OK) {
        return GDEFSTOR_ERR_INVALID;
      }
      v.integer = b ? 1 : 0;
      break;
    }
    case GDEFSTOR_FIELD_DECIMAL:
      if (gtext_json_get_string(j, &s, &s_len) != GTEXT_JSON_OK ||
          !parse_decimal(s, s_len, f->scale, &v.integer)) {
        return GDEFSTOR_ERR_INVALID;
      }
      break;
    case GDEFSTOR_FIELD_DATE:
      if (gtext_json_get_string(j, &s, &s_len) != GTEXT_JSON_OK ||
          !parse_date(s, s_len, &v.integer)) {
        return GDEFSTOR_ERR_INVALID;
      }
      break;
    case GDEFSTOR_FIELD_DATETIME:
      if (gtext_json_get_string(j, &s, &s_len) != GTEXT_JSON_OK ||
          !parse_datetime(s, s_len, &v.integer)) {
        return GDEFSTOR_ERR_INVALID;
      }
      break;
    default:
      return GDEFSTOR_ERR_INVALID;
    }
  }
  rc = own ? gdefstor_tree_set(p->tree, item, NULL, 0, &v)
           : gdefstor_tree_set(p->tree, item, f->name, f->name_len, &v);
  gcu_allocator_free(p->alloc, buf);
  return rc;
}

static GDEFSTOR_Result parse_item(Parse * p, const GTEXT_JSON_Value * obj,
    size_t item, size_t group_field);

static GDEFSTOR_Result parse_child(Parse * p, size_t item,
    const GDEFSTOR_MField * f, const GTEXT_JSON_Value * j, size_t fi) {
  size_t at;
  GDEFSTOR_Result rc;
  if (f->type == GDEFSTOR_FIELD_GROUP) {
    if (gtext_json_typeof(j) != GTEXT_JSON_OBJECT) {
      return GDEFSTOR_ERR_INVALID;
    }
    rc = gdefstor_tree_add(p->tree, item, f->name, f->name_len, 0, &at);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    return parse_item(p, j, at, fi);
  }
  /* An item of a multiple scalar: {"@id": n, "@v": value}, or a bare value. */
  if (gtext_json_typeof(j) == GTEXT_JSON_OBJECT &&
      gtext_json_object_get(j, "@v", 2) != NULL) {
    const GTEXT_JSON_Value * idv = gtext_json_object_get(j, "@id", 3);
    uint64_t id = 0;
    if (gtext_json_object_size(j) != (idv != NULL ? 2u : 1u)) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (idv != NULL) {
      rc = parse_id(idv, &id);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    rc = gdefstor_tree_add(p->tree, item, f->name, f->name_len, id, &at);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    return parse_scalar(p, at, f, 1, gtext_json_object_get(j, "@v", 2));
  }
  rc = gdefstor_tree_add(p->tree, item, f->name, f->name_len, 0, &at);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  return parse_scalar(p, at, f, 1, j);
}

static GDEFSTOR_Result parse_item(Parse * p, const GTEXT_JSON_Value * obj,
    size_t item, size_t group_field) {
  size_t i;
  size_t n;
  GDEFSTOR_Result rc;
  if (gtext_json_typeof(obj) != GTEXT_JSON_OBJECT) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = gtext_json_object_size(obj);
  for (i = 0; i < n; i++) {
    size_t key_len;
    const char * key = gtext_json_object_key(obj, i, &key_len);
    const GTEXT_JSON_Value * j = gtext_json_object_value(obj, i);
    size_t fi;
    const GDEFSTOR_MField * f;
    if (same_span(key, key_len, "@id", 3)) {
      uint64_t id;
      rc = parse_id(j, &id);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      p->tree->items[item].id = id;
      continue;
    }
    fi = gdefstor_model_type_child(p->type, group_field, key, key_len);
    if (fi == GDEFSTOR_NONE) {
      return GDEFSTOR_ERR_INVALID;
    }
    f = &p->type->f[fi];
    if (gtext_json_typeof(j) == GTEXT_JSON_NULL) {
      continue;
    }
    if ((f->flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
      size_t k;
      size_t m;
      if (gtext_json_typeof(j) != GTEXT_JSON_ARRAY) {
        return GDEFSTOR_ERR_INVALID;
      }
      m = gtext_json_array_size(j);
      for (k = 0; k < m; k++) {
        rc = parse_child(p, item, f, gtext_json_array_get(j, k), fi);
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
      }
    } else if (f->type == GDEFSTOR_FIELD_GROUP) {
      rc = parse_child(p, item, f, j, fi);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    } else {
      rc = parse_scalar(p, item, f, 0, j);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result json_status(GTEXT_JSON_Status code) {
  if (code == GTEXT_JSON_E_OOM) {
    return GDEFSTOR_ERR_OOM;
  }
  if (code == GTEXT_JSON_E_LIMIT || code == GTEXT_JSON_E_DEPTH) {
    return GDEFSTOR_ERR_LIMIT;
  }
  return GDEFSTOR_ERR_CORRUPT;
}

GDEFSTOR_Result gdefstor_model_from_json(const GDEFSTOR_Type * type,
    const uint8_t * json, size_t len, const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_Tree ** out_tree) {
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GTEXT_JSON_Value * root;
  const char * s;
  size_t s_len;
  Parse p;
  GDEFSTOR_Result rc;
  if (len > GDEFSTOR_MODEL_MAX_DRAFT_BYTES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  memset(&err, 0, sizeof err);
  opt = gtext_json_parse_options_default();
  opt.max_depth = 2 * (GDEFSTOR_MODEL_MAX_DEPTH + 2) + 4;
  opt.dupkeys = GTEXT_JSON_DUPKEY_ERROR;
  opt.allocator = (const GTEXT_Allocator *)alloc;
  root = gtext_json_parse((const char *)json, len, &opt, &err);
  if (root == NULL) {
    rc = json_status(err.code);
    gtext_json_error_free(&err);
    return rc;
  }
  gtext_json_error_free(&err);
  if (gtext_json_typeof(root) != GTEXT_JSON_OBJECT ||
      gtext_json_get_string(gtext_json_object_get(root, "format", 6), &s,
          &s_len) != GTEXT_JSON_OK) {
    gtext_json_free(root);
    return GDEFSTOR_ERR_FORMAT;
  }
  if (!same_span(s, s_len, TREE_FORMAT, strlen(TREE_FORMAT))) {
    /* The name is read before the document that holds it is freed. */
    int newer = s_len > 13 && memcmp(s, "defiant-tree/", 13) == 0;
    gtext_json_free(root);
    return newer ? GDEFSTOR_ERR_UNSUPPORTED : GDEFSTOR_ERR_FORMAT;
  }
  if (gtext_json_object_size(root) != 2 ||
      gtext_json_typeof(gtext_json_object_get(root, "root", 4)) !=
          GTEXT_JSON_OBJECT) {
    gtext_json_free(root);
    return GDEFSTOR_ERR_CORRUPT;
  }
  memset(&p, 0, sizeof p);
  p.type = type;
  p.alloc = alloc;
  rc = gdefstor_tree_create(alloc, &p.tree);
  if (rc == GDEFSTOR_OK) {
    rc = parse_item(&p, gtext_json_object_get(root, "root", 4), 0,
        GDEFSTOR_NONE);
  }
  gtext_json_free(root);
  if (rc != GDEFSTOR_OK) {
    gdefstor_tree_free(p.tree);
    return rc;
  }
  *out_tree = p.tree;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_model_json_ok(const GDEFSTOR_Allocator * alloc,
    const uint8_t * json, size_t len) {
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GTEXT_JSON_Value * root;
  const char * s;
  size_t s_len;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (len > GDEFSTOR_MODEL_MAX_DRAFT_BYTES) {
    return GDEFSTOR_ERR_LIMIT;
  }
  memset(&err, 0, sizeof err);
  opt = gtext_json_parse_options_default();
  opt.max_depth = 2 * (GDEFSTOR_MODEL_MAX_DEPTH + 2) + 4;
  opt.dupkeys = GTEXT_JSON_DUPKEY_ERROR;
  opt.allocator = (const GTEXT_Allocator *)alloc;
  root = gtext_json_parse((const char *)json, len, &opt, &err);
  if (root == NULL) {
    rc = json_status(err.code);
    gtext_json_error_free(&err);
    return rc == GDEFSTOR_ERR_CORRUPT ? GDEFSTOR_ERR_INVALID : rc;
  }
  gtext_json_error_free(&err);
  if (gtext_json_typeof(root) != GTEXT_JSON_OBJECT ||
      gtext_json_get_string(gtext_json_object_get(root, "format", 6), &s,
          &s_len) != GTEXT_JSON_OK ||
      !same_span(s, s_len, TREE_FORMAT, strlen(TREE_FORMAT)) ||
      gtext_json_typeof(gtext_json_object_get(root, "root", 4)) !=
          GTEXT_JSON_OBJECT) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  gtext_json_free(root);
  return rc;
}

/* ---- the draft document, defiant-draft/1 ---------------------------------- */

#define DRAFT_HEAD "{\"format\":\"defiant-draft/1\",\"languages\":{"
#define DRAFT_NAME "{\"format\":\"defiant-draft/"

GDEFSTOR_Result gdefstor_model_draft_pack(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_DraftTree * const * trees, size_t count, uint8_t ** out,
    size_t * out_len) {
  char * buf = NULL;
  size_t len = 0;
  size_t cap = 0;
  size_t i;
  GDEFSTOR_Result rc = gdefstor_buf_append(&buf, &len, &cap, alloc,
      DRAFT_HEAD, strlen(DRAFT_HEAD));
  for (i = 0; rc == GDEFSTOR_OK && i < count; i++) {
    const GDEFSTOR_DraftTree * t = trees[i];
    if (i > 0) {
      rc = gdefstor_buf_append(&buf, &len, &cap, alloc, ",", 1);
    }
    /* A tag is [A-Za-z0-9-]: a JSON string as it is. */
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&buf, &len, &cap, alloc, "\"", 1);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&buf, &len, &cap, alloc, t->language,
          t->language_len);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&buf, &len, &cap, alloc, "\":", 2);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_buf_append(&buf, &len, &cap, alloc,
          (const char *)t->json, t->json_len);
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_buf_append(&buf, &len, &cap, alloc, "}}", 2);
  }
  if (rc == GDEFSTOR_OK && len > GDEFSTOR_MODEL_MAX_DRAFT_BYTES) {
    rc = GDEFSTOR_ERR_LIMIT;
  }
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(alloc, buf);
    return rc;
  }
  *out = (uint8_t *)buf;
  *out_len = len;
  return GDEFSTOR_OK;
}

static int json_space(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* The end of the object or array at @p at, or SIZE_MAX. Bounded on any
 * bytes; exact on JSON. */
static size_t skip_compound(const uint8_t * p, size_t n, size_t at) {
  size_t depth = 0;
  int in_str = 0;
  if (at >= n || (p[at] != '{' && p[at] != '[')) {
    return SIZE_MAX;
  }
  for (; at < n; at++) {
    uint8_t c = p[at];
    if (in_str) {
      if (c == '\\') {
        at += 1;
      } else if (c == '"') {
        in_str = 0;
      }
      continue;
    }
    if (c == '"') {
      in_str = 1;
    } else if (c == '{' || c == '[') {
      depth += 1;
    } else if (c == '}' || c == ']') {
      depth -= 1;
      if (depth == 0) {
        return at + 1;
      }
    }
  }
  return SIZE_MAX;
}

GDEFSTOR_Result gdefstor_model_draft_unpack(const GDEFSTOR_Allocator * alloc,
    const uint8_t * json, size_t len, GDEFSTOR_DraftPart ** out,
    size_t * out_count) {
  const size_t head = strlen(DRAFT_HEAD);
  GDEFSTOR_DraftPart * v = NULL;
  size_t n = 0;
  size_t cap = 0;
  size_t at = head;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  *out = NULL;
  *out_count = 0;
  if (json == NULL || len < head || memcmp(json, DRAFT_HEAD, head) != 0) {
    /* Another version is newer, anything else is not a draft. */
    return json != NULL && len > strlen(DRAFT_NAME) &&
                   memcmp(json, DRAFT_NAME, strlen(DRAFT_NAME)) == 0
               ? GDEFSTOR_ERR_UNSUPPORTED
               : GDEFSTOR_ERR_CORRUPT;
  }
  if (at < len && json[at] == '}') {
    at += 1;
  } else {
    for (;;) {
      size_t tag = at + 1;
      size_t end;
      GDEFSTOR_DraftPart * o;
      if (at >= len || json[at] != '"') {
        rc = GDEFSTOR_ERR_CORRUPT;
        break;
      }
      end = tag;
      while (end < len && json[end] != '"') {
        end += 1;
      }
      if (end >= len || !gdefstor_model_lang_ok((const char *)json + tag,
                            end - tag) ||
          end + 1 >= len || json[end + 1] != ':' ||
          n >= GDEFSTOR_MODEL_MAX_LANGUAGES ||
          (n > 0 && gdefstor_model_lang_cmp(v[n - 1].lang, v[n - 1].lang_len,
                        (const char *)json + tag, end - tag) >= 0)) {
        rc = GDEFSTOR_ERR_CORRUPT;
        break;
      }
      if (n == cap) {
        size_t grown = cap == 0 ? 4 : cap * 2;
        GDEFSTOR_DraftPart * next = (GDEFSTOR_DraftPart *)
            gcu_allocator_realloc(alloc, v, grown * sizeof *next);
        if (next == NULL) {
          rc = GDEFSTOR_ERR_OOM;
          break;
        }
        v = next;
        cap = grown;
      }
      o = &v[n];
      memset(o, 0, sizeof *o);
      memcpy(o->lang, json + tag, end - tag);
      o->lang_len = end - tag;
      /* The tree's bytes exactly as put, spaces around it included. */
      o->off = end + 2;
      at = o->off;
      while (at < len && json_space(json[at])) {
        at += 1;
      }
      at = skip_compound(json, len, at);
      if (at == SIZE_MAX) {
        rc = GDEFSTOR_ERR_CORRUPT;
        break;
      }
      while (at < len && json_space(json[at])) {
        at += 1;
      }
      o->len = at - o->off;
      n += 1;
      if (at < len && json[at] == ',') {
        at += 1;
        continue;
      }
      if (at < len && json[at] == '}') {
        at += 1;
        break;
      }
      rc = GDEFSTOR_ERR_CORRUPT;
      break;
    }
  }
  if (rc == GDEFSTOR_OK && (at + 1 != len || json[at] != '}')) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(alloc, v);
    return rc;
  }
  *out = v;
  *out_count = n;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_tree_from_json(const GDEFSTOR_Type * type,
    const uint8_t * json, size_t len, const GDEFSTOR_Allocator * allocator,
    GDEFSTOR_Tree ** out_tree) {
  if (type == NULL || out_tree == NULL || (json == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  return gdefstor_model_from_json(type, json, len, gdefstor_alloc(allocator),
      out_tree);
}

/* ---- the JSON form: writing ---------------------------------------------- */

typedef struct Out {
  const GDEFSTOR_Allocator * alloc;
  char * buf;
  size_t len;
  size_t cap;
  GDEFSTOR_Result rc;
} Out;

static void put(Out * w, const char * s, size_t n) {
  if (w->rc == GDEFSTOR_OK) {
    w->rc = gdefstor_buf_append(&w->buf, &w->len, &w->cap, w->alloc, s, n);
  }
}

static void puts_(Out * w, const char * s) {
  put(w, s, strlen(s));
}

static void put_str(Out * w, const char * s, size_t n) {
  if (w->rc == GDEFSTOR_OK) {
    w->rc = gdefstor_json_escape_append(&w->buf, &w->len, &w->cap, w->alloc,
        s, n);
  }
}

/* %.17g round-trips a double; the decimal point is the locale's, so it is
 * put back to '.' (CONVENTIONS: printf reads LC_NUMERIC). */
static void put_double(Out * w, double d) {
  char tmp[64];
  char fixed[64];
  const char * point = localeconv()->decimal_point;
  size_t plen = point == NULL ? 0 : strlen(point);
  size_t i = 0;
  size_t o = 0;
  int n = snprintf(tmp, sizeof tmp, "%.17g", d);
  while (n > 0 && i < (size_t)n && o + 1 < sizeof fixed) {
    if (plen > 0 && strncmp(tmp + i, point, plen) == 0) {
      fixed[o++] = '.';
      i += plen;
    } else {
      fixed[o++] = tmp[i++];
    }
  }
  fixed[o] = '\0';
  puts_(w, fixed);
}

static void put_scalar(Out * w, const GDEFSTOR_MField * f,
    const GDEFSTOR_Value * v) {
  char tmp[64];
  if ((f->flags & GDEFSTOR_FIELD_PERSONAL) != 0) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    puts_(w, "{\"@c\":\"");
    for (i = 0; i < v->len; i++) {
      char pair[2];
      pair[0] = hex[v->bytes[i] >> 4];
      pair[1] = hex[v->bytes[i] & 0xF];
      put(w, pair, 2);
    }
    puts_(w, "\",\"@s\":");
    put_str(w, (const char *)v->subject, v->subject_len);
    puts_(w, "}");
    return;
  }
  switch (f->type) {
  case GDEFSTOR_FIELD_TEXT:
  case GDEFSTOR_FIELD_REFERENCE:
    put_str(w, (const char *)v->bytes, v->len);
    return;
  case GDEFSTOR_FIELD_INTEGER:
    snprintf(tmp, sizeof tmp, "%lld", (long long)v->integer);
    puts_(w, tmp);
    return;
  case GDEFSTOR_FIELD_FLOAT:
    put_double(w, v->real);
    return;
  case GDEFSTOR_FIELD_BOOLEAN:
    puts_(w, v->integer ? "true" : "false");
    return;
  case GDEFSTOR_FIELD_DECIMAL:
    write_decimal(tmp, sizeof tmp, v->integer, f->scale);
    put_str(w, tmp, strlen(tmp));
    return;
  case GDEFSTOR_FIELD_DATE:
    write_date(tmp, v->integer);
    put_str(w, tmp, strlen(tmp));
    return;
  case GDEFSTOR_FIELD_DATETIME:
    write_datetime(tmp, v->integer);
    put_str(w, tmp, strlen(tmp));
    return;
  default:
    w->rc = GDEFSTOR_ERR_INVALID;
  }
}

static void put_id(Out * w, uint64_t id, int * first) {
  char tmp[32];
  if (id == 0) {
    return;
  }
  snprintf(tmp, sizeof tmp, "\"@id\":%llu", (unsigned long long)id);
  puts_(w, tmp);
  *first = 0;
}

static void write_item(Out * w, const GDEFSTOR_Type * type,
    const GDEFSTOR_Tree * tree, size_t item, size_t group_field);

static void write_child(Out * w, const GDEFSTOR_Type * type,
    const GDEFSTOR_Tree * tree, size_t kid, size_t fi) {
  const GDEFSTOR_MField * f = &type->f[fi];
  const GDEFSTOR_TreeNode * k = &tree->items[kid];
  if (f->type == GDEFSTOR_FIELD_GROUP) {
    write_item(w, type, tree, kid, fi);
    return;
  }
  {
    int first = 1;
    size_t v;
    puts_(w, "{");
    put_id(w, k->id, &first);
    for (v = 0; v < k->nvals; v++) {
      if (k->vals[v].field_len == 0) {
        if (!first) {
          puts_(w, ",");
        }
        puts_(w, "\"@v\":");
        put_scalar(w, f, &k->vals[v].v);
        first = 0;
      } else {
        w->rc = GDEFSTOR_ERR_INVALID;
      }
    }
    puts_(w, "}");
  }
}

static void write_item(Out * w, const GDEFSTOR_Type * type,
    const GDEFSTOR_Tree * tree, size_t item, size_t group_field) {
  const GDEFSTOR_TreeNode * n = &tree->items[item];
  size_t i;
  size_t used = 0;
  size_t kids_used = 0;
  int first = 1;
  puts_(w, "{");
  put_id(w, n->id, &first);
  for (i = 0; i < type->n && w->rc == GDEFSTOR_OK; i++) {
    const GDEFSTOR_MField * f = &type->f[i];
    size_t k;
    if (f->parent != group_field) {
      continue;
    }
    if (f->type != GDEFSTOR_FIELD_GROUP &&
        (f->flags & GDEFSTOR_FIELD_MULTIPLE) == 0) {
      for (k = 0; k < n->nvals; k++) {
        if (same_span(n->vals[k].field, n->vals[k].field_len, f->name,
                f->name_len)) {
          if (!first) {
            puts_(w, ",");
          }
          first = 0;
          put_str(w, f->name, f->name_len);
          puts_(w, ":");
          put_scalar(w, f, &n->vals[k].v);
          used += 1;
        }
      }
      continue;
    }
    {
      int any = 0;
      int multiple = (f->flags & GDEFSTOR_FIELD_MULTIPLE) != 0;
      for (k = 0; k < n->nkids; k++) {
        const GDEFSTOR_TreeNode * kid = &tree->items[n->kids[k]];
        if (!same_span(kid->field, kid->field_len, f->name, f->name_len)) {
          continue;
        }
        if (!any) {
          if (!first) {
            puts_(w, ",");
          }
          first = 0;
          put_str(w, f->name, f->name_len);
          puts_(w, multiple ? ":[" : ":");
        } else if (multiple) {
          puts_(w, ",");
        } else {
          w->rc = GDEFSTOR_ERR_INVALID; /* two items of a single group */
        }
        any = 1;
        write_child(w, type, tree, n->kids[k], i);
        kids_used += 1;
      }
      if (any && multiple) {
        puts_(w, "]");
      }
    }
  }
  /* A value or an item under a field the type does not have. */
  if (used != n->nvals || kids_used != n->nkids) {
    if (w->rc == GDEFSTOR_OK) {
      w->rc = GDEFSTOR_ERR_INVALID;
    }
  }
  puts_(w, "}");
}

GDEFSTOR_Result gdefstor_tree_to_json(const GDEFSTOR_Type * type,
    const GDEFSTOR_Tree * tree, uint8_t ** out_json, size_t * out_len) {
  Out w;
  if (type == NULL || tree == NULL || out_json == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&w, 0, sizeof w);
  w.alloc = tree->alloc;
  puts_(&w, "{\"format\":\"" TREE_FORMAT "\",\"root\":");
  write_item(&w, type, tree, 0, GDEFSTOR_NONE);
  puts_(&w, "}");
  if (w.rc != GDEFSTOR_OK) {
    gcu_allocator_free(tree->alloc, w.buf);
    return w.rc;
  }
  *out_json = (uint8_t *)w.buf;
  *out_len = w.len;
  return GDEFSTOR_OK;
}
