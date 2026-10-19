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
 * @file query.h
 * @brief A backend-neutral query over the rows of one projection type.
 * @stability free
 *
 * A query is **data, not code**: a bounded tree of predicates, a sort, an
 * offset, a limit and the columns to return. The caller builds the tree in its
 * own memory and the port reads it during one call. Nothing in it is ever
 * spliced into SQL: a backend that speaks SQL builds its statement from a fixed
 * vocabulary, and every constant, and the page, reach it as a bound value.
 *
 * ## What a predicate sees
 *
 * Every operand is a **set of texts** (byte strings):
 *
 * - ::GDEFSTOR_QUERY_CONST: the listed texts. Zero texts is the empty set.
 * - ::GDEFSTOR_QUERY_COLUMN: a column of the row, or of another projection
 *   type's row with the same entity argument (`join_type`). A column that is
 *   not there, and a joined row that is not there, is the empty set. With a
 *   path, the column is a JSON document, an object of objects whose leaves are
 *   arrays of strings (`{"en":{"title":["a","b"]}}`), and the operand is the
 *   array at `path1` then `path2`; anything missing is the empty set.
 *   ::GDEFSTOR_QUERY_EMPTY_IF_NONE turns an empty set into the single empty
 *   text, which is how a field with no value reads.
 * - ::GDEFSTOR_QUERY_ENTITY: the row's entity argument.
 *
 * `=` holds when some text on the left equals some text on the right, byte for
 * byte; `!=` holds exactly when `=` does not; `in` is `=` against a constant
 * list. `<`, `<=`, `>` and `>=` hold when some pair of texts are both integers
 * (`-?[0-9]{1,18}`, so "07" is 7) and stand in the relation; a text that is
 * not an integer makes its pair false, never an error. `and`, `or` and `not`
 * are the usual truth tables, and an `and` or `or` with no children is true
 * and false.
 *
 * ## A semi-join
 *
 * ::GDEFSTOR_QUERY_EXISTS holds on a row when some row of another projection
 * type (`exists_type`, any entity) satisfies its one child, which is a
 * predicate of that other row. Operands of the child flagged
 * ::GDEFSTOR_QUERY_OUTER are of the row EXISTS is being decided for, so the
 * child can say "the other row's column equals this row's column". An
 * EXISTS inside an EXISTS is refused, and so is an OUTER operand outside one.
 * A relation read as a condition is one of these.
 *
 * ## Order and pages
 *
 * Rows are ordered by the sort keys, then by the entity argument, which is
 * the final tiebreak, so a page is always the same slice of one total order.
 * A sort key is the first text of its operand (none sorts before every text
 * including the empty one). A numeric key puts a text that is not an integer
 * where "none" is, ahead of every integer, tied with the others like it. The
 * entity tiebreak is by bytes, or by integer value (then bytes) when
 * GDEFSTOR_Query::entity_numeric is set. A limit above
 * ::GDEFSTOR_QUERY_LIMIT_MAX is clamped to it; a limit of 0 asks for every
 * match. The total is the number of matches before the offset and the limit.
 *
 * ## Limits
 *
 * A tree deeper than ::GDEFSTOR_QUERY_MAX_DEPTH, with more than
 * ::GDEFSTOR_QUERY_MAX_NODES nodes, or a constant larger than the caps below
 * is refused with ::GDEFSTOR_ERR_LIMIT. A shape that makes no sense is
 * ::GDEFSTOR_ERR_INVALID. A row that cannot be read (a column table or a JSON
 * document that is damaged) is ::GDEFSTOR_ERR_CORRUPT and no partial answer is
 * returned.
 *
 * The query sees the last commit. It checks no permission: the caller of the
 * port is the trusted layer, and a caller that serves a visitor puts the
 * visitor's access filter into the tree.
 *
 * ## Over entities
 *
 * gdefstor_entity_query() and gdefstor_entity_count() ask the same form of
 * the entities of one declared type (model.h), over their current trees. The
 * operands are typed there, and the text operands above are refused:
 *
 * - ::GDEFSTOR_QUERY_FIELD: a field path (`lines.taxes.amount`) to an active,
 *   non-personal scalar field. Its set is that field's values on every current
 *   item that has one, so a comparison on a field inside a multiple, or inside
 *   a group inside one, holds when **some** item matches. Two comparisons are
 *   each decided on their own: `lines.sku = 'A' and lines.qty > 5` does not
 *   say it is the same line. An absent value adds nothing.
 * - ::GDEFSTOR_QUERY_VALUES: typed constants (GDEFSTOR_Value), all of one type.
 * - ::GDEFSTOR_QUERY_META: a fact of the entity record (::GDEFSTOR_QueryMeta).
 * - ::GDEFSTOR_QUERY_ENTITY: the entity's public id, typed as a reference.
 *
 * The two sides of a comparison have the same type (a reference compares with
 * a reference or the public id; a decimal with a decimal of the same scale).
 * `=`, `!=` and `in` take any type; `<`, `<=`, `>`, `>=` take integer, decimal,
 * float, date and date-time, and compare numbers, not text. EXISTS names
 * another declared type; its child is a predicate over that type's entities,
 * and an ::GDEFSTOR_QUERY_OUTER operand reads the entity EXISTS is decided
 * for. A sort key is a single scalar field of the root group, a META fact or
 * the public id; no value sorts before every value; text sorts by bytes. The
 * final tiebreak is the internal row, ascending even under a descending sort,
 * which is creation order. The total counts every match before the offset and
 * the limit. Flags other than ::GDEFSTOR_QUERY_OUTER, a select, `numeric` on a
 * sort and `entity_numeric` are ::GDEFSTOR_ERR_INVALID, and so is a type that
 * was never declared.
 *
 * A personal field holds ciphertext, so the comparisons above refuse it; it is
 * matched only by ::GDEFSTOR_QUERY_BLIND, blind-index equality: the left
 * operand a FIELD naming an active personal scalar field (no OUTER), the
 * right a ::GDEFSTOR_QUERY_CONST of exactly one text of
 * ::GDEFSTOR_BLIND_MAC_BYTES bytes. It holds when some current item of the
 * field's group has a value whose current version has a blind row with that
 * MAC (gdefstor_tree_set_blind()), so "some item matches" for multiples and
 * groups, and an older version's MAC does not match. It composes with every
 * other node, inside an EXISTS too. A personal field is never a sort key and
 * never compared by range. On a store whose backend has no blind table the
 * node is ::GDEFSTOR_ERR_UNSUPPORTED.
 */

