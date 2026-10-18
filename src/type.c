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
 * Type declarations: checking a spec, merging it additively into what is
 * stored, and the stored text (`defiant-type/1`), which the port writes and
 * parses and a backend keeps as given.
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/allocator.h>
#include <stdio.h>
#include <string.h>

#define TYPE_FORMAT "defiant-type/1"

static const char * const type_names[] = {"", "text", "integer", "decimal",
    "float", "boolean", "date", "datetime", "reference", "group"};

#define KNOWN_FLAGS                                                           \
  (GDEFSTOR_FIELD_MULTIPLE | GDEFSTOR_FIELD_REQUIRED | GDEFSTOR_FIELD_INDEXED | \
      GDEFSTOR_FIELD_PERSONAL)
/* What a redeclaration may change. Everything else is what the values mean. */
#define MUTABLE_FLAGS (GDEFSTOR_FIELD_REQUIRED | GDEFSTOR_FIELD_INDEXED)

static int same_span(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

static int same_fold(const char * a, size_t an, const char * b, size_t bn) {
  size_t i;
  if (an != bn) {
    return 0;
  }
  for (i = 0; i < an; i++) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') {
      x = (char)(x - 'A' + 'a');
    }
    if (y >= 'A' && y <= 'Z') {
      y = (char)(y - 'A' + 'a');
    }
    if (x != y) {
      return 0;
    }
  }
  return 1;
}

/* ---- the type ------------------------------------------------------------ */

static void field_release(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_MField * f) {
  gcu_allocator_free(alloc, f->name);
  gcu_allocator_free(alloc, f->path);
  gcu_allocator_free(alloc, f->subject);
}

void gdefstor_type_free(GDEFSTOR_Type * t) {
  size_t i;
  if (t == NULL) {
    return;
  }
  for (i = 0; i < t->n; i++) {
    field_release(t->alloc, &t->f[i]);
  }
  gcu_allocator_free(t->alloc, t->f);
  gcu_allocator_free(t->alloc, t->name);
  gcu_allocator_free(t->alloc, t->group_owner);
  gcu_allocator_free(t->alloc, t->group_ncols);
  gcu_allocator_free(t->alloc, t->col_base);
  gcu_allocator_free(t->alloc, t->col_field);
  gcu_allocator_free(t->alloc, t);
}

