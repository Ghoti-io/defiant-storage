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
 * Cross-file state for the entity port (model.h). Not installed.
 */

#ifndef GHOTI_IO_GDEFSTOR_SRC_MODEL_INTERNAL_H
#define GHOTI_IO_GDEFSTOR_SRC_MODEL_INTERNAL_H

#include <ghoti.io/defiant-storage/macros.h>
#include "internal.h"
#include <stddef.h>
#include <stdint.h>

/* Renames for symbols that are not part of the installed API. */
#define gdefstor_model_id_ok GHOTIIO_DEFSTOR(gdefstor_model_id_ok)
#define gdefstor_model_name_ok GHOTIIO_DEFSTOR(gdefstor_model_name_ok)
#define gdefstor_model_lang_ok GHOTIIO_DEFSTOR(gdefstor_model_lang_ok)
#define gdefstor_model_type_name_ok GHOTIIO_DEFSTOR(gdefstor_model_type_name_ok)
#define gdefstor_model_utf8_ok GHOTIIO_DEFSTOR(gdefstor_model_utf8_ok)
#define gdefstor_model_value_check GHOTIIO_DEFSTOR(gdefstor_model_value_check)
#define gdefstor_model_value_equal GHOTIIO_DEFSTOR(gdefstor_model_value_equal)
#define gdefstor_model_value_compare \
  GHOTIIO_DEFSTOR(gdefstor_model_value_compare)
#define gdefstor_model_value_copy GHOTIIO_DEFSTOR(gdefstor_model_value_copy)
#define gdefstor_model_value_clear GHOTIIO_DEFSTOR(gdefstor_model_value_clear)
#define gdefstor_model_type_parse GHOTIIO_DEFSTOR(gdefstor_model_type_parse)
#define gdefstor_model_type_load GHOTIIO_DEFSTOR(gdefstor_model_type_load)
#define gdefstor_model_type_child GHOTIIO_DEFSTOR(gdefstor_model_type_child)
#define gdefstor_model_tree_push GHOTIIO_DEFSTOR(gdefstor_model_tree_push)
#define gdefstor_model_tree_put GHOTIIO_DEFSTOR(gdefstor_model_tree_put)
#define gdefstor_model_from_json GHOTIIO_DEFSTOR(gdefstor_model_from_json)
#define gdefstor_model_json_ok GHOTIIO_DEFSTOR(gdefstor_model_json_ok)
#define gdefstor_model_draft_pack GHOTIIO_DEFSTOR(gdefstor_model_draft_pack)
#define gdefstor_model_draft_unpack GHOTIIO_DEFSTOR(gdefstor_model_draft_unpack)
#define gdefstor_model_lang_cmp GHOTIIO_DEFSTOR(gdefstor_model_lang_cmp)
#define gdefstor_model_sort GHOTIIO_DEFSTOR(gdefstor_model_sort)
#define gdefstor_query_result_adopt GHOTIIO_DEFSTOR(gdefstor_query_result_adopt)
#define gdefstor_model_ready GHOTIIO_DEFSTOR(gdefstor_model_ready)
#define gdefstor_model_txn GHOTIIO_DEFSTOR(gdefstor_model_txn)
#define gdefstor_model_break GHOTIIO_DEFSTOR(gdefstor_model_break)
#define gdefstor_model_type_text GHOTIIO_DEFSTOR(gdefstor_model_type_text)
#define gdefstor_model_has_blind GHOTIIO_DEFSTOR(gdefstor_model_has_blind)
#define gdefstor_model_has_personal GHOTIIO_DEFSTOR(gdefstor_model_has_personal)
#define gdefstor_model_has_sys GHOTIIO_DEFSTOR(gdefstor_model_has_sys)

/* "No field": the parent of a root-group field, an unset index. */
#define GDEFSTOR_NONE SIZE_MAX
/* "No group" or "no column". */
#define GDEFSTOR_NO_ID UINT32_MAX

/* One field of a declaration, flattened in pre-order. */
typedef struct GDEFSTOR_MField {
  char * name;
  size_t name_len;
  char * path;        /* names from the root, joined by '.' */
  size_t path_len;
  int type;
  int scale;
  unsigned flags;
  int retired;        /* this field, as stored */
  int eff_retired;    /* it or an enclosing group */
  char * subject;     /* PERSONAL: the sibling reference field, or NULL */
  size_t subject_len;
  size_t parent;      /* index of the enclosing GROUP, or GDEFSTOR_NONE */
  uint32_t id;
  uint32_t group;     /* GROUP or MULTIPLE: its group; else GDEFSTOR_NO_ID */
  uint32_t column;    /* single scalar: its column; multiple scalar: 0 */
  uint32_t in_group;  /* the group holding it */
  uint32_t columns;   /* GROUP: the columns its group has had */
  size_t depth;       /* groups enclosing it */
} GDEFSTOR_MField;