#ifndef GHOTI_IO_GDEFSTOR_QUERY_H
#define GHOTI_IO_GDEFSTOR_QUERY_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/store.h>
#include <ghoti.io/defiant-storage/projection.h>
#include <ghoti.io/defiant-storage/value.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Nesting of the predicate tree, the root being depth 1. */
#define GDEFSTOR_QUERY_MAX_DEPTH 64
/** Nodes in the tree, counted as the tree is walked. */
#define GDEFSTOR_QUERY_MAX_NODES 8192
/** Texts in one constant operand. */
#define GDEFSTOR_QUERY_MAX_TEXTS 256
/** Bytes in one text of a constant operand. */
#define GDEFSTOR_QUERY_MAX_TEXT_BYTES 65536
/** Sort keys. */
#define GDEFSTOR_QUERY_MAX_SORTS 4
/** Columns returned with each row. */
#define GDEFSTOR_QUERY_MAX_SELECT 8
/** The largest page. A larger limit is clamped to this. */
#define GDEFSTOR_QUERY_LIMIT_MAX 1000

/** An operand's flag: an empty set is the single empty text. */
#define GDEFSTOR_QUERY_EMPTY_IF_NONE 1
/** An operand's flag, inside an EXISTS: the operand is of the row the EXISTS
 * is being decided for (the outer row), not of the row it looks at. */
#define GDEFSTOR_QUERY_OUTER 2

/** One text. Borrowed for the call that takes it. */
typedef struct GDEFSTOR_QueryText {
  const uint8_t * bytes; ///< May be NULL when @ref len is 0.
  size_t len;            ///< Length of @ref bytes.
} GDEFSTOR_QueryText;

/** What an operand is. */
typedef enum GDEFSTOR_QueryKind {
  GDEFSTOR_QUERY_CONST = 1, ///< The texts listed.
  GDEFSTOR_QUERY_COLUMN,    ///< A column, see the file comment.
  GDEFSTOR_QUERY_ENTITY,    ///< The entity argument of the row (an entity's public id).
  GDEFSTOR_QUERY_FIELD,     ///< Entities only: the values of a field path.
  GDEFSTOR_QUERY_VALUES,    ///< Entities only: typed constants.
  GDEFSTOR_QUERY_META       ///< Entities only: a fact of the entity record.
} GDEFSTOR_QueryKind;

