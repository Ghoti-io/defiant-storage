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
 * Cross-file state for the port. Not installed.
 */

#ifndef GHOTI_IO_GDEFSTOR_SRC_INTERNAL_H
#define GHOTI_IO_GDEFSTOR_SRC_INTERNAL_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/cutil/fiber.h>
#include <stddef.h>
#include <stdint.h>

/* Renames for symbols that are not part of the installed API. */
#define gdefstor_alloc GHOTIIO_DEFSTOR(gdefstor_alloc)
#define gdefstor_buf_append GHOTIIO_DEFSTOR(gdefstor_buf_append)
#define gdefstor_check_payload GHOTIIO_DEFSTOR(gdefstor_check_payload)
#define gdefstor_count GHOTIIO_DEFSTOR(gdefstor_count)
#define gdefstor_decode_outbox GHOTIIO_DEFSTOR(gdefstor_decode_outbox)
#define gdefstor_dup GHOTIIO_DEFSTOR(gdefstor_dup)
#define gdefstor_encode_outbox GHOTIIO_DEFSTOR(gdefstor_encode_outbox)
#define gdefstor_free_pending GHOTIIO_DEFSTOR(gdefstor_free_pending)
#define gdefstor_json_escape_append GHOTIIO_DEFSTOR(gdefstor_json_escape_append)
#define gdefstor_key_hex GHOTIIO_DEFSTOR(gdefstor_key_hex)
#define gdefstor_limits_resolve GHOTIIO_DEFSTOR(gdefstor_limits_resolve)
#define gdefstor_lookup GHOTIIO_DEFSTOR(gdefstor_lookup)
#define gdefstor_mem GHOTIIO_DEFSTOR(gdefstor_mem)
#define gdefstor_mem0 GHOTIIO_DEFSTOR(gdefstor_mem0)
#define gdefstor_meta_add GHOTIIO_DEFSTOR(gdefstor_meta_add)
#define gdefstor_meta_drop GHOTIIO_DEFSTOR(gdefstor_meta_drop)
#define gdefstor_meta_find GHOTIIO_DEFSTOR(gdefstor_meta_find)
#define gdefstor_model_finish GHOTIIO_DEFSTOR(gdefstor_model_finish)
#define gdefstor_plant_enforce_limit GHOTIIO_DEFSTOR(gdefstor_plant_enforce_limit)
#define gdefstor_plant_keep_outbox GHOTIIO_DEFSTOR(gdefstor_plant_keep_outbox)
#define gdefstor_plant_see_uncommitted \
  GHOTIIO_DEFSTOR(gdefstor_plant_see_uncommitted)
#define gdefstor_span_ok GHOTIIO_DEFSTOR(gdefstor_span_ok)
#define gdefstor_stage GHOTIIO_DEFSTOR(gdefstor_stage)

enum {
  GDEFSTOR_KIND_REV = 1,
  GDEFSTOR_KIND_BLOB = 2,
  GDEFSTOR_KIND_PROJ = 3,
  GDEFSTOR_KIND_OUT = 4
};

/* Planted by tools/check-defects.sh. Defined once, in util.c. */
extern int gdefstor_plant_see_uncommitted;
extern int gdefstor_plant_keep_outbox;
extern int gdefstor_plant_enforce_limit;

typedef struct GDEFSTOR_Pending {
  int kind;
  int encoded;
  int drop;
  char * key;
  size_t key_len;
  uint8_t * bytes;
  size_t len;
  char * id;
  size_t id_len;
  char * stream;
  size_t stream_len;
  uint8_t * payload;
  size_t payload_len;
  struct GDEFSTOR_Pending * next;
} GDEFSTOR_Pending;

typedef struct GDEFSTOR_Waiter {
  GCU_Fiber * fiber;
  struct GDEFSTOR_Waiter * next;
} GDEFSTOR_Waiter;

typedef struct GDEFSTOR_Gate {
  char * tenant;
  size_t tenant_len;
  int held;
  GDEFSTOR_Waiter * head;
  GDEFSTOR_Waiter * tail;
  struct GDEFSTOR_Gate * next;
} GDEFSTOR_Gate;

typedef struct GDEFSTOR_Meta {
  char * tenant;
  size_t tenant_len;
  char * id;
  size_t id_len;
  int64_t lease_until;
  int parked;
  struct GDEFSTOR_Meta * next;
} GDEFSTOR_Meta;

