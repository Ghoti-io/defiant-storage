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
 * @brief The query form over the current entities of one declared type.
 * @stability free
 *
 * A query is **data, not code**: a bounded tree of predicates, a sort, an
 * offset and a limit. The caller builds the tree in its own memory and the
 * port reads it during one call. Nothing in it is ever spliced into SQL: a
 * backend that speaks SQL builds its statement from a fixed vocabulary, and
 * every constant, and the page, reach it as a bound value.
 * gdefstor_entity_query() and gdefstor_entity_count() ask it of the entities
 * of one declared type (model.h), over their current trees.
 *
 * ## Operands
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
 * - ::GDEFSTOR_QUERY_CONST: texts; only the MAC of blind-index equality.
 *
 * The two sides of a comparison have the same type (a reference compares with
 * a reference or the public id; a decimal with a decimal of the same scale).
 * `=`, `!=` and `in` take any type; `<`, `<=`, `>`, `>=` take integer, decimal,
 * float, date and date-time, and compare numbers, not text. `=` holds when
 * some value on the left equals some on the right; `!=` holds exactly when
 * `=` does not; `in` is `=` against a list. `and`, `or` and `not` are the
 * usual truth tables, and an `and` or `or` with no children is true and false.
 *
 * ## Languages
 *
 * A query binds **one language** (GDEFSTOR_Query::language, required): every
 * FIELD operand and every FIELD sort key in it, inside an EXISTS too, reads
 * the current items of that language's tree and of no other, so two
 * conditions on one entity are always about the same language. An entity
 * with no tree in that language is still an entity of the type: it reads as
 * one whose every field is absent, so no comparison of a field matches it
 * (a NOT around one does, as for an absent value) and it sorts as no value.
 * META and ENTITY operands are the entity's and read the same in every
 * language.
 *
 * ## A semi-join
 *
 * ::GDEFSTOR_QUERY_EXISTS names another declared type; its one child is a
 * predicate over that type's entities, and an operand flagged
 * ::GDEFSTOR_QUERY_OUTER reads the entity EXISTS is decided for, so the child
 * can say "the other entity's field equals this one's". An EXISTS inside an
 * EXISTS is refused, and so is an OUTER operand outside one. A relation read
 * as a condition is one of these. An EXISTS may name the language its other
 * type is read in (GDEFSTOR_QueryNode::exists_language): its FIELD operands
 * then read that language's items, and its OUTER operands the query's. With
 * none it is the query's language, as everywhere else.
 *
 * ## Order and pages
 *
 * A sort key is a single scalar field of the root group, a META fact or the
 * public id; no value sorts before every value; text sorts by bytes. The
 * final tiebreak is the internal row, ascending even under a descending sort,
 * which is creation order, so a page is always the same slice of one total
 * order. A limit above ::GDEFSTOR_QUERY_LIMIT_MAX is clamped to it; a limit
 * of 0 asks for every match. The total counts every match before the offset
 * and the limit.
 *
 * ## Flags
 *
 * Three flags read an operand or a sort key the way a caller whose own values
 * are texts reads them (core's conditions are), and are refused anywhere else:
 *
 * - ::GDEFSTOR_QUERY_EMPTY_IF_NONE on a FIELD operand of a TEXT or REFERENCE
 *   field: when no current item of the query's language has a value, the
 *   operand is the single empty text instead of the empty set, so `= ""`
 *   holds on the entity and `!= ""` does not. On a sort key of such a field,
 *   no value sorts as the empty text, tied with it.
 * - ::GDEFSTOR_QUERY_AS_TEXT on an operand of type REFERENCE (a reference
 *   field, META CREATOR or REVISION_WHO, ENTITY): it is of type TEXT, so it
 *   compares by bytes with a text field or text constants.
 * - ::GDEFSTOR_QUERY_FIRST on a sort key: the key is a MULTIPLE scalar field
 *   of the root group, and an entity sorts by the value of its first item
 *   (the lowest order key, then the lowest item id) that has one.
 *
 * ## Blind-index equality
 *
 * A personal field holds ciphertext, so the comparisons above refuse it; it is
 * matched only by ::GDEFSTOR_QUERY_BLIND, blind-index equality: the left
 * operand a FIELD naming an active personal scalar field (no OUTER), the
 * right a ::GDEFSTOR_QUERY_CONST of exactly one text of
 * ::GDEFSTOR_BLIND_MAC_BYTES bytes. It holds when some current item of the
 * field's group, in the query's language, has a value whose current version
 * has a blind row with that MAC (gdefstor_tree_set_blind()), so "some item
 * matches" for multiples and groups, and an older version's MAC does not
 * match. It composes with every other node, inside an EXISTS too. A personal
 * field is never a sort key and never compared by range. On a store whose
 * backend has no blind table the node is ::GDEFSTOR_ERR_UNSUPPORTED.
 *
 * ## Limits and refusals
 *
 * A tree deeper than ::GDEFSTOR_QUERY_MAX_DEPTH, with more than
 * ::GDEFSTOR_QUERY_MAX_NODES nodes, or a constant larger than the caps below
 * is refused with ::GDEFSTOR_ERR_LIMIT. A shape that makes no sense, a flag
 * where it does not apply, and a type that was never declared are
 * ::GDEFSTOR_ERR_INVALID.
 *
 * The query sees the last commit. It checks no permission: the caller of the
 * port is the trusted layer, and a caller that serves a visitor puts the
 * visitor's access filter into the tree.
 */

#ifndef GHOTI_IO_GDEFSTOR_QUERY_H
#define GHOTI_IO_GDEFSTOR_QUERY_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/store.h>
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
/** The largest page. A larger limit is clamped to this. */
#define GDEFSTOR_QUERY_LIMIT_MAX 1000

/** An operand's flag: a text or reference field with no value is the single
 * empty text ("Flags"). */
#define GDEFSTOR_QUERY_EMPTY_IF_NONE 1
/** An operand's flag, inside an EXISTS: the operand is of the entity the
 * EXISTS is being decided for (the outer one), not of the one it looks at. */
#define GDEFSTOR_QUERY_OUTER 2
/** An operand's flag: a REFERENCE operand is compared as TEXT ("Flags"). */
#define GDEFSTOR_QUERY_AS_TEXT 4
/** A sort key's flag: a root MULTIPLE scalar field, sorted by its first
 * item's value ("Flags"). */
#define GDEFSTOR_QUERY_FIRST 8

/** One text. Borrowed for the call that takes it. */
typedef struct GDEFSTOR_QueryText {
  const uint8_t * bytes; ///< May be NULL when @ref len is 0.
  size_t len;            ///< Length of @ref bytes.
} GDEFSTOR_QueryText;

/** What an operand is. */
typedef enum GDEFSTOR_QueryKind {
  GDEFSTOR_QUERY_CONST = 1, ///< The texts listed: a blind MAC.
  GDEFSTOR_QUERY_ENTITY,    ///< The entity's public id.
  GDEFSTOR_QUERY_FIELD,     ///< The values of a field path.
  GDEFSTOR_QUERY_VALUES,    ///< Typed constants.
  GDEFSTOR_QUERY_META       ///< A fact of the entity record.
} GDEFSTOR_QueryKind;

/** The facts of an entity record a ::GDEFSTOR_QUERY_META operand reads. */
typedef enum GDEFSTOR_QueryMeta {
  GDEFSTOR_META_PUBLISHED = 1, ///< Boolean: the newest state.
  GDEFSTOR_META_DELETED,       ///< Boolean: the newest state (soft delete).
  GDEFSTOR_META_CREATOR,       ///< Reference: who created it; none for the system.
  GDEFSTOR_META_CREATED,       ///< Date-time: when.
  GDEFSTOR_META_REVISION,      ///< Integer: the newest revision, 0 for none.
  /** Reference: who wrote the newest revision; none for an entity with no
   * revision and for one the system wrote (an empty who). */
  GDEFSTOR_META_REVISION_WHO
} GDEFSTOR_QueryMeta;

/** One side of a comparison, or a sort key. */
typedef struct GDEFSTOR_QueryOperand {
  int kind;                   ///< A ::GDEFSTOR_QueryKind.
  int flags;                  ///< ::GDEFSTOR_QUERY_EMPTY_IF_NONE and the others, or 0.
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
  GDEFSTOR_QUERY_EQ,       ///< Some left value equals some right value.
  GDEFSTOR_QUERY_NE,       ///< No left value equals any right value.
  GDEFSTOR_QUERY_IN,       ///< EQ against a list (right is VALUES).
  GDEFSTOR_QUERY_LT,       ///< Some pair, left < right.
  GDEFSTOR_QUERY_LE,       ///< Some pair, left <= right.
  GDEFSTOR_QUERY_GT,       ///< Some pair, left > right.
  GDEFSTOR_QUERY_GE,       ///< Some pair, left >= right.
  GDEFSTOR_QUERY_EXISTS,   ///< Some entity of another type satisfies the child.
  /** Blind-index equality. Left is a FIELD naming a personal field, right a
   * CONST of one text, the ::GDEFSTOR_BLIND_MAC_BYTES bytes of a MAC: holds
   * when some current value version of the field has a blind row with that
   * MAC ("Blind-index equality"). */
  GDEFSTOR_QUERY_BLIND
} GDEFSTOR_QueryOp;

/** A node of the predicate tree. */
typedef struct GDEFSTOR_QueryNode {
  int op;                                   ///< A ::GDEFSTOR_QueryOp.
  GDEFSTOR_QueryOperand left;               ///< Comparisons only.
  GDEFSTOR_QueryOperand right;              ///< Comparisons only.
  const struct GDEFSTOR_QueryNode * const * children; ///< AND, OR, NOT, EXISTS.
  size_t child_count;                       ///< How many children.
  const char * exists_type;                 ///< EXISTS: the other declared type.
  size_t exists_type_len;                   ///< Length of @ref exists_type.
  /** EXISTS: the language the other type's FIELD operands read (a language
   * tag), or NULL for the query's. OUTER operands always read the query's. */
  const char * exists_language;
  size_t exists_language_len;               ///< Its length.
} GDEFSTOR_QueryNode;

/** One sort key. */
typedef struct GDEFSTOR_QuerySort {
  GDEFSTOR_QueryOperand key; ///< FIELD, META or ENTITY ("Order and pages").
  int descending;            ///< Largest first.
} GDEFSTOR_QuerySort;

/** A query. Begins with @ref size. */
typedef struct GDEFSTOR_Query {
  size_t size;                            ///< `sizeof(GDEFSTOR_Query)`.
  const GDEFSTOR_QueryNode * where;       ///< NULL is every row.
  const GDEFSTOR_QuerySort * sorts;       ///< Sort keys, or NULL.
  size_t sort_count;                      ///< How many.
  uint64_t offset;                        ///< Matches skipped.
  uint64_t limit;                         ///< Rows returned, 0 for all.
  /** The one language every field operand and sort key reads (a language
   * tag, model.h). Required. */
  const char * language;
  size_t language_len;                    ///< Its length.
} GDEFSTOR_Query;

/** The rows of a query. Opaque. */
typedef struct GDEFSTOR_QueryResult GDEFSTOR_QueryResult;

/** @return How many rows this page holds. */
GDEFSTOR_API size_t gdefstor_query_result_rows(const GDEFSTOR_QueryResult * r);

/** @return The matches before the offset and the limit. */
GDEFSTOR_API uint64_t gdefstor_query_result_total(
    const GDEFSTOR_QueryResult * r);

/**
 * @brief The public id of a row's entity. Valid until the result is freed.
 *
 * @return ::GDEFSTOR_OK or ::GDEFSTOR_ERR_INVALID for an index out of range.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_query_result_entity(
    const GDEFSTOR_QueryResult * r, size_t row, const char ** out_entity,
    size_t * out_len);

/** @brief Release a result. NULL is ignored. */
GDEFSTOR_API void gdefstor_query_result_free(GDEFSTOR_QueryResult * r);

/**
 * @brief Run a query over the current entities of one declared type.
 *
 * See the file comment. gdefstor_query_result_entity() gives each entity's
 * public id.
 *
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, ::GDEFSTOR_ERR_LIMIT,
 *   ::GDEFSTOR_ERR_UNSUPPORTED (a blind node on a backend without the blind
 *   table), ::GDEFSTOR_ERR_OOM, or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_query(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const GDEFSTOR_Query * query, GDEFSTOR_QueryResult ** out_result);

/**
 * @brief Count the current entities of one declared type that match, with
 * the predicate's fields read in @p language.
 *
 * The same predicate and refusals as gdefstor_entity_query().
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_count(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const char * language, size_t language_len,
    const GDEFSTOR_QueryNode * where, uint64_t * out_count);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_QUERY_H */