/** The facts of an entity record a ::GDEFSTOR_QUERY_META operand reads. */
typedef enum GDEFSTOR_QueryMeta {
  GDEFSTOR_META_PUBLISHED = 1, ///< Boolean: the newest state.
  GDEFSTOR_META_DELETED,       ///< Boolean: the newest state (soft delete).
  GDEFSTOR_META_CREATOR,       ///< Reference: who created it; none for the system.
  GDEFSTOR_META_CREATED,       ///< Date-time: when.
  GDEFSTOR_META_REVISION       ///< Integer: the newest revision, 0 for none.
} GDEFSTOR_QueryMeta;

/** One side of a comparison, a sort key or a returned column. */
typedef struct GDEFSTOR_QueryOperand {
  int kind;                   ///< A ::GDEFSTOR_QueryKind.
  int flags;                  ///< ::GDEFSTOR_QUERY_EMPTY_IF_NONE or 0.
  const char * column;        ///< COLUMN: the column name.
  size_t column_len;          ///< Length of @ref column.
  const char * join_type;     ///< COLUMN: another projection type, or NULL.
  size_t join_type_len;       ///< Length of @ref join_type.
  int path_depth;             ///< COLUMN: 0 (plain), or 2 (JSON, two keys).
  const char * path1;         ///< The first key when @ref path_depth is 2.
  size_t path1_len;           ///< Length of @ref path1.
  const char * path2;         ///< The second key when @ref path_depth is 2.
  size_t path2_len;           ///< Length of @ref path2.
  const GDEFSTOR_QueryText * texts; ///< CONST: the texts.
  size_t text_count;          ///< CONST: how many.
  const char * field;         ///< FIELD: a dotted path of field names.
  size_t field_len;           ///< Length of @ref field.
  const GDEFSTOR_Value * values; ///< VALUES: the constants.
  size_t value_count;         ///< VALUES: how many, at most ::GDEFSTOR_QUERY_MAX_TEXTS.
  int meta;                   ///< META: a ::GDEFSTOR_QueryMeta.
} GDEFSTOR_QueryOperand;

/** What a node asks. */
typedef enum GDEFSTOR_QueryOp {
  GDEFSTOR_QUERY_TRUE = 1, ///< Holds.
  GDEFSTOR_QUERY_FALSE,    ///< Does not hold.
  GDEFSTOR_QUERY_AND,      ///< Every child holds.
  GDEFSTOR_QUERY_OR,       ///< Some child holds.
  GDEFSTOR_QUERY_NOT,      ///< The one child does not hold.
  GDEFSTOR_QUERY_EQ,       ///< Some left text equals some right text.
  GDEFSTOR_QUERY_NE,       ///< No left text equals any right text.
  GDEFSTOR_QUERY_IN,       ///< EQ against a constant list (right is CONST).
  GDEFSTOR_QUERY_LT,       ///< Some integer pair, left < right.
  GDEFSTOR_QUERY_LE,       ///< Some integer pair, left <= right.
  GDEFSTOR_QUERY_GT,       ///< Some integer pair, left > right.
  GDEFSTOR_QUERY_GE,       ///< Some integer pair, left >= right.
  GDEFSTOR_QUERY_EXISTS,   ///< Some row of another type satisfies the child.
  /** Entities only: blind-index equality. Left is a FIELD naming a personal
   * field, right a CONST of one text, the ::GDEFSTOR_BLIND_MAC_BYTES bytes of
   * a MAC: holds when some current value version of the field has a blind
   * row with that MAC ("Over entities"). */
  GDEFSTOR_QUERY_BLIND
} GDEFSTOR_QueryOp;

/** A node of the predicate tree. */
typedef struct GDEFSTOR_QueryNode {
  int op;                                   ///< A ::GDEFSTOR_QueryOp.
  GDEFSTOR_QueryOperand left;               ///< Comparisons only.
  GDEFSTOR_QueryOperand right;              ///< Comparisons only.
  const struct GDEFSTOR_QueryNode * const * children; ///< AND, OR, NOT, EXISTS.
  size_t child_count;                       ///< How many children.
  const char * exists_type;                 ///< EXISTS: the projection type.
  size_t exists_type_len;                   ///< Length of @ref exists_type.
} GDEFSTOR_QueryNode;

/** One sort key. */
typedef struct GDEFSTOR_QuerySort {
  GDEFSTOR_QueryOperand key; ///< COLUMN or ENTITY.
  int numeric;               ///< Compare as integers.
  int descending;            ///< Largest first.
} GDEFSTOR_QuerySort;

