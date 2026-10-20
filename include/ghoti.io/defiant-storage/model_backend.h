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
 * - **The blind table is apart** (a backend that serves it, below). A MAC is
 *   written only into it, only in the `apply` that writes the version it
 *   indexes, and is read back only through it; `rebuild` leaves it alone.
 * - **Records, counters and outbox rows are truth** (a backend that serves
 *   them, below). They are written only in a transaction, `rebuild` leaves
 *   them alone, and they are visited by `dump` (GDEFSTOR_MODEL_TRUTH).
 * - **A counter's advance outlives its transaction's abort.** A value
 *   `counter_next` returned for a ::GDEFSTOR_MODEL_COUNTER is never returned
 *   again for that tenant and name, even when the transaction aborts (`abort`,
 *   or a `commit` that fails): the backend keeps a floor apart from the
 *   committed value, which is what `counter_get` and `dump` read.
 * - **Languages are a column, never a table.** Every item version, close and
 *   blind row carries the language of the tree it is in (model.h), and the
 *   backend keeps it beside the row. Item ids are unique within an entity
 *   across languages, so (group, item, valid_from) still names one version.
 *   A backend reads one language's items without reading the others' where
 *   it is asked for one (`items`), and has no table, column or index per
 *   language.
 * - Strings in visited rows are borrowed for the call; the port copies them.
 *   Rows are visited in no particular order, except where a member says.
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
  /** Added with languages: the languages whose items this revision wrote
   * (inserted or closed), sorted by bytes and joined by `,`; from the port,
   * stored as given. */
  const char * languages;
  size_t languages_len;   ///< Its length.
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
  const char * language;  ///< The language of the tree it is in (model.h).
  size_t language_len;    ///< Its length.
} GDEFSTOR_ModelItem;

/** Close one open item version. */
typedef struct GDEFSTOR_ModelClose {
  uint32_t group;         ///< Its group.
  uint64_t item;          ///< Its item.
  uint64_t valid_from;    ///< The version's valid_from; it must be open.
  const char * language;  ///< The version's language, as it was written.
  size_t language_len;    ///< Its length.
} GDEFSTOR_ModelClose;

/**
 * A blind-index row: the MAC of one personal value version (AD-14). The
 * blind table holds these, one per value version that was written with a
 * MAC, and nothing else does: never a group, revision or current row. They
 * are truth while their subject exists (a rebuild leaves them alone) and are
 * deleted per subject at erasure.
 */
typedef struct GDEFSTOR_ModelBlind {
  uint64_t item;          ///< The item version it indexes: the item ...
  uint64_t valid_from;    ///< ... and that version's valid_from.
  uint32_t field;         ///< The personal field: its GDEFSTOR_FieldInfo::id.
  const uint8_t * subject;///< The value's subject (its subject column).
  size_t subject_len;     ///< Its length.
  const uint8_t * mac;    ///< ::GDEFSTOR_BLIND_MAC_BYTES bytes.
  uint32_t key_id;        ///< GDEFSTOR_BlindMac::key_id.
  uint32_t version;       ///< GDEFSTOR_BlindMac::version.
  const char * language;  ///< The language of the version it indexes.
  size_t language_len;    ///< Its length.
} GDEFSTOR_ModelBlind;

/** One revision write, computed by the port. Applied all or nothing. */
typedef struct GDEFSTOR_ModelApply {
  uint64_t row;                       ///< The entity.
  const GDEFSTOR_ModelRevision * revision; ///< The new revision row.
  uint64_t last_item;                 ///< The entity's new highest item id.
  const GDEFSTOR_ModelClose * closes; ///< Set valid_to = the revision on each.
  size_t close_count;                 ///< How many.
  const GDEFSTOR_ModelItem * inserts; ///< New versions; valid_from is the revision, valid_to 0.
  size_t insert_count;                ///< How many.
  /** Added with the blind index: rows for the blind table, each for a
   * version among @ref inserts (its valid_from is the revision). Only a
   * backend that serves the blind index is handed any. */
  const GDEFSTOR_ModelBlind * blinds;
  size_t blind_count;                 ///< How many.
} GDEFSTOR_ModelApply;

/** A record row: non-revisioned bytes under a namespace and a key (model.h). */
typedef struct GDEFSTOR_ModelRecord {
  const char * ns;        ///< The namespace.
  size_t ns_len;          ///< Its length.
  const char * key;       ///< The key.
  size_t key_len;         ///< Its length.
  const uint8_t * bytes;  ///< The value; may be NULL when @ref len is 0.
  size_t len;             ///< Its length.
} GDEFSTOR_ModelRecord;