struct GDEFSTOR_Write {
  GDEFSTOR_Store * store;
  char * tenant;
  size_t tenant_len;
  GDEFSTOR_Limits limits;
  GDEFSTOR_Pending * head;
  GDEFSTOR_Pending * tail;
  int alive;
  /* The entity port's backend transaction, opened by the first entity
   * operation; NULL until then. A write with one stages no key/bytes rows. */
  void * model_txn;
  /* Set when an entity operation failed after it had written to model_txn:
   * the commit then aborts and returns this. */
  GDEFSTOR_Result model_broken;
};

struct GDEFSTOR_Store {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_Limits limits;
  GDEFSTOR_Backend backend;
  /* The entity port (model.h); has_model is 0 for a key/bytes-only store. */
  GDEFSTOR_ModelBackend model;
  int has_model;
  GDEFSTOR_Gate * gates;
  GDEFSTOR_Write * opens;
  GDEFSTOR_Meta * meta;
};

struct GDEFSTOR_Revision {
  const GDEFSTOR_Allocator * alloc;
  uint64_t seq;
  uint64_t row;
  char * entity;
  size_t entity_len;
  char * type_name;
  size_t type_len;
  char * body;
  size_t body_len;
  char * lang_json;
  size_t lang_json_len;
};

struct GDEFSTOR_Projection {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_Column * columns;
  size_t count;
  char * names;
  uint8_t * values;
};

struct GDEFSTOR_Outbox_Row {
  const GDEFSTOR_Allocator * alloc;
  char * id;
  size_t id_len;
  char * stream;
  size_t stream_len;
  uint64_t seq;
  uint8_t * payload;
  size_t payload_len;
  int parked;
};

const GDEFSTOR_Allocator * gdefstor_alloc(const GDEFSTOR_Allocator * in);

void * gdefstor_mem(const GDEFSTOR_Allocator * alloc, size_t n);

void * gdefstor_mem0(const GDEFSTOR_Allocator * alloc, size_t n);

char * gdefstor_dup(const GDEFSTOR_Allocator * alloc, const char * p,
    size_t n);

void gdefstor_limits_resolve(const GDEFSTOR_Limits * in,
    GDEFSTOR_Limits * out);

int gdefstor_span_ok(const char * p, size_t n, size_t cap);

GDEFSTOR_Result gdefstor_key_hex(const GDEFSTOR_Allocator * alloc,
    const char * prefix, const char * a, size_t a_len, const char * b,
    size_t b_len, int with_b, char ** out, size_t * out_len);

GDEFSTOR_Result gdefstor_lookup(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len, int uncommitted,
    uint8_t ** out_bytes, size_t * out_len);

GDEFSTOR_Result gdefstor_stage(GDEFSTOR_Write * write, GDEFSTOR_Pending * row);

GDEFSTOR_Result gdefstor_count(GDEFSTOR_Store * store, GDEFSTOR_Write * write,
    const char * prefix, size_t prefix_len, size_t * out_count);

void gdefstor_free_pending(const GDEFSTOR_Allocator * alloc,
    GDEFSTOR_Pending * row);

GDEFSTOR_Meta * gdefstor_meta_find(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * id, size_t id_len);

GDEFSTOR_Result gdefstor_meta_add(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * id, size_t id_len);

void gdefstor_meta_drop(GDEFSTOR_Store * store, const char * tenant,
    size_t tenant_len, const char * id, size_t id_len);

GDEFSTOR_Result gdefstor_json_escape_append(char ** buf, size_t * len,
    size_t * cap, const GDEFSTOR_Allocator * alloc, const char * s,
    size_t n);

GDEFSTOR_Result gdefstor_buf_append(char ** buf, size_t * len, size_t * cap,
    const GDEFSTOR_Allocator * alloc, const char * s, size_t n);

GDEFSTOR_Result gdefstor_encode_outbox(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Limits * limits, const char * id, size_t id_len,
    const char * stream, size_t stream_len, uint64_t seq,
    const uint8_t * payload, size_t payload_len, uint8_t ** out,
    size_t * out_len);

GDEFSTOR_Result gdefstor_decode_outbox(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Limits * limits, const uint8_t * bytes, size_t len,
    GDEFSTOR_Outbox_Row ** out_row);