/** A query. Begins with @ref size. */
typedef struct GDEFSTOR_Query {
  size_t size;                            ///< `sizeof(GDEFSTOR_Query)`.
  const GDEFSTOR_QueryNode * where;       ///< NULL is every row.
  const GDEFSTOR_QuerySort * sorts;       ///< Sort keys, or NULL.
  size_t sort_count;                      ///< How many.
  int entity_numeric;                     ///< Final tiebreak as integers.
  uint64_t offset;                        ///< Matches skipped.
  uint64_t limit;                         ///< Rows returned, 0 for all.
  const GDEFSTOR_QueryOperand * select;   ///< Columns to return, or NULL.
  size_t select_count;                    ///< How many.
} GDEFSTOR_Query;

/** The rows of a query. Opaque. */
typedef struct GDEFSTOR_QueryResult GDEFSTOR_QueryResult;

/**
 * @brief Run a query over the committed rows of one projection type.
 *
 * @param store The store.
 * @param tenant Tenant bytes.
 * @param tenant_len Its length.
 * @param type The projection type.
 * @param type_len Its length.
 * @param query The query.
 * @param out_result Receives the result on success.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, ::GDEFSTOR_ERR_LIMIT,
 *   ::GDEFSTOR_ERR_CORRUPT, ::GDEFSTOR_ERR_OOM, or a backend's error. An
 *   unknown tenant or type is success with no rows.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_projection_query(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_Query * query, GDEFSTOR_QueryResult ** out_result);

/**
 * @brief Count the committed rows of one projection type that match.
 *
 * The same predicate and the same refusals as gdefstor_projection_query(); the
 * sort, offset, limit and select are not looked at.
 *
 * @param where The predicate, or NULL for every row.
 * @param out_count Receives the number of matches.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_projection_count(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_QueryNode * where, uint64_t * out_count);

/** @return How many rows this page holds. */
GDEFSTOR_API size_t gdefstor_query_result_rows(const GDEFSTOR_QueryResult * r);

/** @return The matches before the offset and the limit. */
GDEFSTOR_API uint64_t gdefstor_query_result_total(
    const GDEFSTOR_QueryResult * r);

/**
 * @brief The entity argument of a row. Valid until the result is freed.
 *
 * @return ::GDEFSTOR_OK or ::GDEFSTOR_ERR_INVALID for an index out of range.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_query_result_entity(
    const GDEFSTOR_QueryResult * r, size_t row, const char ** out_entity,
    size_t * out_len);

/**
 * @brief One returned column of a row: the first text of the select operand.
 *
 * @param select Index into GDEFSTOR_Query::select.
 * @param out_present Set to 0 when the operand had no text.
 * @return ::GDEFSTOR_OK or ::GDEFSTOR_ERR_INVALID for an index out of range.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_query_result_value(
    const GDEFSTOR_QueryResult * r, size_t row, size_t select,
    int * out_present, const uint8_t ** out_bytes, size_t * out_len);

/** @brief Release a result. NULL is ignored. */
GDEFSTOR_API void gdefstor_query_result_free(GDEFSTOR_QueryResult * r);

/**
 * @brief Run a query over the current entities of one declared type.
 *
 * See "Over entities" above. gdefstor_query_result_entity() gives each
 * entity's public id; there are no select columns.
 *
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, ::GDEFSTOR_ERR_LIMIT,
 *   ::GDEFSTOR_ERR_UNSUPPORTED (a store without the entity port),
 *   ::GDEFSTOR_ERR_OOM, or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_query(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_Query * query, GDEFSTOR_QueryResult ** out_result);

/**
 * @brief Count the current entities of one declared type that match.
 *
 * The same predicate and refusals as gdefstor_entity_query().
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_count(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_QueryNode * where, uint64_t * out_count);

/* ---- for the authors of a backend --------------------------------------- */

/**
 * What a backend that answers queries itself is handed: the tenant, the key
 * prefix of every row of the type, and a query the port has already checked.
 * A backend that does not set GDEFSTOR_Backend::query is answered by the
 * port's own scan, which reads every row of the type through `list` and `get`.
 */
typedef struct GDEFSTOR_QueryPlan {
  const char * type;          ///< The projection type.
  size_t type_len;            ///< Its length.
  const char * prefix;        ///< Key prefix shared by every row of the type.
  size_t prefix_len;          ///< Its length.
  const GDEFSTOR_Query * query; ///< Checked: shape, limits, select.
  uint64_t limit;             ///< The page size after clamping, UINT64_MAX for all.
  int count_only;             ///< Only @ref GDEFSTOR_QueryRows::total is wanted.
} GDEFSTOR_QueryPlan;

/**
 * What a backend writes back: the page, in order, and the total. Arrays and
 * their elements come from the store's allocator; the port frees them. The
 * entity is the decoded argument, and the row is the encoded projection (the
 * port decodes the columns to return from it).
 */