/** GDEFSTOR_ModelBackend::counter_next: a caller's counter (model.h), whose
 * advance outlives an abort. */
#define GDEFSTOR_MODEL_COUNTER 1
/** GDEFSTOR_ModelBackend::counter_next: an outbox stream's last sequence,
 * named by the stream, whose advance is undone with its transaction. */
#define GDEFSTOR_MODEL_STREAM 2

/** An outbox row (AD-33). */
typedef struct GDEFSTOR_ModelOutbox {
  const char * stream;      ///< Its stream.
  size_t stream_len;        ///< Its length.
  uint64_t seq;             ///< Its sequence in the stream, from the port.
  const char * public_id;   ///< Its public id, unique in the tenant.
  size_t public_id_len;     ///< Its length.
  const char * format;      ///< Its payload's format name.
  size_t format_len;        ///< Its length.
  const uint8_t * payload;  ///< Opaque; may be NULL when @ref payload_len is 0.
  size_t payload_len;       ///< Its length.
  int64_t lease_until_ms;   ///< 0 until claimed; then where the lease ends.
  uint32_t failures;        ///< Failed deliveries counted.
  int parked;               ///< Non-zero once parked: never claimed, never deleted.
} GDEFSTOR_ModelOutbox;

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
  /** Added with the blind index: a blind row of entity @p row. A backend
   * calls it only when it is set, and then from `items` (every blind row of
   * the entity, which the port matches to the versions it reads), `dump`
   * (GDEFSTOR_MODEL_TRUTH: every blind row) and the scan the reference engine
   * is given (every blind row of each entity it visits, after the entity). */
  GDEFSTOR_Result (*blind)(void * ctx, uint64_t row,
      const GDEFSTOR_ModelBlind * b);
  /** Added with records: a record, from `record_get`, `record_list` and
   * `dump` (GDEFSTOR_MODEL_TRUTH, every record). Called only when set. */
  GDEFSTOR_Result (*record)(void * ctx, const GDEFSTOR_ModelRecord * r);
  /** Added with counters: a counter of @p kind and its committed value, from
   * `dump` (GDEFSTOR_MODEL_TRUTH, every counter of both kinds; the port
   * leaves out a value of 0). Called only when set. */
  GDEFSTOR_Result (*counter)(void * ctx, int kind, const char * name,
      size_t name_len, uint64_t value);
  /** Added with the outbox: an outbox row, from `outbox_claim` and `dump`
   * (GDEFSTOR_MODEL_TRUTH, every row, parked or not). Called only when set. */
  GDEFSTOR_Result (*outbox)(void * ctx, const GDEFSTOR_ModelOutbox * o);
  /** Added with languages: a language entity @p row has a tree in, from
   * `languages`. */
  GDEFSTOR_Result (*language)(void * ctx, uint64_t row, const char * language,
      size_t language_len);
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
 * current items of every language, and, when the visitor's `blind` is set,
 * its blind rows), for the reference engine, which keeps those of the
 * query's language. */
typedef GDEFSTOR_Result (*GDEFSTOR_ModelScanFn)(void * ctx, const char * type,
    size_t type_len, const GDEFSTOR_ModelVisitor * visitor);

/** One blind row a backend's `blind_history` found, with what the port reports
 * of it. Strings are borrowed for the call. */
typedef struct GDEFSTOR_ModelBlindHit {
  uint64_t row;             ///< The entity.
  const char * public_id;   ///< Its public id.
  size_t public_id_len;     ///< Its length.
  uint64_t item;            ///< The version's item ...
  uint64_t valid_from;      ///< ... and valid_from.
  uint64_t valid_to;        ///< That version's valid_to, or 0 while it is open.
  const uint8_t * subject;  ///< The row's subject.
  size_t subject_len;       ///< Its length.
  uint32_t key_id;          ///< As stored.
  uint32_t version;         ///< As stored.
  const char * language;    ///< The version's language.
  size_t language_len;      ///< Its length.
} GDEFSTOR_ModelBlindHit;

/** Where `blind_history` hands its rows, in no particular order. */
typedef GDEFSTOR_Result (*GDEFSTOR_ModelBlindFn)(void * ctx,
    const GDEFSTOR_ModelBlindHit * hit);

/** One personal value version a backend's `subject_lookup` found. Strings are
 * borrowed for the call. */