static GDEFSTOR_Result push_field(GDEFSTOR_Type * t, size_t * cap,
    const GDEFSTOR_MField * src, size_t * out_index) {
  GDEFSTOR_MField * f;
  if (t->n >= GDEFSTOR_MODEL_MAX_FIELDS) {
    return GDEFSTOR_ERR_LIMIT;
  }
  if (t->n == *cap) {
    size_t grown = *cap == 0 ? 16 : *cap * 2;
    GDEFSTOR_MField * next = (GDEFSTOR_MField *)gcu_allocator_realloc(
        t->alloc, t->f, grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    t->f = next;
    *cap = grown;
  }
  f = &t->f[t->n];
  *f = *src;
  f->name = gdefstor_dup(t->alloc, src->name, src->name_len);
  f->path = NULL;
  f->path_len = 0;
  f->subject = src->subject == NULL
                   ? NULL
                   : gdefstor_dup(t->alloc, src->subject, src->subject_len);
  if (f->name == NULL || (src->subject != NULL && f->subject == NULL)) {
    gcu_allocator_free(t->alloc, f->name);
    gcu_allocator_free(t->alloc, f->subject);
    return GDEFSTOR_ERR_OOM;
  }
  *out_index = t->n;
  t->n += 1;
  return GDEFSTOR_OK;
}

size_t gdefstor_model_type_child(const GDEFSTOR_Type * t, size_t parent,
    const char * name, size_t name_len) {
  size_t i;
  for (i = 0; i < t->n; i++) {
    if (t->f[i].parent == parent &&
        same_span(t->f[i].name, t->f[i].name_len, name, name_len)) {
      return i;
    }
  }
  return GDEFSTOR_NONE;
}

/* Paths, retirement, groups and the column map, from the stored members.
 * ERR_CORRUPT for a declaration whose ids, groups or columns do not fit. */
static GDEFSTOR_Result finalize(GDEFSTOR_Type * t) {
  size_t i;
  uint32_t g;
  size_t total = 0;
  if (t->next_group == 0 || t->next_group > GDEFSTOR_MODEL_MAX_FIELDS + 1) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  t->group_owner = (size_t *)gdefstor_mem(t->alloc,
      t->next_group * sizeof *t->group_owner);
  t->group_ncols = (uint32_t *)gdefstor_mem0(t->alloc,
      t->next_group * sizeof *t->group_ncols);
  t->col_base = (uint32_t *)gdefstor_mem0(t->alloc,
      t->next_group * sizeof *t->col_base);
  if (t->group_owner == NULL || t->group_ncols == NULL || t->col_base == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (g = 0; g < t->next_group; g++) {
    t->group_owner[g] = GDEFSTOR_NONE;
  }
  t->group_ncols[0] = t->root_columns;
  for (i = 0; i < t->n; i++) {
    GDEFSTOR_MField * f = &t->f[i];
    size_t j;
    int multiple = (f->flags & GDEFSTOR_FIELD_MULTIPLE) != 0;
    if (f->parent != GDEFSTOR_NONE &&
        (f->parent >= i || t->f[f->parent].type != GDEFSTOR_FIELD_GROUP)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (f->id >= t->next_id) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    for (j = 0; j < i; j++) {
      if (t->f[j].id == f->id ||
          (t->f[j].parent == f->parent &&
              same_fold(t->f[j].name, t->f[j].name_len, f->name,
                  f->name_len))) {
        return GDEFSTOR_ERR_CORRUPT;
      }
    }
    f->depth = f->parent == GDEFSTOR_NONE ? 0 : t->f[f->parent].depth + 1;
    f->eff_retired =
        f->retired || (f->parent != GDEFSTOR_NONE && t->f[f->parent].eff_retired);
    f->in_group = f->parent == GDEFSTOR_NONE ? 0 : t->f[f->parent].group;
    if (f->depth > GDEFSTOR_MODEL_MAX_DEPTH) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (f->type == GDEFSTOR_FIELD_GROUP || multiple) {
      if (f->group == 0 || f->group >= t->next_group ||
          t->group_owner[f->group] != GDEFSTOR_NONE) {
        return GDEFSTOR_ERR_CORRUPT;
      }
      t->group_owner[f->group] = i;
      t->group_ncols[f->group] =
          f->type == GDEFSTOR_FIELD_GROUP ? f->columns : 1;
      if (f->type != GDEFSTOR_FIELD_GROUP && f->column != 0) {
        return GDEFSTOR_ERR_CORRUPT;
      }
    } else if (f->group != GDEFSTOR_NO_ID) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    {
      size_t plen = f->parent == GDEFSTOR_NONE
                        ? f->name_len
                        : t->f[f->parent].path_len + 1 + f->name_len;
      char * path = (char *)gdefstor_mem(t->alloc, plen + 1);
      if (path == NULL) {
        return GDEFSTOR_ERR_OOM;
      }
      if (f->parent == GDEFSTOR_NONE) {
        memcpy(path, f->name, f->name_len);
      } else {
        const GDEFSTOR_MField * p = &t->f[f->parent];
        memcpy(path, p->path, p->path_len);
        path[p->path_len] = '.';
        memcpy(path + p->path_len + 1, f->name, f->name_len);
      }
      path[plen] = '\0';
      f->path = path;
      f->path_len = plen;
    }
  }
  for (g = 0; g < t->next_group; g++) {
    if (g > 0 && t->group_owner[g] == GDEFSTOR_NONE) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (t->group_ncols[g] > GDEFSTOR_MODEL_MAX_FIELDS) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    t->col_base[g] = (uint32_t)total;
    total += t->group_ncols[g];
  }
  t->col_field = (size_t *)gdefstor_mem(t->alloc,
      (total == 0 ? 1 : total) * sizeof *t->col_field);
  if (t->col_field == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < total; i++) {
    t->col_field[i] = GDEFSTOR_NONE;
  }
  for (i = 0; i < t->n; i++) {
    const GDEFSTOR_MField * f = &t->f[i];
    uint32_t grp;
    uint32_t col;
    if (f->type == GDEFSTOR_FIELD_GROUP) {
      continue;
    }
    if ((f->flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
      grp = f->group;
      col = 0;
    } else {
      grp = f->in_group;
      col = f->column;
    }
    if (col >= t->group_ncols[grp] ||
        t->col_field[t->col_base[grp] + col] != GDEFSTOR_NONE) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    t->col_field[t->col_base[grp] + col] = i;
  }
  for (i = 0; i < total; i++) {
    if (t->col_field[i] == GDEFSTOR_NONE) {
      return GDEFSTOR_ERR_CORRUPT;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Type * type_new(const GDEFSTOR_Allocator * alloc,
    const char * name, size_t name_len) {
  GDEFSTOR_Type * t = (GDEFSTOR_Type *)gdefstor_mem0(alloc, sizeof *t);
  if (t == NULL) {
    return NULL;
  }
  t->alloc = alloc;
  t->name = gdefstor_dup(alloc, name, name_len);
  if (t->name == NULL) {
    gcu_allocator_free(alloc, t);
    return NULL;
  }
  t->name_len = name_len;
  return t;
}

/* ---- checking a spec ----------------------------------------------------- */

static GDEFSTOR_Result check_spec(const GDEFSTOR_FieldSpec * fields,
    size_t count, size_t depth, size_t * total) {
  size_t i;
  size_t j;
  if (count > 0 && fields == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (count > GDEFSTOR_MODEL_MAX_GROUP_FIELDS) {
    return GDEFSTOR_ERR_LIMIT;
  }
  for (i = 0; i < count; i++) {
    const GDEFSTOR_FieldSpec * s = &fields[i];
    unsigned personal;
    if (s->size != sizeof(GDEFSTOR_FieldSpec) ||
        !gdefstor_model_name_ok(s->name, s->name_len) ||
        s->type < GDEFSTOR_FIELD_TEXT || s->type > GDEFSTOR_FIELD_GROUP ||
        (s->flags & ~KNOWN_FLAGS) != 0) {
      return GDEFSTOR_ERR_INVALID;
    }
    *total += 1;
    if (*total > GDEFSTOR_MODEL_MAX_FIELDS) {
      return GDEFSTOR_ERR_LIMIT;
    }
    for (j = 0; j < i; j++) {
      if (same_fold(fields[j].name, fields[j].name_len, s->name, s->name_len)) {
        return GDEFSTOR_ERR_INVALID;
      }
    }
    if (s->type == GDEFSTOR_FIELD_DECIMAL
            ? (s->scale < 0 || s->scale > GDEFSTOR_MODEL_MAX_SCALE)
            : s->scale != 0) {
      return GDEFSTOR_ERR_INVALID;
    }
    personal = s->flags & GDEFSTOR_FIELD_PERSONAL;
    if (s->type == GDEFSTOR_FIELD_GROUP) {
      GDEFSTOR_Result rc;
      /* A group holds values only through its fields. */
      if (personal != 0 || (s->flags & GDEFSTOR_FIELD_INDEXED) != 0 ||
          s->child_count == 0 || s->subject != NULL || s->subject_len != 0) {
        return GDEFSTOR_ERR_INVALID;
      }
      if (depth + 1 > GDEFSTOR_MODEL_MAX_DEPTH) {
        return GDEFSTOR_ERR_LIMIT;
      }
      rc = check_spec(s->children, s->child_count, depth + 1, total);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      continue;
    }
    if (s->child_count != 0 || s->children != NULL) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (personal == 0) {
      if (s->subject != NULL || s->subject_len != 0) {
        return GDEFSTOR_ERR_INVALID;
      }
      continue;
    }
    /* A personal value is ciphertext; indexing it is the blind index's job
     * (AD-14), which this port does not have yet. */
    if ((s->flags & GDEFSTOR_FIELD_INDEXED) != 0) {
      return GDEFSTOR_ERR_INVALID;
    }
    if (s->subject != NULL) {
      int found = 0;
      if (!gdefstor_model_name_ok(s->subject, s->subject_len)) {
        return GDEFSTOR_ERR_INVALID;
      }
      for (j = 0; j < count; j++) {
        const GDEFSTOR_FieldSpec * r = &fields[j];
        if (j != i && same_span(r->name, r->name_len, s->subject,
                          s->subject_len)) {
          if (r->type != GDEFSTOR_FIELD_REFERENCE ||
              (r->flags & (GDEFSTOR_FIELD_MULTIPLE | GDEFSTOR_FIELD_PERSONAL)) !=
                  0) {
            return GDEFSTOR_ERR_INVALID;
          }
          found = 1;
        }
      }
      if (!found) {
        return GDEFSTOR_ERR_INVALID;
      }
    } else if (s->subject_len != 0) {
      return GDEFSTOR_ERR_INVALID;
    }
  }
  return GDEFSTOR_OK;
}

/* ---- merging a spec into what is stored ---------------------------------- */

typedef struct Merge {
  GDEFSTOR_Type * t; /* the result */
  size_t cap;
  const GDEFSTOR_Type * old;
} Merge;

static GDEFSTOR_Result take_column(Merge * m, size_t parent,
    uint32_t * out_column) {
  uint32_t * count =
      parent == GDEFSTOR_NONE ? &m->t->root_columns : &m->t->f[parent].columns;
  if (*count >= GDEFSTOR_MODEL_MAX_FIELDS) {
    return GDEFSTOR_ERR_LIMIT;
  }
  *out_column = *count;
  *count += 1;
  return GDEFSTOR_OK;
}

/* Copy a retired group's descendants as they are stored. */
static GDEFSTOR_Result copy_subtree(Merge * m, size_t old_parent,
    size_t new_parent) {
  size_t i;
  for (i = 0; i < m->old->n; i++) {
    const GDEFSTOR_MField * o = &m->old->f[i];
    GDEFSTOR_MField copy;
    size_t at;
    GDEFSTOR_Result rc;
    if (o->parent != old_parent) {
      continue;
    }
    copy = *o;
    copy.parent = new_parent;
    rc = push_field(m->t, &m->cap, &copy, &at);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    if (o->type == GDEFSTOR_FIELD_GROUP) {
      rc = copy_subtree(m, i, at);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result add_new(Merge * m, const GDEFSTOR_FieldSpec * s,
    size_t parent);

static GDEFSTOR_Result add_children(Merge * m, const GDEFSTOR_FieldSpec * s,
    size_t at) {
  size_t k;
  for (k = 0; k < s->child_count; k++) {
    GDEFSTOR_Result rc = add_new(m, &s->children[k], at);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result add_new(Merge * m, const GDEFSTOR_FieldSpec * s,
    size_t parent) {
  GDEFSTOR_MField f;
  size_t at;
  GDEFSTOR_Result rc;
  memset(&f, 0, sizeof f);
  f.name = (char *)s->name;
  f.name_len = s->name_len;
  f.type = s->type;
  f.scale = s->scale;
  f.flags = s->flags;
  f.subject = (char *)s->subject;
  f.subject_len = s->subject == NULL ? 0 : s->subject_len;
  f.parent = parent;
  f.id = m->t->next_id;
  f.group = GDEFSTOR_NO_ID;
  f.column = GDEFSTOR_NO_ID;
  if (m->t->next_id == UINT32_MAX) {
    return GDEFSTOR_ERR_LIMIT;
  }
  m->t->next_id += 1;
  if (s->type == GDEFSTOR_FIELD_GROUP ||
      (s->flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
    f.group = m->t->next_group;
    m->t->next_group += 1;
    if (s->type != GDEFSTOR_FIELD_GROUP) {
      f.column = 0;
    }
  } else {
    rc = take_column(m, parent, &f.column);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  rc = push_field(m->t, &m->cap, &f, &at);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (s->type == GDEFSTOR_FIELD_GROUP) {
    return add_children(m, s, at);
  }
  return GDEFSTOR_OK;
}

static int same_subject(const GDEFSTOR_MField * o,
    const GDEFSTOR_FieldSpec * s) {
  if (o->subject == NULL || s->subject == NULL) {
    return o->subject == NULL && s->subject == NULL;
  }
  return same_span(o->subject, o->subject_len, s->subject, s->subject_len);
}

/* Merge @p spec into the stored children of @p old_parent (GDEFSTOR_NONE: the
 * root group), appending the result under @p new_parent. */
static GDEFSTOR_Result merge_group(Merge * m, size_t old_parent,
    const GDEFSTOR_FieldSpec * spec, size_t count, size_t new_parent) {
  unsigned char matched[GDEFSTOR_MODEL_MAX_GROUP_FIELDS];
  size_t siblings = 0;
  size_t i;
  size_t k;
  GDEFSTOR_Result rc;
  memset(matched, 0, sizeof matched);
  for (i = 0; m->old != NULL && i < m->old->n; i++) {
    const GDEFSTOR_MField * o = &m->old->f[i];
    const GDEFSTOR_FieldSpec * s = NULL;
    GDEFSTOR_MField copy;
    size_t at;
    if (o->parent != old_parent) {
      continue;
    }
    siblings += 1;
    for (k = 0; k < count; k++) {
      if (same_span(spec[k].name, spec[k].name_len, o->name, o->name_len)) {
        s = &spec[k];
        matched[k] = 1;
        break;
      }
    }
    copy = *o;
    copy.parent = new_parent;
    if (s == NULL) {
      copy.retired = 1;
      rc = push_field(m->t, &m->cap, &copy, &at);
      if (rc == GDEFSTOR_OK && o->type == GDEFSTOR_FIELD_GROUP) {
        rc = copy_subtree(m, i, at);
      }
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
      continue;
    }
    /* What a value means may not change: that is a new field (AD-15). */
    if (s->type != o->type || s->scale != o->scale ||
        (s->flags & ~MUTABLE_FLAGS) != (o->flags & ~MUTABLE_FLAGS) ||
        !same_subject(o, s)) {
      return GDEFSTOR_ERR_UNSUPPORTED;
    }
    copy.retired = 0;
    copy.flags = (o->flags & ~MUTABLE_FLAGS) | (s->flags & MUTABLE_FLAGS);
    rc = push_field(m->t, &m->cap, &copy, &at);
    if (rc == GDEFSTOR_OK && o->type == GDEFSTOR_FIELD_GROUP) {
      rc = merge_group(m, i, s->children, s->child_count, at);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (k = 0; k < count; k++) {
    if (matched[k]) {
      continue;
    }
    for (i = 0; m->old != NULL && i < m->old->n; i++) {
      const GDEFSTOR_MField * o = &m->old->f[i];
      if (o->parent == old_parent &&
          same_fold(o->name, o->name_len, spec[k].name, spec[k].name_len)) {
        /* site.Title beside a retired site.title: one column name in an
         * engine that folds case. */
        return GDEFSTOR_ERR_INVALID;
      }
    }
    siblings += 1;
    if (siblings > GDEFSTOR_MODEL_MAX_GROUP_FIELDS) {
      return GDEFSTOR_ERR_LIMIT;
    }
    rc = add_new(m, &spec[k], new_parent);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

/* ---- the stored text ----------------------------------------------------- */

typedef struct Text {
  const GDEFSTOR_Allocator * alloc;
  char * buf;
  size_t len;
  size_t cap;
  GDEFSTOR_Result rc;
} Text;

static void put(Text * w, const char * s) {
  if (w->rc == GDEFSTOR_OK) {
    w->rc = gdefstor_buf_append(&w->buf, &w->len, &w->cap, w->alloc, s,
        strlen(s));
  }
}

static void put_str(Text * w, const char * s, size_t n) {
  if (w->rc == GDEFSTOR_OK) {
    w->rc = gdefstor_json_escape_append(&w->buf, &w->len, &w->cap, w->alloc,
        s, n);
  }
}

static void put_num(Text * w, long long v) {
  char tmp[32];
  snprintf(tmp, sizeof tmp, "%lld", v);
  put(w, tmp);
}

static void put_fields(Text * w, const GDEFSTOR_Type * t, size_t parent) {
  size_t i;
  int first = 1;
  put(w, "[");
  for (i = 0; i < t->n; i++) {
    const GDEFSTOR_MField * f = &t->f[i];
    if (f->parent != parent) {
      continue;
    }
    if (!first) {
      put(w, ",");
    }
    first = 0;
    put(w, "{\"name\":");
    put_str(w, f->name, f->name_len);
    put(w, ",\"id\":");
    put_num(w, (long long)f->id);
    put(w, ",\"type\":\"");
    put(w, type_names[f->type]);
    put(w, "\",\"scale\":");
    put_num(w, f->scale);
    put(w, ",\"flags\":");
    put_num(w, (long long)f->flags);
    put(w, ",\"retired\":");
    put_num(w, f->retired ? 1 : 0);
    put(w, ",\"subject\":");
    if (f->subject == NULL) {
      put(w, "null");
    } else {
      put_str(w, f->subject, f->subject_len);
    }
    put(w, ",\"group\":");
    put_num(w, f->group == GDEFSTOR_NO_ID ? -1 : (long long)f->group);
    put(w, ",\"column\":");
    put_num(w, f->column == GDEFSTOR_NO_ID ? -1 : (long long)f->column);
    if (f->type == GDEFSTOR_FIELD_GROUP) {
      put(w, ",\"columns\":");
      put_num(w, (long long)f->columns);
      put(w, ",\"fields\":");
      put_fields(w, t, i);
    }
    put(w, "}");
  }
  put(w, "]");
}

GDEFSTOR_Result gdefstor_model_type_text(const GDEFSTOR_Type * t, char ** out,
    size_t * out_len) {
  Text w;
  memset(&w, 0, sizeof w);
  w.alloc = t->alloc;
  put(&w, "{\"format\":\"" TYPE_FORMAT "\",\"name\":");
  put_str(&w, t->name, t->name_len);
  put(&w, ",\"next_id\":");
  put_num(&w, (long long)t->next_id);
  put(&w, ",\"next_group\":");
  put_num(&w, (long long)t->next_group);
  put(&w, ",\"columns\":");
  put_num(&w, (long long)t->root_columns);
  put(&w, ",\"fields\":");
  put_fields(&w, t, GDEFSTOR_NONE);
  put(&w, "}");
  if (w.rc != GDEFSTOR_OK) {
    gcu_allocator_free(t->alloc, w.buf);
    return w.rc;
  }
  *out = w.buf;
  *out_len = w.len;
  return GDEFSTOR_OK;
}

/* ---- parsing the stored text --------------------------------------------- */

static GDEFSTOR_Result get_u32(const GTEXT_JSON_Value * obj, const char * key,
    int allow_none, uint32_t * out) {
  int64_t v;
  if (gtext_json_get_i64(gtext_json_object_get(obj, key, strlen(key)), &v) !=
      GTEXT_JSON_OK) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (v == -1 && allow_none) {
    *out = GDEFSTOR_NO_ID;
    return GDEFSTOR_OK;
  }
  if (v < 0 || v >= (int64_t)UINT32_MAX) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  *out = (uint32_t)v;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result parse_fields(GDEFSTOR_Type * t, size_t * cap,
    const GTEXT_JSON_Value * arr, size_t parent, size_t depth) {
  size_t i;
  size_t n;
  if (gtext_json_typeof(arr) != GTEXT_JSON_ARRAY ||
      depth > GDEFSTOR_MODEL_MAX_DEPTH) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  n = gtext_json_array_size(arr);
  if (n > GDEFSTOR_MODEL_MAX_GROUP_FIELDS) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  for (i = 0; i < n; i++) {
    const GTEXT_JSON_Value * o = gtext_json_array_get(arr, i);
    const GTEXT_JSON_Value * subj;
    GDEFSTOR_MField f;
    const char * s;
    size_t s_len;
    const char * type;
    size_t type_len;
    uint32_t u;
    size_t at;
    int k;
    GDEFSTOR_Result rc;
    if (gtext_json_typeof(o) != GTEXT_JSON_OBJECT) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    memset(&f, 0, sizeof f);
    if (gtext_json_get_string(gtext_json_object_get(o, "name", 4), &s,
            &s_len) != GTEXT_JSON_OK ||
        !gdefstor_model_name_ok(s, s_len) ||
        gtext_json_get_string(gtext_json_object_get(o, "type", 4), &type,
            &type_len) != GTEXT_JSON_OK) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    f.name = (char *)s;
    f.name_len = s_len;
    for (k = GDEFSTOR_FIELD_TEXT; k <= GDEFSTOR_FIELD_GROUP; k++) {
      if (same_span(type, type_len, type_names[k], strlen(type_names[k]))) {
        f.type = k;
      }
    }
    if (f.type == 0 || get_u32(o, "id", 0, &f.id) != GDEFSTOR_OK ||
        get_u32(o, "scale", 0, &u) != GDEFSTOR_OK ||
        get_u32(o, "flags", 0, &f.flags) != GDEFSTOR_OK ||
        get_u32(o, "group", 1, &f.group) != GDEFSTOR_OK ||
        get_u32(o, "column", 1, &f.column) != GDEFSTOR_OK) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    f.scale = (int)u;
    if ((f.flags & ~KNOWN_FLAGS) != 0 ||
        (f.type == GDEFSTOR_FIELD_DECIMAL ? u > GDEFSTOR_MODEL_MAX_SCALE
                                           : u != 0)) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    if (get_u32(o, "retired", 0, &u) != GDEFSTOR_OK || u > 1) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    f.retired = (int)u;
    subj = gtext_json_object_get(o, "subject", 7);
    if (gtext_json_typeof(subj) == GTEXT_JSON_STRING) {
      if (gtext_json_get_string(subj, &s, &s_len) != GTEXT_JSON_OK ||
          !gdefstor_model_name_ok(s, s_len) ||
          (f.flags & GDEFSTOR_FIELD_PERSONAL) == 0) {
        return GDEFSTOR_ERR_CORRUPT;
      }
      f.subject = (char *)s;
      f.subject_len = s_len;
    } else if (gtext_json_typeof(subj) != GTEXT_JSON_NULL) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    f.parent = parent;
    if (f.type == GDEFSTOR_FIELD_GROUP) {
      if (get_u32(o, "columns", 0, &f.columns) != GDEFSTOR_OK ||
          f.column != GDEFSTOR_NO_ID) {
        return GDEFSTOR_ERR_CORRUPT;
      }
    } else if ((f.flags & GDEFSTOR_FIELD_MULTIPLE) == 0 &&
               f.column == GDEFSTOR_NO_ID) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    rc = push_field(t, cap, &f, &at);
    if (rc != GDEFSTOR_OK) {
      return rc == GDEFSTOR_ERR_LIMIT ? GDEFSTOR_ERR_CORRUPT : rc;
    }
    if (f.type == GDEFSTOR_FIELD_GROUP) {
      rc = parse_fields(t, cap, gtext_json_object_get(o, "fields", 6), at,
          depth + 1);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result from_json(GTEXT_JSON_Status code) {
  if (code == GTEXT_JSON_E_OOM) {
    return GDEFSTOR_ERR_OOM;
  }
  return GDEFSTOR_ERR_CORRUPT;
}

GDEFSTOR_Result gdefstor_model_type_parse(const GDEFSTOR_Allocator * alloc,
    const uint8_t * text, size_t len, GDEFSTOR_Type ** out) {
  GTEXT_JSON_Parse_Options opt;
  GTEXT_JSON_Error err;
  GTEXT_JSON_Value * root;
  const char * s;
  size_t s_len;
  GDEFSTOR_Type * t = NULL;
  size_t cap = 0;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  memset(&err, 0, sizeof err);
  opt = gtext_json_parse_options_default();
  opt.max_depth = 2 * (GDEFSTOR_MODEL_MAX_DEPTH + 2) + 2;
  opt.dupkeys = GTEXT_JSON_DUPKEY_ERROR;
  opt.allocator = (const GTEXT_Allocator *)alloc;
  root = gtext_json_parse((const char *)text, len, &opt, &err);
  if (root == NULL) {
    rc = from_json(err.code);
    gtext_json_error_free(&err);
    return rc;
  }
  gtext_json_error_free(&err);
  if (gtext_json_typeof(root) != GTEXT_JSON_OBJECT ||
      gtext_json_get_string(gtext_json_object_get(root, "format", 6), &s,
          &s_len) != GTEXT_JSON_OK ||
      !same_span(s, s_len, TYPE_FORMAT, strlen(TYPE_FORMAT)) ||
      gtext_json_get_string(gtext_json_object_get(root, "name", 4), &s,
          &s_len) != GTEXT_JSON_OK ||
      !gdefstor_model_type_name_ok(s, s_len)) {
    gtext_json_free(root);
    return GDEFSTOR_ERR_CORRUPT;
  }
  t = type_new(alloc, s, s_len);
  if (t == NULL) {
    gtext_json_free(root);
    return GDEFSTOR_ERR_OOM;
  }
  if (get_u32(root, "next_id", 0, &t->next_id) != GDEFSTOR_OK ||
      get_u32(root, "next_group", 0, &t->next_group) != GDEFSTOR_OK ||
      get_u32(root, "columns", 0, &t->root_columns) != GDEFSTOR_OK) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  if (rc == GDEFSTOR_OK) {
    rc = parse_fields(t, &cap, gtext_json_object_get(root, "fields", 6),
        GDEFSTOR_NONE, 0);
  }
  gtext_json_free(root);
  if (rc == GDEFSTOR_OK) {
    rc = finalize(t);
  }
  if (rc != GDEFSTOR_OK) {
    gdefstor_type_free(t);
    return rc;
  }
  *out = t;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_model_type_load(GDEFSTOR_Store * store, void * txn,
    const char * tenant, size_t tenant_len, const char * name,
    size_t name_len, GDEFSTOR_Type ** out) {
  uint8_t * text = NULL;
  size_t len = 0;
  GDEFSTOR_Result rc;
  if (!gdefstor_model_type_name_ok(name, name_len)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = store->model.type_load(store->model.ctx, txn, tenant, tenant_len, name,
      name_len, &text, &len);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_type_parse(store->alloc, text, len, out);
  gcu_allocator_free(store->alloc, text);
  if (rc == GDEFSTOR_OK &&
      !same_span((*out)->name, (*out)->name_len, name, name_len)) {
    gdefstor_type_free(*out);
    *out = NULL;
    return GDEFSTOR_ERR_CORRUPT;
  }
  return rc;
}

/* ---- the public calls ---------------------------------------------------- */

GDEFSTOR_Result gdefstor_type_declare(GDEFSTOR_Write * write,
    const GDEFSTOR_TypeSpec * spec) {
  GDEFSTOR_Store * store;
  void * txn = NULL;
  GDEFSTOR_Type * old = NULL;
  Merge m;
  char * text = NULL;
  size_t text_len = 0;
  size_t total = 0;
  GDEFSTOR_Result rc;
  if (write == NULL || write->store == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  store = write->store;
  rc = gdefstor_model_ready(store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (spec == NULL || spec->size != sizeof(GDEFSTOR_TypeSpec) ||
      !gdefstor_model_type_name_ok(spec->name, spec->name_len) ||
      spec->name_len > write->limits.max_type_bytes) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = check_spec(spec->fields, spec->field_count, 0, &total);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_txn(write, &txn);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_model_type_load(store, txn, write->tenant, write->tenant_len,
      spec->name, spec->name_len, &old);
  if (rc == GDEFSTOR_ERR_INVALID) {
    old = NULL;
  } else if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&m, 0, sizeof m);
  m.old = old;
  m.t = type_new(store->alloc, spec->name, spec->name_len);
  if (m.t == NULL) {
    gdefstor_type_free(old);
    return GDEFSTOR_ERR_OOM;
  }
  if (old != NULL) {
    m.t->next_id = old->next_id;
    m.t->next_group = old->next_group;
    m.t->root_columns = old->root_columns;
  } else {
    m.t->next_id = 1;
    m.t->next_group = 1;
  }
  rc = merge_group(&m, GDEFSTOR_NONE, spec->fields, spec->field_count,
      GDEFSTOR_NONE);
  if (rc == GDEFSTOR_OK) {
    rc = finalize(m.t);
    /* finalize() checks a stored declaration; one the merge made that it
     * refuses is a bug here, not damage. */
    if (rc == GDEFSTOR_ERR_CORRUPT) {
      rc = GDEFSTOR_ERR_INTERNAL;
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_type_text(m.t, &text, &text_len);
  }
  if (rc == GDEFSTOR_OK && old != NULL) {
    char * old_text = NULL;
    size_t old_len = 0;
    rc = gdefstor_model_type_text(old, &old_text, &old_len);
    if (rc == GDEFSTOR_OK && same_span(old_text, old_len, text, text_len)) {
      /* Nothing changed: write nothing. */
      gcu_allocator_free(store->alloc, old_text);
      gcu_allocator_free(store->alloc, text);
      gdefstor_type_free(m.t);
      gdefstor_type_free(old);
      return GDEFSTOR_OK;
    }
    gcu_allocator_free(store->alloc, old_text);
  }
  if (rc == GDEFSTOR_OK) {
    rc = store->model.type_store(store->model.ctx, txn, m.t,
        (const uint8_t *)text, text_len);
    if (rc != GDEFSTOR_OK) {
      rc = gdefstor_model_break(write, rc);
    }
  }
  gcu_allocator_free(store->alloc, text);
  gdefstor_type_free(m.t);
  gdefstor_type_free(old);
  return rc;
}

GDEFSTOR_Result gdefstor_type_get(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * type, size_t type_len,
    GDEFSTOR_Type ** out_type) {
  GDEFSTOR_Result rc;
  if (store == NULL || out_type == NULL ||
      !gdefstor_span_ok(tenant, tenant_len, store->limits.max_tenant_bytes)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefstor_model_ready(store);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  return gdefstor_model_type_load(store, NULL, tenant, tenant_len, type,
      type_len, out_type);
}

size_t gdefstor_type_field_count(const GDEFSTOR_Type * type) {
  return type == NULL ? 0 : type->n;
}

GDEFSTOR_Result gdefstor_type_field(const GDEFSTOR_Type * type, size_t index,
    GDEFSTOR_FieldInfo * out) {
  const GDEFSTOR_MField * f;
  if (type == NULL || out == NULL || out->size != sizeof *out ||
      index >= type->n) {
    return GDEFSTOR_ERR_INVALID;
  }
  f = &type->f[index];
  out->name = f->name;
  out->name_len = f->name_len;
  out->path = f->path;
  out->path_len = f->path_len;
  out->type = f->type;
  out->scale = f->scale;
  out->flags = f->flags;
  out->retired = f->eff_retired;
  out->subject = f->subject;
  out->subject_len = f->subject_len;
  out->parent = f->parent;
  out->id = f->id;
  out->group = f->group;
  out->column = f->type == GDEFSTOR_FIELD_GROUP ? GDEFSTOR_NO_ID : f->column;
  out->in_group = f->in_group;
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefstor_type_find(const GDEFSTOR_Type * type,
    const char * path, size_t path_len, size_t * out_index) {
  size_t i;
  if (type == NULL || out_index == NULL || path == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; i < type->n; i++) {
    if (same_span(type->f[i].path, type->f[i].path_len, path, path_len)) {
      *out_index = i;
      return GDEFSTOR_OK;
    }
  }
  return GDEFSTOR_ERR_INVALID;
}
