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
 * @file model_backend.h
 * @brief What a backend implements to serve the entity port (model.h).
 * @stability free
 *
 * For the authors of a backend. The port owns the meaning: it validates
 * declarations and trees, decides which items changed, assigns item ids and
 * order keys, checks the base revision, builds trees and differences, and
 * holds the reference query engine. A backend owns storage: it keeps the
 * truth rows exactly as handed to it and the current tables beside them, and
 * answers queries. The rows map one to one onto AD-14's tables, so a SQL
 * backend writes ordinary rows (`T`, `T_state`, `T_revision`, one table per
 * group, their current tables) and needs nothing an index cannot serve; none
 * of these calls needs a scan of more than one entity, except `rebuild` and
 * `dump`.
 *
 * Rules every backend keeps, which the conformance suite checks:
 *
 * - **Truth is append-only.** Entity rows, states, revisions and item versions
 *   are only inserted. The one update is an item version's `valid_to`, from
 *   open (0) to a revision number, once; a close of a version that is not open
 *   is ::GDEFSTOR_ERR_INTERNAL and changes nothing.
 * - **Current tables are derived.** They hold the open versions and the newest
 *   state, are written in the same transaction as the truth, and `rebuild`
 *   recreates them from the truth alone with the same result.
 * - **Isolation.** A call with a NULL `txn` sees the last commit, even while a
 *   transaction of the same tenant is open; a call with the open `txn` sees
 *   that transaction's own writes.
 * - Strings in visited rows are borrowed for the call; the port copies them.
 *   Rows are visited in no particular order.
 */

#ifndef GHOTI_IO_GDEFSTOR_MODEL_BACKEND_H
#define GHOTI_IO_GDEFSTOR_MODEL_BACKEND_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/core.h>
#include <ghoti.io/defiant-storage/model.h>
#include <ghoti.io/defiant-storage/query.h>
#include <ghoti.io/defiant-storage/value.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** An entity row (`T`), with its derived newest state. */
typedef struct GDEFSTOR_ModelEntity {
  uint64_t row;             ///< Assigned by the backend: increasing, never reused.
  const char * type;        ///< Immutable.
  size_t type_len;          ///< Its length.
  const char * public_id;   ///< Immutable, unique in the tenant.
  size_t public_id_len;     ///< Its length.
  const char * creator;     ///< Immutable; may be empty.
  size_t creator_len;       ///< Its length.
  int64_t created_ms;       ///< Immutable.
  uint64_t revision;        ///< The highest revision, 0 for none.
  uint64_t last_item;       ///< The highest item id ever given out.
  int published;            ///< Derived: the newest state row's, else 0.
  int deleted;              ///< Derived: the newest state row's, else 0.
} GDEFSTOR_ModelEntity;

/** A state row (`T_state`). */
typedef struct GDEFSTOR_ModelState {
  /** Its place in the entity's state history, 1, 2, 3 ...: assigned by the
   * backend on append (one more than the entity's last), ignored on the way in. */
  uint64_t seq;
  int published;          ///< From here on.
  int deleted;            ///< From here on.
  const char * actor;     ///< Who; may be empty.
  size_t actor_len;       ///< Its length.
  int64_t when_ms;        ///< When.
} GDEFSTOR_ModelState;

/** A revision row (`T_revision`). */
typedef struct GDEFSTOR_ModelRevision {
  uint64_t revision;      ///< Its number.
  /** The tenant's commit sequence: the same for every revision one transaction
   * writes, larger for each later transaction. Assigned by the backend in
   * `apply`; ignored on the way in. */
  uint64_t commit_seq;
  const char * who;       ///< Who; may be empty.
  size_t who_len;         ///< Its length.
  int64_t when_ms;        ///< When.
  const char * why;       ///< Why; may be empty.
  size_t why_len;         ///< Its length.
} GDEFSTOR_ModelRevision;