typedef struct GDEFSTOR_ModelSubjectHit {
  const char * type;        ///< The entity's type.
  size_t type_len;          ///< Its length.
  uint64_t row;             ///< The entity.
  const char * public_id;   ///< Its public id.
  size_t public_id_len;     ///< Its length.
  uint32_t group;           ///< The version's group ...
  uint32_t column;          ///< ... and the personal column naming the subject.
  uint64_t item;            ///< The item.
  uint64_t valid_from;      ///< The version's valid_from.
  uint64_t valid_to;        ///< Its valid_to, or 0 while it is open.
  const char * language;    ///< The version's language.
  size_t language_len;      ///< Its length.
} GDEFSTOR_ModelSubjectHit;

/** Where `subject_lookup` hands its rows, in no particular order. */
typedef GDEFSTOR_Result (*GDEFSTOR_ModelSubjectFn)(void * ctx,
    const GDEFSTOR_ModelSubjectHit * hit);

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
  /** Visit (`item`) an entity's items of @p language (of every language when
   * @p language_len is 0): with @p as_of 0, its current tables; otherwise
   * every version with valid_from <= as_of and valid_to either 0 or > as_of,
   * from the truth. When the visitor's `blind` is set (only on a backend that
   * serves the blind index), also every blind row of the entity in that
   * language (every language when @p language_len is 0). */
  GDEFSTOR_Result (*items)(void * ctx, void * txn, const char * tenant,
      size_t tenant_len, uint64_t row, uint64_t as_of, const char * language,
      size_t language_len, const GDEFSTOR_ModelVisitor * visitor);
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
   * to that size (GDEFSTOR_MODEL_BACKEND_SIZE_V1), and one built against the
   * struct that ends at @ref index_state to GDEFSTOR_MODEL_BACKEND_SIZE_V2,
   * at @ref blind_delete to GDEFSTOR_MODEL_BACKEND_SIZE_V3, and at
   * @ref outbox_parked to GDEFSTOR_MODEL_BACKEND_SIZE_V4, and at
   * @ref languages to GDEFSTOR_MODEL_BACKEND_SIZE_V5; the port treats
   * every member past its size as NULL. (The rows above gained a language
   * with languages, so a backend is built against this header whatever its
   * size.) */
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
  /* ---- the blind index (AD-14): optional, all three or none ---------------
   * A backend that sets them keeps the blind table: it writes
   * GDEFSTOR_ModelApply::blinds in `apply`, visits blind rows through
   * GDEFSTOR_ModelVisitor::blind, answers ::GDEFSTOR_QUERY_BLIND in `query`
   * and leaves the table alone in `rebuild`. One that does not is never
   * handed a MAC: the port refuses those calls ::GDEFSTOR_ERR_UNSUPPORTED. */
  /** The context the three below are called with, kept apart from @ref ctx
   * for the reason @ref index_ctx is. */
  void * blind_ctx;
  /** Visit (@p fn) every blind row of field @p field (an index for
   * gdefstor_type_field()) of @p type whose MAC is @p mac
   * (::GDEFSTOR_BLIND_MAC_BYTES bytes), as of the last commit. Served by an
   * index on the MAC, never by reading ciphertext or subjects. */
  GDEFSTOR_Result (*blind_history)(void * blind_ctx, const char * tenant,
      size_t tenant_len, const GDEFSTOR_Type * type, size_t field,
      const uint8_t * mac, GDEFSTOR_ModelBlindFn fn, void * fn_ctx);
  /** Visit (@p fn) every item version, of every type and every personal
   * column, retired or not, whose subject column is @p subject, as of the
   * last commit. Served by the subject columns' indexes, never by the blind
   * table. */
  GDEFSTOR_Result (*subject_lookup)(void * blind_ctx, const char * tenant,
      size_t tenant_len, const uint8_t * subject, size_t subject_len,
      GDEFSTOR_ModelSubjectFn fn, void * fn_ctx);
  /** Delete every blind row of @p subject, of every type, in @p txn.
   * Deleting none is success. */
  GDEFSTOR_Result (*blind_delete)(void * blind_ctx, void * txn,
      const uint8_t * subject, size_t subject_len);
  /* ---- records, counters and the outbox (model.h; AD-31, AD-33): optional,
   * all of them or none -----------------------------------------------------
   * A backend that sets them keeps three more kinds of truth beside the
   * entities, written in the same transactions. One that does not is never
   * asked: the port answers those calls ::GDEFSTOR_ERR_UNSUPPORTED. The port
   * validates every name and value before it calls; a backend only stores. */
  /** The context the members below are called with, kept apart from
   * @ref ctx for the reason @ref index_ctx is. */
  void * sys_ctx;
  /** Insert record (@p r->ns, @p r->key), or replace its value, in @p txn. */
  GDEFSTOR_Result (*record_put)(void * sys_ctx, void * txn,
      const GDEFSTOR_ModelRecord * r);
  /** Delete record (@p ns, @p key) in @p txn: `*out_deleted` 1 when there was
   * one, 0 when not (which is success). */
  GDEFSTOR_Result (*record_delete)(void * sys_ctx, void * txn, const char * ns,
      size_t ns_len, const char * key, size_t key_len, int * out_deleted);
  /** Visit (`record`) record (@p ns, @p key) as @p txn sees it (NULL: the
   * last commit); visit nothing when there is none. */
  GDEFSTOR_Result (*record_get)(void * sys_ctx, void * txn,
      const char * tenant, size_t tenant_len, const char * ns, size_t ns_len,
      const char * key, size_t key_len, const GDEFSTOR_ModelVisitor * visitor);
  /** Visit (`record`), as of the last commit and in ascending byte order of
   * key, the records of @p ns whose key begins with @p prefix (every key when
   * @p prefix_len is 0), skipping the first @p offset and visiting at most
   * @p limit. */
  GDEFSTOR_Result (*record_list)(void * sys_ctx, const char * tenant,
      size_t tenant_len, const char * ns, size_t ns_len, const char * prefix,
      size_t prefix_len, uint64_t offset, size_t limit,
      const GDEFSTOR_ModelVisitor * visitor);
  /** Advance counter (@p kind, @p name) in @p txn and return its new value:
   * one more than the larger of its value and its floor (both 0 for a counter
   * never advanced), which becomes its value. ::GDEFSTOR_ERR_LIMIT when that
   * would pass INT64_MAX. For ::GDEFSTOR_MODEL_COUNTER the value is also kept
   * as the floor whatever becomes of @p txn (the rules above); for
   * ::GDEFSTOR_MODEL_STREAM it is undone with the rest of an aborted
   * transaction. */
  GDEFSTOR_Result (*counter_next)(void * sys_ctx, void * txn, int kind,
      const char * name, size_t name_len, uint64_t * out_value);
  /** A counter's committed value as of the last commit, 0 when none. */
  GDEFSTOR_Result (*counter_get)(void * sys_ctx, const char * tenant,
      size_t tenant_len, int kind, const char * name, size_t name_len,
      uint64_t * out_value);
  /** Insert an outbox row in @p txn, as the port made it (sequence, no
   * lease, no failure, not parked). ::GDEFSTOR_ERR_INVALID when its public id
   * is a row's already. */
  GDEFSTOR_Result (*outbox_insert)(void * sys_ctx, void * txn,
      const GDEFSTOR_ModelOutbox * o);
  /** In @p txn: when a row of @p stream that is not parked has
   * `lease_until_ms > now_ms`, do nothing; otherwise take the stream's rows
   * that are not parked in ascending sequence, at most @p max of them, set
   * each one's `lease_until_ms` to @p until_ms, and visit (`outbox`) each, as
   * it is now, in that order. */
  GDEFSTOR_Result (*outbox_claim)(void * sys_ctx, void * txn,
      const char * stream, size_t stream_len, int64_t now_ms, int64_t until_ms,
      size_t max, const GDEFSTOR_ModelVisitor * visitor);
  /** Delete the row with this public id in @p txn, unless it is parked:
   * `*out_deleted` 1 when it was deleted, 0 when there is none or it is
   * parked (both success). */
  GDEFSTOR_Result (*outbox_delete)(void * sys_ctx, void * txn,
      const char * public_id, size_t public_id_len, int * out_deleted);
  /** In @p txn, for the row with this public id: when it is parked, change
   * nothing; otherwise count one more failure and, when the count reaches
   * @p park_at, park it (its lease cleared to 0), else end its lease at
   * @p now_ms. `*out_found` 0 when there is no such row; `*out_parked` its
   * parked flag afterwards. */
  GDEFSTOR_Result (*outbox_fail)(void * sys_ctx, void * txn,
      const char * public_id, size_t public_id_len, int64_t now_ms,
      uint32_t park_at, int * out_found, int * out_parked);
  /** How many rows of @p stream (every stream when @p stream_len is 0) are
   * parked, as of the last commit. */
  GDEFSTOR_Result (*outbox_parked)(void * sys_ctx, const char * tenant,
      size_t tenant_len, const char * stream, size_t stream_len,
      uint64_t * out_count);
  /* ---- languages (model.h): optional ------------------------------------- */
  /** The context @ref languages is called with, kept apart from @ref ctx for
   * the reason @ref index_ctx is. */
  void * lang_ctx;
  /** Visit (`language`) each language entity @p row has a tree in, once:
   * with @p as_of 0 by its current root items, otherwise by the root-group
   * versions valid at @p as_of (a language's tree is the one with a root
   * item). May be NULL: the port then reads the root items through `items`,
   * which reads every language's whole tree. */
  GDEFSTOR_Result (*languages)(void * lang_ctx, void * txn, const char * tenant,
      size_t tenant_len, uint64_t row, uint64_t as_of,
      const GDEFSTOR_ModelVisitor * visitor);
  /* ---- restoring a snapshot (snapshot.h; AD-23): optional, all three or
   * none -------------------------------------------------------------------
   * An import writes the truth a snapshot holds back as it was: the
   * revisions with their commit sequences, the counters at their values and
   * the outbox rows with their leases, failures and parked flags. A write
   * through the members above cannot say any of those, so a backend that
   * sets these can be imported into; one that does not refuses an import
   * that holds entity truth with ::GDEFSTOR_ERR_UNSUPPORTED. The port calls
   * them only in an import's own transaction, into a tenant that holds no
   * truth, with values it has validated. */
  /** The context the three below are called with, kept apart from @ref ctx
   * for the reason @ref index_ctx is. */
  void * restore_ctx;
  /** As `apply`, except that the revision row keeps
   * GDEFSTOR_ModelRevision::commit_seq as given instead of taking the
   * transaction's: the tenant's commit sequence is raised to at least it,
   * so the next transaction that writes a revision takes a larger one.
   * Every revision one import restores may carry a different sequence. */
  GDEFSTOR_Result (*restore_revision)(void * restore_ctx, void * txn,
      const GDEFSTOR_ModelApply * a);
  /** Set counter (@p kind, @p name) to @p value in @p txn, as a committed
   * value would be: `counter_get` and `dump` read it once @p txn commits,
   * and the next `counter_next` returns one more. Only a backend that keeps
   * records, counters and the outbox is called. */
  GDEFSTOR_Result (*restore_counter)(void * restore_ctx, void * txn, int kind,
      const char * name, size_t name_len, uint64_t value);
  /** As `outbox_insert`, except that the row keeps the
   * GDEFSTOR_ModelOutbox::lease_until_ms, `failures` and `parked` it is
   * given. Only a backend that keeps records, counters and the outbox is
   * called. */
  GDEFSTOR_Result (*restore_outbox)(void * restore_ctx, void * txn,
      const GDEFSTOR_ModelOutbox * o);
} GDEFSTOR_ModelBackend;