GDEFSTOR_Result gdefstor_check_payload(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Limits * limits, const uint8_t * payload, size_t len);

/* End the entity port's transaction of a write, if it has one: commit it
 * (when @p commit and the write is not broken) or abort it. model.c. */
GDEFSTOR_Result gdefstor_model_finish(GDEFSTOR_Write * write, int commit);

/* ---- the entity part of a snapshot (model_snapshot.c) -------------------
 * The seam between the snapshot (snapshot.c) and the entity port: the
 * snapshot calls only these. */

#define gdefstor_snap_model_free GHOTIIO_DEFSTOR(gdefstor_snap_model_free)
#define gdefstor_snap_model_export GHOTIIO_DEFSTOR(gdefstor_snap_model_export)
#define gdefstor_snap_model_member GHOTIIO_DEFSTOR(gdefstor_snap_model_member)
#define gdefstor_snap_model_check GHOTIIO_DEFSTOR(gdefstor_snap_model_check)
#define gdefstor_snap_model_import GHOTIIO_DEFSTOR(gdefstor_snap_model_import)
#define gdefstor_snap_model_empty GHOTIIO_DEFSTOR(gdefstor_snap_model_empty)
#define gdefstor_snap_model_none GHOTIIO_DEFSTOR(gdefstor_snap_model_none)
#define gdefstor_snap_model_can_hold \
  GHOTIIO_DEFSTOR(gdefstor_snap_model_can_hold)
#define gdefstor_snap_model_open GHOTIIO_DEFSTOR(gdefstor_snap_model_open)


/* One member of the entity part: its name and its bytes, owned. */
typedef struct GDEFSTOR_SnapMember {
  char * name;
  size_t name_len;
  uint8_t * bytes;
  size_t len;
} GDEFSTOR_SnapMember;

/* The entity truth of one tenant, read from a store or from a snapshot. */
typedef struct GDEFSTOR_SnapModel GDEFSTOR_SnapModel;

void gdefstor_snap_model_free(GDEFSTOR_SnapModel * m);

/* Read the tenant's truth through the backend's `dump` and spell it as the
 * members of a snapshot's entity part, in the order they are written: none
 * for a tenant that holds no truth, or a store without the entity port.
 * Free each member's name and bytes and the array with the store's
 * allocator. */
GDEFSTOR_Result gdefstor_snap_model_export(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const GDEFSTOR_Limits * cap,
    GDEFSTOR_SnapMember ** out, size_t * out_count);

/* Hand an entity part every member the manifest lists under "model", in
 * its order (gdefstor_snap_model_member(); the first call makes *m), then
 * check the whole (gdefstor_snap_model_check()). Each returns ERR_CORRUPT
 * for content that is not what an export writes. */
GDEFSTOR_Result gdefstor_snap_model_member(GDEFSTOR_SnapModel ** m,
    const GDEFSTOR_Allocator * alloc, const GDEFSTOR_Limits * cap,
    const char * name, size_t name_len, const uint8_t * bytes, size_t len);
GDEFSTOR_Result gdefstor_snap_model_check(GDEFSTOR_SnapModel * m);

/* Whether the tenant holds any entity truth (a declaration, an entity, a
 * draft, a blind row, a record, a counter or an outbox row), as of the last
 * commit. A store without the entity port holds none. */
GDEFSTOR_Result gdefstor_snap_model_empty(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, int * out_empty);

/* Write a checked entity part into @p write's tenant, which the caller has
 * found empty. Nothing is committed here. ERR_UNSUPPORTED when the part is
 * not empty and the store cannot hold it (no entity port, no restore
 * members, or records, counters or outbox rows on a backend without them). */
GDEFSTOR_Result gdefstor_snap_model_import(GDEFSTOR_Write * write,
    const GDEFSTOR_SnapModel * m);

/* Whether a checked entity part holds nothing. */
int gdefstor_snap_model_none(const GDEFSTOR_SnapModel * m);

/* Whether the store can be imported into: the entity port, with the
 * restore members and records, counters and the outbox (model_backend.h). */
int gdefstor_snap_model_can_hold(const GDEFSTOR_Store * store);

/* Open @p write's entity transaction now (gdefstor_model_txn()). */
GDEFSTOR_Result gdefstor_snap_model_open(GDEFSTOR_Write * write);

#endif /* GHOTI_IO_GDEFSTOR_SRC_INTERNAL_H */