/** One version of one item: a row of a group table. */
typedef struct GDEFSTOR_ModelItem {
  uint32_t group;         ///< The group (table); the root group is 0.
  uint64_t item;          ///< Stable for the life of the item. The root item is 1.
  uint64_t parent;        ///< The parent item; 0 for the root item.
  double order;           ///< Order among the parent's items of this group.
  uint64_t valid_from;    ///< The revision this version first holds in.
  uint64_t valid_to;      ///< The revision that superseded it, or 0 (open).
  /** One per column of the group, by column number; a row written before a
   * column existed has fewer, and the missing ones are no value. */
  const GDEFSTOR_Value * values;
  size_t value_count;     ///< How many.
} GDEFSTOR_ModelItem;

/** Close one open item version. */
typedef struct GDEFSTOR_ModelClose {
  uint32_t group;         ///< Its group.
  uint64_t item;          ///< Its item.
  uint64_t valid_from;    ///< The version's valid_from; it must be open.
} GDEFSTOR_ModelClose;

/** One revision write, computed by the port. Applied all or nothing. */
typedef struct GDEFSTOR_ModelApply {
  uint64_t row;                       ///< The entity.
  const GDEFSTOR_ModelRevision * revision; ///< The new revision row.
  uint64_t last_item;                 ///< The entity's new highest item id.
  const GDEFSTOR_ModelClose * closes; ///< Set valid_to = the revision on each.
  size_t close_count;                 ///< How many.
  const GDEFSTOR_ModelItem * inserts; ///< New versions; valid_from is the revision, valid_to 0.
  size_t insert_count;                ///< How many.
} GDEFSTOR_ModelApply;

/** A draft row. */
typedef struct GDEFSTOR_ModelDraft {
  const char * public_id; ///< The entity, or the id it reserves.
  size_t public_id_len;   ///< Its length.
  const char * type;      ///< The type.
  size_t type_len;        ///< Its length.
  uint64_t base_revision; ///< Its base.
  const uint8_t * json;   ///< The tree.
  size_t json_len;        ///< Its length.
} GDEFSTOR_ModelDraft;

/**
 * Where a backend hands rows to the port. Each read names the members it
 * calls; a member that read does not use may be NULL. A member's error stops
 * the read, which returns it.
 */
typedef struct GDEFSTOR_ModelVisitor {
  void * ctx; ///< Passed to every member.
  GDEFSTOR_Result (*entity)(void * ctx, const GDEFSTOR_ModelEntity * e);
  GDEFSTOR_Result (*state)(void * ctx, uint64_t row, const GDEFSTOR_ModelState * s);
  GDEFSTOR_Result (*revision)(void * ctx, uint64_t row,
      const GDEFSTOR_ModelRevision * r);
  GDEFSTOR_Result (*item)(void * ctx, uint64_t row, const GDEFSTOR_ModelItem * i);
  GDEFSTOR_Result (*draft)(void * ctx, const GDEFSTOR_ModelDraft * d);
  GDEFSTOR_Result (*type)(void * ctx, const char * name, size_t name_len,
      const uint8_t * text, size_t text_len);
} GDEFSTOR_ModelVisitor;

/** What a backend's `query` is handed: a query the port has checked. */
typedef struct GDEFSTOR_ModelPlan {
  const GDEFSTOR_Allocator * alloc; ///< Allocate GDEFSTOR_ModelHits with this.
  const GDEFSTOR_Type * type;       ///< The queried type's declaration.
  const GDEFSTOR_Query * query;     ///< Checked: shape, limits, types, paths.
  /** The declaration of every type an EXISTS names, in the order of a
   * depth-first walk of the tree, one per EXISTS node. */
  const GDEFSTOR_Type * const * exists_types;
  size_t exists_count;              ///< How many.
  uint64_t limit;                   ///< The page after clamping; UINT64_MAX for every match.
  int count_only;                   ///< Only the total is wanted.
} GDEFSTOR_ModelPlan;