/** `sizeof(GDEFSTOR_ModelBackend)` before @ref GDEFSTOR_ModelBackend::index_ctx
 * was added; gdefstor_store_create_model() accepts it. */
#define GDEFSTOR_MODEL_BACKEND_SIZE_V1 \
  offsetof(GDEFSTOR_ModelBackend, index_ctx)

/** `sizeof(GDEFSTOR_ModelBackend)` before @ref GDEFSTOR_ModelBackend::blind_ctx
 * was added; gdefstor_store_create_model() accepts it. */
#define GDEFSTOR_MODEL_BACKEND_SIZE_V2 \
  offsetof(GDEFSTOR_ModelBackend, blind_ctx)

/** `sizeof(GDEFSTOR_ModelBackend)` before @ref GDEFSTOR_ModelBackend::sys_ctx
 * was added; gdefstor_store_create_model() accepts it. */
#define GDEFSTOR_MODEL_BACKEND_SIZE_V3 \
  offsetof(GDEFSTOR_ModelBackend, sys_ctx)

/** `sizeof(GDEFSTOR_ModelBackend)` before @ref GDEFSTOR_ModelBackend::lang_ctx
 * was added; gdefstor_store_create_model() accepts it. */
#define GDEFSTOR_MODEL_BACKEND_SIZE_V4 \
  offsetof(GDEFSTOR_ModelBackend, lang_ctx)

/** `sizeof(GDEFSTOR_ModelBackend)` before
 * @ref GDEFSTOR_ModelBackend::restore_ctx was added;
 * gdefstor_store_create_model() accepts it. */
#define GDEFSTOR_MODEL_BACKEND_SIZE_V5 \
  offsetof(GDEFSTOR_ModelBackend, restore_ctx)

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