typedef struct GDEFSTOR_QueryRows {
  uint64_t total;       ///< Matches before the offset and the limit.
  size_t count;         ///< Rows in the page.
  char ** entities;     ///< Entity arguments, `count` of them.
  size_t * entity_lens; ///< Their lengths.
  uint8_t ** rows;      ///< Encoded rows, `count` of them.
  size_t * row_lens;    ///< Their lengths.
} GDEFSTOR_QueryRows;

/**
 * @brief The key prefix of every row of a projection type, colon included.
 *
 * @param alloc The store's allocator, or NULL.
 * @param out_prefix Receives a fresh string; free it with the allocator.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_projection_key_prefix(
    const GDEFSTOR_Allocator * alloc, const char * type, size_t type_len,
    char ** out_prefix, size_t * out_len);

/** @brief The key of one projection row. Free with the allocator. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_projection_key(
    const GDEFSTOR_Allocator * alloc, const char * type, size_t type_len,
    const char * entity, size_t entity_len, char ** out_key, size_t * out_len);

/**
 * @brief The entity argument inside a key that starts with the @p prefix_len
 * bytes of gdefstor_projection_key_prefix().
 *
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_CORRUPT for a key that is not hex.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_projection_key_entity(
    const GDEFSTOR_Allocator * alloc, const char * key, size_t key_len,
    size_t prefix_len, char ** out_entity, size_t * out_len);

/** One encoded row, borrowed. `bytes` NULL is a row that is not there. */
typedef struct GDEFSTOR_QueryRowRef {
  const uint8_t * bytes; ///< The encoded projection, or NULL when absent.
  size_t len;            ///< Its length.
} GDEFSTOR_QueryRowRef;

/**
 * The rows a node can look at: the queried row, the row of the left
 * operand's `join_type` and that of the right's, all with this entity.
 */
typedef struct GDEFSTOR_QueryEnv {
  GDEFSTOR_QueryRowRef main;       ///< The queried row (inside an EXISTS, the other type's).
  GDEFSTOR_QueryRowRef left_join;  ///< The left operand's joined row, of its own row's entity.
  GDEFSTOR_QueryRowRef right_join; ///< The right operand's joined row, likewise.
  const uint8_t * entity;          ///< The entity argument.
  size_t entity_len;               ///< Its length.
  GDEFSTOR_QueryRowRef outer;      ///< Inside an EXISTS: the row it is decided for.
  const uint8_t * outer_entity;    ///< Inside an EXISTS: that row's entity argument.
  size_t outer_entity_len;         ///< Its length.
} GDEFSTOR_QueryEnv;

/**
 * @brief Whether a comparison node holds on a row.
 *
 * The reference meaning of EQ, NE, IN, LT, LE, GT and GE. The tree walk above
 * it is the backend's. The constants are taken from the node.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_query_leaf(
    const GDEFSTOR_Allocator * alloc, const GDEFSTOR_QueryNode * node,
    const GDEFSTOR_QueryEnv * env, int * out_holds);

/** A sort key, computed. */
typedef struct GDEFSTOR_QueryKey {
  int present;        ///< 0 is "none", which sorts first.
  int is_int;         ///< The key is @ref value.
  int64_t value;      ///< A numeric key that parsed.
  uint8_t * bytes;    ///< A text key, owned.
  size_t len;         ///< Its length.
} GDEFSTOR_QueryKey;

/** @brief Compute the key of one sort for a row. Free with gdefstor_query_key_free(). */
GDEFSTOR_API GDEFSTOR_Result gdefstor_query_key_make(
    const GDEFSTOR_Allocator * alloc, const GDEFSTOR_QuerySort * sort,
    const GDEFSTOR_QueryEnv * env, GDEFSTOR_QueryKey * out);

/** @brief Compare two keys of one sort, ascending: negative, 0 or positive. */
GDEFSTOR_API int gdefstor_query_key_compare(const GDEFSTOR_QuerySort * sort,
    const GDEFSTOR_QueryKey * a, const GDEFSTOR_QueryKey * b);

/** @brief Release a key's bytes. */
GDEFSTOR_API void gdefstor_query_key_free(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_QueryKey * key);

/**
 * @brief The first text of a select operand on a row.
 *
 * @param out_bytes A fresh copy; free with the allocator. NULL when absent.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_query_first_text(
    const GDEFSTOR_Allocator * alloc, const GDEFSTOR_QueryOperand * operand,
    const GDEFSTOR_QueryEnv * env, int * out_present, uint8_t ** out_bytes,
    size_t * out_len);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_QUERY_H */