struct GDEFSTOR_Type {
  const GDEFSTOR_Allocator * alloc;
  char * name;
  size_t name_len;
  GDEFSTOR_MField * f;
  size_t n;
  uint32_t next_id;
  uint32_t next_group;
  uint32_t root_columns;
  /* Derived on parse. */
  size_t * group_owner;   /* [group]: the field whose items it holds; root: NONE */
  uint32_t * group_ncols; /* [group]: its columns */
  uint32_t * col_base;    /* [group]: the first entry of col_field for it */
  size_t * col_field;     /* the field of each column of each group */
};

/* A blind-index MAC, held by value (no allocation): a tree's, or one read
 * back from a backend's blind row. */
typedef struct GDEFSTOR_BlindVal {
  int set;
  uint8_t mac[GDEFSTOR_BLIND_MAC_BYTES];
  uint32_t key_id;
  uint32_t version;
} GDEFSTOR_BlindVal;

/* One value set on an item. Its bytes and subject are owned. */
typedef struct GDEFSTOR_TreeVal {
  char * field;
  size_t field_len;
  GDEFSTOR_Value v;
  GDEFSTOR_BlindVal blind; /* gdefstor_tree_set_blind(); dropped by a set */
} GDEFSTOR_TreeVal;

typedef struct GDEFSTOR_TreeNode {
  uint64_t id;
  size_t parent;
  char * field;
  size_t field_len;
  double order;
  GDEFSTOR_TreeVal * vals;
  size_t nvals;
  size_t capvals;
  size_t * kids;
  size_t nkids;
  size_t capkids;
} GDEFSTOR_TreeNode;

struct GDEFSTOR_Tree {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_TreeNode * items;
  size_t n;
  size_t cap;
  size_t bytes; /* every value's bytes together */
};

/* `[0-9A-Za-z_-]`, 1 to @p cap bytes, or empty when @p allow_empty. */
int gdefstor_model_id_ok(const char * p, size_t n, size_t cap,
    int allow_empty);

/* A language tag: `[A-Za-z0-9-]{1,35}` (model.h). */
int gdefstor_model_lang_ok(const char * p, size_t n);

/* A field name: `[A-Za-z0-9_]{1,64}`. */
int gdefstor_model_name_ok(const char * p, size_t n);

/* A type's machine name: `<module>.<name>`, 3 to 128 bytes (AD-35). */
int gdefstor_model_type_name_ok(const char * p, size_t n);

/* Well-formed UTF-8 with no NUL, no surrogate and no overlong form. */
int gdefstor_model_utf8_ok(const uint8_t * p, size_t n);

/* A value (type != 0) fit for field @p f: ERR_INVALID or ERR_LIMIT. */
GDEFSTOR_Result gdefstor_model_value_check(const GDEFSTOR_MField * f,
    const GDEFSTOR_Value * v, size_t id_cap);

/* The same value, subject included. */
int gdefstor_model_value_equal(const GDEFSTOR_Value * a,
    const GDEFSTOR_Value * b);

/* The order of two values of one type: negative, 0, positive. No value
 * (type 0) is before every value. */
int gdefstor_model_value_compare(const GDEFSTOR_Value * a,
    const GDEFSTOR_Value * b);

/* A copy that owns its bytes; -0.0 becomes 0.0. */
GDEFSTOR_Result gdefstor_model_value_copy(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_Value * dst, const GDEFSTOR_Value * src);

/* Free what gdefstor_model_value_copy() allocated and zero the value. */
void gdefstor_model_value_clear(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_Value * v);

/* Parse a stored declaration; ERR_CORRUPT for one that is not well formed. */
GDEFSTOR_Result gdefstor_model_type_parse(const GDEFSTOR_Allocator * alloc,
    const uint8_t * text, size_t len, GDEFSTOR_Type ** out);

/* Load and parse a type through the backend; ERR_INVALID when undeclared. */
GDEFSTOR_Result gdefstor_model_type_load(GDEFSTOR_Store * store, void * txn,
    const char * tenant, size_t tenant_len, const char * name,
    size_t name_len, GDEFSTOR_Type ** out);

