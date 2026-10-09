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
};

struct GDEFSTOR_Store {
  const GDEFSTOR_Allocator * alloc;
  GDEFSTOR_Limits limits;
  GDEFSTOR_Backend backend;
  GDEFSTOR_Gate * gates;
  GDEFSTOR_Write * opens;
  GDEFSTOR_Meta * meta;
};

struct GDEFSTOR_Revision {
  const GDEFSTOR_Allocator * alloc;
  uint64_t seq;
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

#endif /* GHOTI_IO_GDEFSTOR_SRC_INTERNAL_H */