/** What a backend's `query` writes back. Arrays from GDEFSTOR_ModelPlan::alloc. */
typedef struct GDEFSTOR_ModelHits {
  uint64_t total;          ///< Matches before the offset and the limit.
  size_t count;            ///< Entities in the page, in order.
  uint64_t * rows;         ///< Their rows.
  char ** public_ids;      ///< Their public ids, each allocated.
  size_t * public_id_lens; ///< Their lengths.
} GDEFSTOR_ModelHits;

/** gdefstor_model_scan(): visit every current entity of @p type (entity, then its
 * current items), for the reference engine. */
typedef GDEFSTOR_Result (*GDEFSTOR_ModelScanFn)(void * ctx, const char * type,
    size_t type_len, const GDEFSTOR_ModelVisitor * visitor);

/**
 * The entity backend. A backend zeroes it, sets @ref size to
 * `sizeof(GDEFSTOR_ModelBackend)` and fills every member; all are required,
 * except those below @ref dump, which say they are optional.
 */
typedef struct GDEFSTOR_ModelBackend {
  size_t size; ///< `sizeof(GDEFSTOR_ModelBackend)`.
  void * ctx;  ///< Passed to every function.
  void (*destroy)(void * ctx);
  /** Open a transaction for one tenant. At most one per tenant at a time. */
  GDEFSTOR_Result (*begin)(void * ctx, const char * tenant, size_t tenant_len,
      void ** out_txn);
  GDEFSTOR_Result (*commit)(void * ctx, void * txn); ///< Publish it. Consumed either way.
  GDEFSTOR_Result (*abort)(void * ctx, void * txn);  ///< Drop it. Consumed.
  /** A type's declaration text, as stored; ::GDEFSTOR_ERR_INVALID when none.
   * The bytes come from the store's allocator. */
  GDEFSTOR_Result (*type_load)(void * ctx, void * txn, const char * tenant,
      size_t tenant_len, const char * type, size_t type_len, uint8_t ** out_text,
      size_t * out_len);
  /** Store a declaration: @p decl parsed, @p text as the port writes it. A SQL
   * backend runs its additive schema step for it here. */
  GDEFSTOR_Result (*type_store)(void * ctx, void * txn,
      const GDEFSTOR_Type * decl, const uint8_t * text, size_t text_len);
  /** Visit (`entity`) the row with this public id; ::GDEFSTOR_ERR_INVALID when none. */
  GDEFSTOR_Result (*entity_find)(void * ctx, void * txn, const char * tenant,
      size_t tenant_len, const char * public_id, size_t public_id_len,
      const GDEFSTOR_ModelVisitor * visitor);
  /** Insert an entity row (its row, revision and state members are ignored). */
  GDEFSTOR_Result (*entity_insert)(void * ctx, void * txn,
      const GDEFSTOR_ModelEntity * e, uint64_t * out_row);
  /** Append a state row; the entity's newest state follows it. */
  GDEFSTOR_Result (*state_append)(void * ctx, void * txn, uint64_t row,
      const GDEFSTOR_ModelState * s);
  /** Write one revision: its row, the closes, the inserts, the entity's
   * revision and last item, and the current tables. All or nothing. */
  GDEFSTOR_Result (*apply)(void * ctx, void * txn, const GDEFSTOR_ModelApply * a);
  /** Visit (`revision`, `state`) every revision and state row of an entity. */
  GDEFSTOR_Result (*history)(void * ctx, void * txn, const char * tenant,
      size_t tenant_len, uint64_t row, const GDEFSTOR_ModelVisitor * visitor);
  /** Visit (`item`) an entity's items: with @p as_of 0, its current tables;
   * otherwise every version with valid_from <= as_of and valid_to either 0 or
   * > as_of, from the truth. */
  GDEFSTOR_Result (*items)(void * ctx, void * txn, const char * tenant,
      size_t tenant_len, uint64_t row, uint64_t as_of,
      const GDEFSTOR_ModelVisitor * visitor);
  /** Insert or replace the draft of a public id. */
  GDEFSTOR_Result (*draft_put)(void * ctx, void * txn,
      const GDEFSTOR_ModelDraft * d);
  /** Visit (`draft`) the draft of a public id; ::GDEFSTOR_ERR_INVALID when none. */
  GDEFSTOR_Result (*draft_get)(void * ctx, void * txn, const char * tenant,
      size_t tenant_len, const char * public_id, size_t public_id_len,
      const GDEFSTOR_ModelVisitor * visitor);
  /** Delete a draft; ::GDEFSTOR_ERR_INVALID when none. */
  GDEFSTOR_Result (*draft_delete)(void * ctx, void * txn,
      const char * public_id, size_t public_id_len);
  /** Answer a checked query over the last commit, exactly as
   * gdefstor_model_scan() would. */
  GDEFSTOR_Result (*query)(void * ctx, const char * tenant, size_t tenant_len,
      const GDEFSTOR_ModelPlan * plan, GDEFSTOR_ModelHits * out);
  /** Drop every current table and derived lookup of the tenant and rebuild
   * them from the truth. */
  GDEFSTOR_Result (*rebuild)(void * ctx, void * txn);
  /** Visit, as of the last commit: GDEFSTOR_MODEL_TRUTH, every `type`,
   * `entity`, `state`, `revision`, `item` version and `draft`;
   * GDEFSTOR_MODEL_CURRENT, every `entity` and current `item`. */
  GDEFSTOR_Result (*dump)(void * ctx, const char * tenant, size_t tenant_len,
      int which, const GDEFSTOR_ModelVisitor * visitor);
  /* ---- added after the first version: optional ---------------------------
   * A backend built against the struct that ends at @ref dump sets @ref size
   * to that size (GDEFSTOR_MODEL_BACKEND_SIZE_V1), and the port treats every
   * member below as NULL. */
  /** The context @ref index_state is called with. A member of its own, not
   * @ref ctx, so that a wrapper that copies this struct and replaces @ref ctx
   * to intercept the other members still reaches the backend that owns the
   * index. */
  void * index_ctx;
  /**
   * The state of the index of field @p field (an index for
   * gdefstor_type_field()) of @p type, as of the last commit: a
   * ::GDEFSTOR_IndexState other than NONE (the port answers NONE itself for a
   * field that is not indexed). May be NULL: every indexed field then reads
   * ::GDEFSTOR_INDEX_READY. A backend that builds an index after the
   * declaring write commits (AD-15) answers ::GDEFSTOR_INDEX_INDEXING until
   * the build is done.
   */
  GDEFSTOR_Result (*index_state)(void * index_ctx, const char * tenant,
      size_t tenant_len, const GDEFSTOR_Type * type, size_t field,
      int * out_state);
} GDEFSTOR_ModelBackend;

/** `sizeof(GDEFSTOR_ModelBackend)` before @ref GDEFSTOR_ModelBackend::index_ctx
 * was added; gdefstor_store_create_model() accepts it. */
#define GDEFSTOR_MODEL_BACKEND_SIZE_V1 \
  offsetof(GDEFSTOR_ModelBackend, index_ctx)

/**
 * @brief The reference query engine: answer @p plan over the entities
 * @p scan visits.
 *
 * This is the meaning every backend's `query` must equal, and what a backend
 * with no engine of its own (the memory backend) calls from its `query`. For
 * a plan with EXISTS it scans each named type once.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_model_scan(const GDEFSTOR_ModelPlan * plan,
    GDEFSTOR_ModelScanFn scan, void * scan_ctx, GDEFSTOR_ModelHits * out);

/** @brief Release what a `query` wrote, with GDEFSTOR_ModelPlan::alloc. */
GDEFSTOR_API void gdefstor_model_hits_release(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_ModelHits * hits);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_MODEL_BACKEND_H */