/* The child of group-field @p parent (GDEFSTOR_NONE: the root group) named
 * @p name, or GDEFSTOR_NONE. */
size_t gdefstor_model_type_child(const GDEFSTOR_Type * t, size_t parent,
    const char * name, size_t name_len);

/* Append an item with a known order key, for trees read from storage. */
GDEFSTOR_Result gdefstor_model_tree_push(GDEFSTOR_Tree * tree, size_t parent,
    const char * field, size_t field_len, uint64_t id, double order,
    size_t * out_item);

/* Set a value without the caps a caller's tree is held to. */
GDEFSTOR_Result gdefstor_model_tree_put(GDEFSTOR_Tree * tree, size_t item,
    const char * field, size_t field_len, const GDEFSTOR_Value * value);

/* Parse a JSON tree with the given allocator; the work behind
 * gdefstor_tree_from_json(). */
GDEFSTOR_Result gdefstor_model_from_json(const GDEFSTOR_Type * type,
    const uint8_t * json, size_t len, const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_Tree ** out_tree);

/* Well-formed JSON whose format is `defiant-tree/1`: what a draft put checks
 * before the type is consulted at apply. */
GDEFSTOR_Result gdefstor_model_json_ok(const GDEFSTOR_Allocator * alloc,
    const uint8_t * json, size_t len);

/* Two language tags in byte order: negative, 0, positive. */
int gdefstor_model_lang_cmp(const char * a, size_t an, const char * b,
    size_t bn);

/* One language of a stored draft: its tag, and where its tree's JSON is in
 * the draft document. */
typedef struct GDEFSTOR_DraftPart {
  char lang[GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES];
  size_t lang_len;
  size_t off;
  size_t len;
} GDEFSTOR_DraftPart;

/* The draft document (`defiant-draft/1`, model.md) of @p count trees already
 * checked and sorted by language: ERR_LIMIT past the draft limit. */
GDEFSTOR_Result gdefstor_model_draft_pack(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_DraftTree * const * trees, size_t count, uint8_t ** out,
    size_t * out_len);

/* The languages of a stored draft document, in order: ERR_CORRUPT for one
 * this port did not write, ERR_UNSUPPORTED for a newer version. */
GDEFSTOR_Result gdefstor_model_draft_unpack(const GDEFSTOR_Allocator * alloc,
    const uint8_t * json, size_t len, GDEFSTOR_DraftPart ** out,
    size_t * out_count);

/* A stable merge sort of the indexes 0..n-1 by @p cmp (C's qsort takes no
 * context). The array comes from @p alloc. */
GDEFSTOR_Result gdefstor_model_sort(const GDEFSTOR_Allocator * alloc, size_t n,
    int (*cmp)(const void * ctx, size_t a, size_t b), const void * ctx,
    size_t ** out);

/* Build a query result from a backend's hits, taking their public ids. */
GDEFSTOR_Result gdefstor_query_result_adopt(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_ModelHits * hits, GDEFSTOR_QueryResult ** out);

/* ERR_UNSUPPORTED unless the store has the entity port. */
GDEFSTOR_Result gdefstor_model_ready(const GDEFSTOR_Store * store);

/* The write's entity transaction, opened on first use. ERR_INVALID for a
 * write that is not alive or has key/bytes rows staged; the broken error for
 * a write already broken. */
GDEFSTOR_Result gdefstor_model_txn(GDEFSTOR_Write * write, void ** out_txn);

/* Mark the write broken by @p rc (an error after the backend was written). */
GDEFSTOR_Result gdefstor_model_break(GDEFSTOR_Write * write,
    GDEFSTOR_Result rc);

/* The canonical stored text of a declaration. */
GDEFSTOR_Result gdefstor_model_type_text(const GDEFSTOR_Type * t,
    char ** out, size_t * out_len);

/* Whether the store's backend serves the blind index (model_backend.h). */
int gdefstor_model_has_blind(const GDEFSTOR_Store * store);

/* Whether the store's backend keeps records, counters and the outbox
 * (model_backend.h). */
int gdefstor_model_has_sys(const GDEFSTOR_Store * store);

/* Whether a declaration has a personal field, retired ones included. */
int gdefstor_model_has_personal(const GDEFSTOR_Type * t);

#endif /* GHOTI_IO_GDEFSTOR_SRC_MODEL_INTERNAL_H */
