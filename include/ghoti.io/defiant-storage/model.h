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
 * @file model.h
 * @brief The entity port: types, entities, revisions, field trees, drafts.
 * @stability free
 *
 * This is the port the entity layer speaks (AD-13, AD-14): entities of
 * declared types, each with one linear revision history of a typed, nested
 * field tree, and records, counters and an outbox beside them. It is the
 * port's one storage model. The model, its limits and the reasons for each
 * rule are in documentation/model.md; what follows is the contract per call.
 *
 * A store is opened with gdefstor_store_open() over a backend
 * (model_backend.h).
 *
 * Writes take a GDEFSTOR_Write: the tenant's writer queue and one backend
 * transaction. Records, counters and the outbox (below) commit in that
 * transaction with the entities. A call that fails before it reached the
 * backend leaves the write as it was; one that fails after (an I/O error, a
 * full allocator) marks the write broken, and its commit aborts and returns
 * that error. Reads take the store and the tenant and see the last commit.
 *
 * Identifiers: a public id, a creator, an actor and a subject are 1 to
 * GDEFSTOR_Limits::max_public_id_bytes bytes of `[0-9A-Za-z_-]` (an actor or a
 * creator may also be empty: the system). Nothing here generates a public id
 * (AD-22 is the caller's).
 *
 * Languages: an entity holds one tree per language, all under one revision
 * history (model.md, "Languages"). A language is a tag of 1 to
 * ::GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES bytes of `[A-Za-z0-9-]` (`en`, `fr`,
 * `pt-BR`), compared byte for byte: `en` and `EN` are two languages. Every
 * call that reads or writes a tree names its language, and a query binds one
 * (query.h).
 */

#ifndef GHOTI_IO_GDEFSTOR_MODEL_H
#define GHOTI_IO_GDEFSTOR_MODEL_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/core.h>
#include <ghoti.io/defiant-storage/store.h>
#include <ghoti.io/defiant-storage/value.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits --------------------------------------------------------------- */

/** Groups nested below the root group. */
#define GDEFSTOR_MODEL_MAX_DEPTH 8
/** Fields of one type at every depth, retired ones included. */
#define GDEFSTOR_MODEL_MAX_FIELDS 512
/** Fields directly inside one group, retired ones included. */
#define GDEFSTOR_MODEL_MAX_GROUP_FIELDS 64
/** Bytes of a field name: `[A-Za-z0-9_]`, unique among siblings ignoring ASCII case. */
#define GDEFSTOR_MODEL_MAX_NAME_BYTES 64
/** Items in one tree, the root item included. */
#define GDEFSTOR_MODEL_MAX_ITEMS 65536
/** Bytes of one text, reference or personal value. */
#define GDEFSTOR_MODEL_MAX_VALUE_BYTES (1024u * 1024u)
/** Bytes of every value of one tree together. */
#define GDEFSTOR_MODEL_MAX_TREE_BYTES (16u * 1024u * 1024u)
/** Bytes of the reason a revision records. */
#define GDEFSTOR_MODEL_MAX_WHY_BYTES 4096
/** Bytes of one draft. */
#define GDEFSTOR_MODEL_MAX_DRAFT_BYTES (32u * 1024u * 1024u)
/** Digits after the point of a decimal field, at most. */
#define GDEFSTOR_MODEL_MAX_SCALE 18
/** The largest magnitude of a decimal's unscaled integer: 10^18 - 1. */
#define GDEFSTOR_MODEL_MAX_DECIMAL INT64_C(999999999999999999)
/**
 * Order keys of siblings closer than this are renumbered: every sibling gets
 * the key 1, 2, 3 ... as a new item version, in the same revision.
 */
#define GDEFSTOR_MODEL_ORDER_EPSILON (1.0 / 1048576.0)
/** Bytes of a language tag: `[A-Za-z0-9-]`, at least one. */
#define GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES 35
/**
 * Languages one entity holds trees in at once. A save that would leave more
 * is ::GDEFSTOR_ERR_LIMIT and writes nothing; it is a cap per entity, and the
 * tables have no column or row per language that would set another.
 */
#define GDEFSTOR_MODEL_MAX_LANGUAGES 64

/* ---- the store ------------------------------------------------------------ */

struct GDEFSTOR_ModelBackend;

/**
 * @brief Open a store over an entity backend.
 *
 * @p model is copied, its `ctx` borrowed until gdefstor_store_destroy(),
 * which destroys it.
 *
 * @param model Required: every required member set, and @ref
 *   GDEFSTOR_ModelBackend::size this header's (model_backend.h).
 * @param allocator NULL uses the default.
 * @param limits NULL uses gdefstor_limits_default().
 * @param out_store Receives the store on success. Not written on failure.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (a required member missing,
 *   an optional family half set, or another size), or ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_store_open(
    const struct GDEFSTOR_ModelBackend * model,
    const GDEFSTOR_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store);

/* ---- type declarations ---------------------------------------------------- */

/**
 * @brief One field of a declaration. The caller owns every pointer for the call.
 *
 * @ref size is `sizeof(GDEFSTOR_FieldSpec)`.
 */
typedef struct GDEFSTOR_FieldSpec {
  size_t size;              ///< `sizeof(GDEFSTOR_FieldSpec)`.
  const char * name;        ///< `[A-Za-z0-9_]{1,64}`.
  size_t name_len;          ///< Length of @ref name.
  int type;                 ///< A ::GDEFSTOR_FieldType.
  int scale;                ///< DECIMAL: digits after the point, 0 to 18. Else 0.
  unsigned flags;           ///< ::GDEFSTOR_FIELD_MULTIPLE and the others.
  /** PERSONAL: the name of a sibling REFERENCE field (single, not personal)
   * whose value is the subject, or NULL for the entity's own subject. */
  const char * subject;
  size_t subject_len;       ///< Length of @ref subject.
  const struct GDEFSTOR_FieldSpec * children; ///< GROUP: its fields, at least one.
  size_t child_count;       ///< How many.
} GDEFSTOR_FieldSpec;

/** A type declaration. @ref size is `sizeof(GDEFSTOR_TypeSpec)`. */
typedef struct GDEFSTOR_TypeSpec {
  size_t size;                      ///< `sizeof(GDEFSTOR_TypeSpec)`.
  const char * name;                ///< A machine name, `<module>.<name>` (AD-35).
  size_t name_len;                  ///< Length of @ref name.
  const GDEFSTOR_FieldSpec * fields;///< The root group's fields.
  size_t field_count;               ///< How many.
} GDEFSTOR_TypeSpec;

/**
 * @brief Declare a type, or change its declaration additively.
 *
 * The spec is the whole desired field tree. Against what is stored: a field
 * that is new is added; a stored field the spec leaves out is retired (its
 * data stays readable, and stays in the current tree, but nothing can write
 * it); a retired field the spec names again, unchanged, is restored. A field
 * whose type, scale, ::GDEFSTOR_FIELD_MULTIPLE, ::GDEFSTOR_FIELD_PERSONAL or
 * subject differ from what is stored is refused with
 * ::GDEFSTOR_ERR_UNSUPPORTED: that is a new field and a conversion (AD-15).
 * REQUIRED and INDEXED may change. A spec equal to what is stored writes
 * nothing.
 *
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (a name, a type or a shape
 *   that is wrong), ::GDEFSTOR_ERR_LIMIT, ::GDEFSTOR_ERR_UNSUPPORTED, or a
 *   backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_type_declare(GDEFSTOR_Write * write,
    const GDEFSTOR_TypeSpec * spec);

/** A stored declaration. Opaque. */
typedef struct GDEFSTOR_Type GDEFSTOR_Type;

/** The parent of a field of the root group. */
#define GDEFSTOR_TYPE_ROOT SIZE_MAX

/**
 * @brief Read a type's declaration as of the last commit.
 *
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID for a type never declared.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_type_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    GDEFSTOR_Type ** out_type);

/** @brief Release a declaration. NULL is ignored. */
GDEFSTOR_API void gdefstor_type_free(GDEFSTOR_Type * type);

/**
 * @brief The type's machine name, as it was declared.
 *
 * A backend that keeps each type in tables of its own needs it where it is
 * handed only the parsed declaration (GDEFSTOR_ModelBackend::type_store, the
 * types of a GDEFSTOR_ModelPlan).
 *
 * @param out_name Receives the name; borrowed until the type is freed. Not
 *   NUL-terminated by contract.
 * @param out_len Receives its length.
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID for a NULL argument.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_type_name(const GDEFSTOR_Type * type,
    const char ** out_name, size_t * out_len);

/** @return Fields at every depth, retired ones included, in declaration order. */
GDEFSTOR_API size_t gdefstor_type_field_count(const GDEFSTOR_Type * type);

/** One field of a stored declaration. Pointers are valid until the type is freed. */
typedef struct GDEFSTOR_FieldInfo {
  size_t size;              ///< Set by the caller: `sizeof(GDEFSTOR_FieldInfo)`.
  const char * name;        ///< The field's name.
  size_t name_len;          ///< Its length.
  const char * path;        ///< Names from the root group, joined by `.`.
  size_t path_len;          ///< Its length.
  int type;                 ///< A ::GDEFSTOR_FieldType.
  int scale;                ///< DECIMAL: digits after the point.
  unsigned flags;           ///< As declared.
  int retired;              ///< Non-zero when it or an enclosing group is retired.
  const char * subject;     ///< PERSONAL: the subject field, or NULL.
  size_t subject_len;       ///< Its length.
  size_t parent;            ///< Index of the enclosing GROUP, or ::GDEFSTOR_TYPE_ROOT.
  uint32_t id;              ///< Stable for the life of the field; never reused.
  /** GROUP or MULTIPLE: the group its items live in (the root group is 0).
   * Otherwise UINT32_MAX. Stable; never reused. */
  uint32_t group;
  /** A single scalar: its column in the enclosing group. A multiple scalar:
   * 0, the one column of its own group. A GROUP: UINT32_MAX. Stable. */
  uint32_t column;
  uint32_t in_group;        ///< The group holding the field (its parent's, or 0).
} GDEFSTOR_FieldInfo;

/**
 * @brief Describe field @p index.
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID for an index out of range
 *   or an @p out whose size is not this header's.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_type_field(const GDEFSTOR_Type * type,
    size_t index, GDEFSTOR_FieldInfo * out);

/**
 * @brief The index of the field at a dotted path, retired or not.
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID when there is none.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_type_find(const GDEFSTOR_Type * type,
    const char * path, size_t path_len, size_t * out_index);

/** What gdefstor_type_index_state() says of a field's index (AD-15). */
typedef enum GDEFSTOR_IndexState {
  /** The field is not declared ::GDEFSTOR_FIELD_INDEXED (or is a group). */
  GDEFSTOR_INDEX_NONE = 0,
  /** Declared indexed, and its index is not built yet: the field reads as
   * "indexing". Queries on it answer exactly as they will once it is built;
   * only their cost differs. */
  GDEFSTOR_INDEX_INDEXING,
  /** Declared indexed, and its index is built and serves queries. */
  GDEFSTOR_INDEX_READY
} GDEFSTOR_IndexState;

/**
 * @brief Whether the index of the field at a dotted path is built, as of the
 * last commit.
 *
 * Declaring a field ::GDEFSTOR_FIELD_INDEXED commits at once; the index itself
 * is built after that commit, outside the declaring write's transaction
 * (AD-15: a build never holds a lock on a large table inside a step), and a
 * backend may still be building it, or may have to build it again after a
 * failed attempt. Until it is built the field reads
 * ::GDEFSTOR_INDEX_INDEXING. A backend with no separate index (the memory
 * backend) reads every indexed field ::GDEFSTOR_INDEX_READY.
 *
 * @param out_state Receives a ::GDEFSTOR_IndexState. Written only on success.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (a type never declared, a path
 *   it does not have, a NULL argument), or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_type_index_state(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const char * path, size_t path_len, int * out_state);

/* ---- field trees ---------------------------------------------------------- */

/**
 * @brief An entity's field tree. Opaque.
 *
 * Item 0 is the root item, which holds the entity's own scalar fields. Every
 * other item sits under one field of its parent: a GROUP field (its items hold
 * that group's fields) or a MULTIPLE scalar field (each item holds one value).
 * Items carry an id that is stable for the life of the item. A tree built for
 * a save names an existing item by its id and a new one with id 0; it states
 * order by the order items are added, never by key.
 */
typedef struct GDEFSTOR_Tree GDEFSTOR_Tree;

/**
 * @brief An empty tree: the root item, no values.
 * @param allocator NULL uses the default. Every allocation of the tree uses it.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_tree_create(
    const GDEFSTOR_Allocator * allocator, GDEFSTOR_Tree ** out_tree);

/** @brief Release a tree. NULL is ignored. */
GDEFSTOR_API void gdefstor_tree_free(GDEFSTOR_Tree * tree);

/**
 * @brief Append an item under @p field of item @p parent, after its siblings.
 *
 * @param id 0 for a new item, or the id of an item the entity has now.
 * @param out_item Receives the new item's index. May be NULL.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, ::GDEFSTOR_ERR_LIMIT, or
 *   ::GDEFSTOR_ERR_OOM. Whether @p field exists is checked by the save.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_tree_add(GDEFSTOR_Tree * tree,
    size_t parent, const char * field, size_t field_len, uint64_t id,
    size_t * out_item);

/**
 * @brief Set scalar @p field of item @p item, replacing what was set.
 *
 * For an item of a MULTIPLE scalar field, @p field_len is 0: the item's own
 * value. A @p value of type 0 unsets. The bytes are copied.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_tree_set(GDEFSTOR_Tree * tree,
    size_t item, const char * field, size_t field_len,
    const GDEFSTOR_Value * value);

/** @return Items, the root included. */
GDEFSTOR_API size_t gdefstor_tree_item_count(const GDEFSTOR_Tree * tree);

/** One item. Pointers are valid until the tree is freed or changed. */
typedef struct GDEFSTOR_TreeItem {
  size_t size;           ///< Set by the caller: `sizeof(GDEFSTOR_TreeItem)`.
  uint64_t id;           ///< Its id; 0 for a new item of a tree being built.
  size_t parent;         ///< Its parent's index, ::GDEFSTOR_TYPE_ROOT for the root.
  const char * field;    ///< The field of the parent it sits under ("" for the root).
  size_t field_len;      ///< Its length.
  double order;          ///< A read tree: its stored order key. A built tree: 0.
  size_t value_count;    ///< Values set on it.
} GDEFSTOR_TreeItem;

/**
 * @brief Describe item @p index.
 *
 * A tree read from storage lists a parent before its children and siblings in
 * order.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_tree_item(const GDEFSTOR_Tree * tree,
    size_t index, GDEFSTOR_TreeItem * out);

/**
 * @brief The @p k th value set on item @p item, and its field ("" for an
 * item's own value). Borrowed until the tree is freed or changed.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_tree_value(const GDEFSTOR_Tree * tree,
    size_t item, size_t k, const char ** out_field, size_t * out_field_len,
    GDEFSTOR_Value * out_value);

/**
 * @brief The value of @p field on item @p item. Sets @p out_value's type to 0
 * when there is none. Borrowed until the tree is freed or changed.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_tree_get(const GDEFSTOR_Tree * tree,
    size_t item, const char * field, size_t field_len,
    GDEFSTOR_Value * out_value);

/** Bytes of a blind-index MAC: an HMAC-SHA-256 (AD-14, AD-19). */
#define GDEFSTOR_BLIND_MAC_BYTES 32

/**
 * @brief The blind-index MAC of one personal value, as the caller computed it.
 *
 * Storage never sees a key or a plaintext (AD-19): the MAC arrives computed,
 * under the tenant's blind-index key for the field (AD-29), over the
 * normalized plaintext. It is opaque bytes here, compared only for equality.
 * @ref size is `sizeof(GDEFSTOR_BlindMac)`.
 */
typedef struct GDEFSTOR_BlindMac {
  size_t size;           ///< `sizeof(GDEFSTOR_BlindMac)`.
  const uint8_t * mac;   ///< ::GDEFSTOR_BLIND_MAC_BYTES bytes. Copied.
  size_t mac_len;        ///< Must be ::GDEFSTOR_BLIND_MAC_BYTES.
  uint32_t key_id;       ///< The key that made it, recorded with it (AD-14).
  uint32_t version;      ///< The normalization version, recorded with it (AD-14).
} GDEFSTOR_BlindMac;

/**
 * @brief Give the value of @p field on item @p item its blind-index MAC.
 *
 * The value must be set already (gdefstor_tree_set()); setting the value again
 * drops the MAC. For an item of a MULTIPLE field, @p field_len is 0. A save
 * writes the MAC into the blind table, never into a revision, group or
 * current row, as one row for each personal value that gets a new version
 * in that save (model.md, "The blind index"); an unchanged value writes
 * nothing. The save refuses a MAC on a field that is not personal with
 * ::GDEFSTOR_ERR_INVALID, and on a store whose backend has no blind table
 * with ::GDEFSTOR_ERR_UNSUPPORTED. A tree read from storage carries no MAC,
 * and gdefstor_tree_to_json() writes none.
 *
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID (a NULL argument, a MAC of
 *   another length, a size that is not this header's, or no value set there).
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_tree_set_blind(GDEFSTOR_Tree * tree,
    size_t item, const char * field, size_t field_len,
    const GDEFSTOR_BlindMac * mac);

/**
 * @brief Parse a tree from its JSON form (`defiant-tree/1`, model.md), which
 * names fields and so needs the declaration to type the values.
 *
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_FORMAT, ::GDEFSTOR_ERR_UNSUPPORTED (a
 *   newer version), ::GDEFSTOR_ERR_CORRUPT, ::GDEFSTOR_ERR_INVALID (a field
 *   the type does not have, or a value of the wrong shape),
 *   ::GDEFSTOR_ERR_LIMIT or ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_tree_from_json(const GDEFSTOR_Type * type,
    const uint8_t * json, size_t len, const GDEFSTOR_Allocator * allocator,
    GDEFSTOR_Tree ** out_tree);

/**
 * @brief Write a tree in its JSON form. Free the bytes with
 * gdefstor_bytes_free() and the tree's allocator.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_tree_to_json(const GDEFSTOR_Type * type,
    const GDEFSTOR_Tree * tree, uint8_t ** out_json, size_t * out_len);

/* ---- entities ------------------------------------------------------------- */

/**
 * @brief Create an entity: reserve its row and record who created it and when.
 *
 * It has no revision until the first save. The public id must be unused in
 * the tenant, by an entity of any type and by a draft that reserved it.
 *
 * @param creator The creating principal's public id, or empty for the system.
 * @param out_row Receives the internal row. May be NULL. Never leaves the server.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (an undeclared type, a public id
 *   that is taken or malformed), or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_create(GDEFSTOR_Write * write,
    const char * type, size_t type_len, const char * public_id,
    size_t public_id_len, const char * creator, size_t creator_len,
    int64_t created_ms, uint64_t * out_row);

/** An entity record. Opaque. */
typedef struct GDEFSTOR_Entity GDEFSTOR_Entity;

/** What an entity record says. Pointers are valid until it is freed. */
typedef struct GDEFSTOR_EntityInfo {
  size_t size;              ///< Set by the caller: `sizeof(GDEFSTOR_EntityInfo)`.
  uint64_t row;             ///< Internal row.
  const char * type;        ///< Its type.
  size_t type_len;          ///< Its length.
  const char * public_id;   ///< Its public id.
  size_t public_id_len;     ///< Its length.
  const char * creator;     ///< Who created it; empty for the system.
  size_t creator_len;       ///< Its length.
  int64_t created_ms;       ///< When.
  uint64_t revision;        ///< The newest revision, which is the current one; 0 for none.
  int published;            ///< The newest state.
  int deleted;              ///< The newest state (a soft delete).
} GDEFSTOR_EntityInfo;

/**
 * @brief Read an entity record as of the last commit.
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID when there is none.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, GDEFSTOR_Entity ** out_entity);

/** @brief Describe a record. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_info(
    const GDEFSTOR_Entity * entity, GDEFSTOR_EntityInfo * out);

/** @brief Release a record. NULL is ignored. */
GDEFSTOR_API void gdefstor_entity_free(GDEFSTOR_Entity * entity);

/**
 * @brief Append a state change (published, soft-deleted) with its actor and time.
 *
 * A state equal to the newest one appends nothing.
 *
 * @param out_changed Set to 1 when a row was appended, 0 when not. May be NULL.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_set_state(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, int published, int deleted,
    const char * actor, size_t actor_len, int64_t when_ms, int * out_changed);

/** Who, when and why for a revision. */
typedef struct GDEFSTOR_RevisionMeta {
  size_t size;          ///< `sizeof(GDEFSTOR_RevisionMeta)`.
  const char * who;     ///< The principal's public id; empty for the system.
  size_t who_len;       ///< Its length.
  int64_t when_ms;      ///< When, by the caller's clock.
  const char * why;     ///< Free text, UTF-8, up to 4096 bytes. May be empty.
  size_t why_len;       ///< Its length.
} GDEFSTOR_RevisionMeta;

/** One language's part of a save. @ref size is `sizeof(GDEFSTOR_LanguageTree)`. */
typedef struct GDEFSTOR_LanguageTree {
  size_t size;              ///< `sizeof(GDEFSTOR_LanguageTree)`.
  const char * language;    ///< A language tag.
  size_t language_len;      ///< Its length.
  /** The language's whole new tree, or NULL to remove the language: every
   * item of it is closed, and it has no tree from this revision on. */
  const GDEFSTOR_Tree * tree;
} GDEFSTOR_LanguageTree;

/**
 * @brief Write revision N+1 of an entity from the whole new trees of the
 * languages it names.
 *
 * @p base_revision must be the entity's newest revision (0 before the first);
 * when the head has moved this is ::GDEFSTOR_ERR_CONFLICT and nothing is
 * written. Each tree is validated against the type as declared now. Per
 * language, only the items that changed are written: each gets a new version,
 * its old one closed; an item missing from the tree is closed; an unchanged
 * item is not touched. A language the save does not name keeps its tree and
 * gets no row; a named language whose tree is unchanged gets none either. All
 * the languages' rows are one revision: one number, one who, when and why.
 * Moving an item changes only its order key (the midpoint of its new
 * neighbours), unless the gap is below ::GDEFSTOR_MODEL_ORDER_EPSILON, when
 * all its siblings are renumbered in this same revision. Retired fields keep
 * their stored data; a tree may restate it but not change it.
 *
 * Item ids are the entity's, across languages: an item belongs to the
 * language it was created in, and a tree names only items of its own
 * language. The first tree of a language gets a new root item.
 *
 * @param trees The languages, each at most once, at most
 *   ::GDEFSTOR_MODEL_MAX_LANGUAGES. May be NULL when @p count is 0 (a save
 *   that changes nothing).
 * @param out_revision Receives N+1, or 0 when no language changed and nothing
 *   was written. May be NULL.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_CONFLICT, ::GDEFSTOR_ERR_INVALID (a
 *   tag that is not one, a language named twice, a tree that is wrong),
 *   ::GDEFSTOR_ERR_LIMIT (among others, more than
 *   ::GDEFSTOR_MODEL_MAX_LANGUAGES languages afterwards), or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_save_languages(
    GDEFSTOR_Write * write, const char * public_id, size_t public_id_len,
    uint64_t base_revision, const GDEFSTOR_RevisionMeta * meta,
    const GDEFSTOR_LanguageTree * trees, size_t count,
    uint64_t * out_revision);

/**
 * @brief Write revision N+1 from one language's whole new tree:
 * gdefstor_entity_save_languages() with that one language.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_save(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    const GDEFSTOR_RevisionMeta * meta, const char * language,
    size_t language_len, const GDEFSTOR_Tree * tree, uint64_t * out_revision);

/**
 * @brief Remove a language: a revision that closes every current item of its
 * tree. History keeps it (an as-of read of an earlier revision finds it), and
 * a later save of the language starts a new tree.
 *
 * A language the entity has no tree in writes nothing: @p out_revision is 0.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_remove_language(
    GDEFSTOR_Write * write, const char * public_id, size_t public_id_len,
    uint64_t base_revision, const GDEFSTOR_RevisionMeta * meta,
    const char * language, size_t language_len, uint64_t * out_revision);

/**
 * @brief Write a revision whose trees are revision @p to_revision's, in every
 * language.
 *
 * An ordinary save, built from the past trees: items keep their ids (an item
 * removed since comes back under its old id) and their old order keys, old
 * rows are never touched, and every revision in between still reads as it did.
 * A language that had no tree at @p to_revision is removed; one removed since
 * comes back. Retired fields keep their current data. Reverting to the current
 * trees writes nothing.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_revert(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len, uint64_t base_revision,
    uint64_t to_revision, const GDEFSTOR_RevisionMeta * meta,
    uint64_t * out_revision);

/**
 * @brief Read an entity's tree in one language: the current one, from the
 * current tables, when @p revision is 0; otherwise the tree as of that
 * revision, from history.
 *
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID for no such entity or
 *   revision, a tag that is not one, or no tree in that language then (an
 *   entity with no revision has no tree; gdefstor_entity_languages() says
 *   which languages there are).
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_tree(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, const char * language, size_t language_len,
    uint64_t revision, GDEFSTOR_Tree ** out_tree);

/** The languages an entity has trees in. Opaque. */
typedef struct GDEFSTOR_Languages GDEFSTOR_Languages;

/**
 * @brief The languages an entity has a tree in: now (from the current tables)
 * when @p revision is 0, otherwise as of that revision. Sorted by bytes.
 *
 * @return ::GDEFSTOR_OK (an entity with no revision has none), or
 *   ::GDEFSTOR_ERR_INVALID for no such entity or revision.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_languages(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, uint64_t revision, GDEFSTOR_Languages ** out);

/** @return The languages in the list. */
GDEFSTOR_API size_t gdefstor_languages_count(const GDEFSTOR_Languages * l);

/** @brief Language @p index (0 is the lowest by bytes). Borrowed until freed;
 * not NUL-terminated by contract. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_languages_get(const GDEFSTOR_Languages * l,
    size_t index, const char ** out_language, size_t * out_len);

/** @brief Release a list. NULL is ignored. */
GDEFSTOR_API void gdefstor_languages_free(GDEFSTOR_Languages * l);

/** An entity's revisions and states. Opaque. */
typedef struct GDEFSTOR_History GDEFSTOR_History;

/** One revision. Pointers are valid until the history is freed. */
typedef struct GDEFSTOR_RevisionInfo {
  size_t size;           ///< Set by the caller: `sizeof(GDEFSTOR_RevisionInfo)`.
  uint64_t revision;     ///< Its number, 1, 2, 3 ...
  uint64_t commit_seq;   ///< The tenant's commit sequence of the write that made it.
  const char * who;      ///< Who.
  size_t who_len;        ///< Its length.
  int64_t when_ms;       ///< When.
  const char * why;      ///< Why.
  size_t why_len;        ///< Its length.
  /** The languages whose trees it changed (saved, removed, reverted), sorted
   * by bytes and joined by `,`: `de,fr`. */
  const char * languages;
  size_t languages_len;  ///< Its length.
} GDEFSTOR_RevisionInfo;

/** One state change. Pointers are valid until the history is freed. */
typedef struct GDEFSTOR_StateInfo {
  size_t size;           ///< Set by the caller: `sizeof(GDEFSTOR_StateInfo)`.
  int published;         ///< The state from here on.
  int deleted;           ///< The state from here on.
  const char * actor;    ///< Who changed it.
  size_t actor_len;      ///< Its length.
  int64_t when_ms;       ///< When.
} GDEFSTOR_StateInfo;

/**
 * @brief Read an entity's revisions and state changes, oldest first.
 *
 * With a @p language, only the revisions that changed that language's tree;
 * with NULL (and @p language_len 0), every revision. State changes are the
 * entity's, in either case all of them.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_history(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, const char * language, size_t language_len,
    GDEFSTOR_History ** out_history);

/** @return Revisions in the history. */
GDEFSTOR_API size_t gdefstor_history_revision_count(const GDEFSTOR_History * h);

/** @brief Describe revision @p index (0 is the oldest). */
GDEFSTOR_API GDEFSTOR_Result gdefstor_history_revision(
    const GDEFSTOR_History * h, size_t index, GDEFSTOR_RevisionInfo * out);

/** @return State changes in the history. */
GDEFSTOR_API size_t gdefstor_history_state_count(const GDEFSTOR_History * h);

/** @brief Describe state change @p index (0 is the oldest). */
GDEFSTOR_API GDEFSTOR_Result gdefstor_history_state(const GDEFSTOR_History * h,
    size_t index, GDEFSTOR_StateInfo * out);

/** @brief Release a history. NULL is ignored. */
GDEFSTOR_API void gdefstor_history_free(GDEFSTOR_History * h);

/** The difference between two revisions. Opaque. */
typedef struct GDEFSTOR_Diff GDEFSTOR_Diff;

/** What happened to an item. */
typedef enum GDEFSTOR_DiffKind {
  GDEFSTOR_DIFF_ADDED = 1, ///< In the later tree only.
  GDEFSTOR_DIFF_CHANGED,   ///< In both, different.
  GDEFSTOR_DIFF_REMOVED    ///< In the earlier tree only.
} GDEFSTOR_DiffKind;

/** A changed item's values differ. */
#define GDEFSTOR_DIFF_VALUES 0x1u
/** A changed item has another parent. */
#define GDEFSTOR_DIFF_PARENT 0x2u
/** A changed item has another order key. */
#define GDEFSTOR_DIFF_ORDER 0x4u

/** One item of a difference. Pointers are valid until the diff is freed. */
typedef struct GDEFSTOR_DiffItem {
  size_t size;          ///< Set by the caller: `sizeof(GDEFSTOR_DiffItem)`.
  int kind;             ///< A ::GDEFSTOR_DiffKind.
  unsigned what;        ///< CHANGED: ::GDEFSTOR_DIFF_VALUES and the others.
  uint64_t item;        ///< The item's id.
  const char * field;   ///< The path of the field it sits under ("" for the root item).
  size_t field_len;     ///< Its length.
  const char * language; ///< The language of the tree it is in.
  size_t language_len;  ///< Its length.
} GDEFSTOR_DiffItem;

/**
 * @brief The items added, changed and removed from revision @p from to @p to.
 *
 * Revision 0 is the empty tree before the first. With a @p language, the
 * items of that language's tree; with NULL (and @p language_len 0), of every
 * language, each item naming its own, so the languages a revision touched
 * are those of the diff from the one before it. Items are listed by id.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_entity_diff(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, const char * language, size_t language_len,
    uint64_t from, uint64_t to, GDEFSTOR_Diff ** out_diff);

/** @return Items in the difference. */
GDEFSTOR_API size_t gdefstor_diff_count(const GDEFSTOR_Diff * diff);

/** @brief Describe item @p index of the difference. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_diff_item(const GDEFSTOR_Diff * diff,
    size_t index, GDEFSTOR_DiffItem * out);

/** @brief Release a difference. NULL is ignored. */
GDEFSTOR_API void gdefstor_diff_free(GDEFSTOR_Diff * diff);

/* ---- drafts --------------------------------------------------------------- */

/** One language of a draft. @ref size is `sizeof(GDEFSTOR_DraftTree)`. */
typedef struct GDEFSTOR_DraftTree {
  size_t size;            ///< `sizeof(GDEFSTOR_DraftTree)`.
  const char * language;  ///< A language tag.
  size_t language_len;    ///< Its length.
  const uint8_t * json;   ///< The language's whole tree, `defiant-tree/1`.
  size_t json_len;        ///< Its length.
} GDEFSTOR_DraftTree;

/**
 * @brief Store the draft of an entity: a JSON tree (`defiant-tree/1`) for each
 * language it changes, and the revision it was made from. One per entity, for
 * all its languages; a second put replaces it.
 *
 * For an entity that exists, @p type is NULL or its type and @p base_revision
 * is at most its newest. For a public id with no entity, the draft reserves it
 * (gdefstor_entity_create() then refuses it): @p type is required and
 * declared, and @p base_revision is 0. Each JSON is checked to be well formed
 * now; it is validated against the type when applied. Nothing outside the
 * draft is written. The stored draft is one JSON document (`defiant-draft/1`,
 * model.md) holding each language's tree as given.
 *
 * @param trees The languages, each at most once, at most
 *   ::GDEFSTOR_MODEL_MAX_LANGUAGES; NULL when @p count is 0. A language the
 *   draft leaves out keeps its tree when the draft is applied; a draft cannot
 *   remove a language.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, ::GDEFSTOR_ERR_LIMIT (the
 *   languages, or more than ::GDEFSTOR_MODEL_MAX_DRAFT_BYTES together), or a
 *   backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_draft_put(GDEFSTOR_Write * write,
    const char * type, size_t type_len, const char * public_id,
    size_t public_id_len, uint64_t base_revision,
    const GDEFSTOR_DraftTree * trees, size_t count);

/** A stored draft. Opaque. */
typedef struct GDEFSTOR_Draft GDEFSTOR_Draft;

/** What a draft holds. Pointers are valid until it is freed. */
typedef struct GDEFSTOR_DraftInfo {
  size_t size;            ///< Set by the caller: `sizeof(GDEFSTOR_DraftInfo)`.
  const char * type;      ///< The entity's type.
  size_t type_len;        ///< Its length.
  uint64_t base_revision; ///< The revision it was made from; 0 for a new entity.
  size_t language_count;  ///< Its languages: gdefstor_draft_tree() reads each.
} GDEFSTOR_DraftInfo;

/**
 * @brief Read the draft of a public id as of the last commit.
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID when there is none.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_draft_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, GDEFSTOR_Draft ** out_draft);

/** @brief Describe a draft. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_draft_info(const GDEFSTOR_Draft * draft,
    GDEFSTOR_DraftInfo * out);

/**
 * @brief Language @p index of a draft (sorted by bytes), and its tree's JSON
 * exactly as it was put. Pointers are borrowed until the draft is freed.
 * @param out Its @ref GDEFSTOR_DraftTree::size is set by the caller.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_draft_tree(const GDEFSTOR_Draft * draft,
    size_t index, GDEFSTOR_DraftTree * out);

/** @brief Release a draft. NULL is ignored. */
GDEFSTOR_API void gdefstor_draft_free(GDEFSTOR_Draft * draft);

/**
 * @brief Delete a draft. Revisions, states and the entity are not touched.
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID when there is none.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_draft_delete(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len);

/**
 * @brief Apply a draft: gdefstor_entity_save_languages() with the draft's
 * trees and base revision, then delete the draft.
 *
 * For a reserved id this first creates the entity, with @p meta's who and when
 * as its creator and creation time. When the head has moved since the draft's
 * base this is ::GDEFSTOR_ERR_CONFLICT, nothing is written and the draft stays.
 * A draft whose tree equals the current one writes no revision and is deleted.
 *
 * @param out_revision As gdefstor_entity_save().
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_draft_apply(GDEFSTOR_Write * write,
    const char * public_id, size_t public_id_len,
    const GDEFSTOR_RevisionMeta * meta, uint64_t * out_revision);

/* ---- the blind index and subjects (AD-14, AD-19) ------------------------- */

/** What gdefstor_blind_history() found. Opaque. */
typedef struct GDEFSTOR_BlindHits GDEFSTOR_BlindHits;

/** One value version holding the MAC. Pointers are valid until the hits are freed. */
typedef struct GDEFSTOR_BlindHit {
  size_t size;              ///< Set by the caller: `sizeof(GDEFSTOR_BlindHit)`.
  uint64_t row;             ///< The entity's internal row.
  const char * public_id;   ///< The entity's public id.
  size_t public_id_len;     ///< Its length.
  uint64_t item;            ///< The item.
  uint64_t valid_from;      ///< The first revision that held this value version.
  /** The revision that superseded it, or 0 while it is current: the version
   * held in revisions valid_from up to (not including) valid_to. */
  uint64_t valid_to;
  const uint8_t * subject;  ///< The value's subject.
  size_t subject_len;       ///< Its length.
  uint32_t key_id;          ///< As written (GDEFSTOR_BlindMac::key_id).
  uint32_t version;         ///< As written (GDEFSTOR_BlindMac::version).
  const char * language;    ///< The language of the tree the version is in.
  size_t language_len;      ///< Its length.
} GDEFSTOR_BlindHit;

/**
 * @brief The history lookup: every entity of @p type with any value version
 * of personal field @p field whose blind row holds @p mac, as of the last
 * commit, with the revisions that held it, in every language (each hit names
 * its own).
 *
 * One hit per value version, ordered by row, item and valid_from. History is
 * searchable while the subject exists: after gdefstor_blind_delete_subject()
 * the subject's versions are not found. A retired field is searched too.
 *
 * @return ::GDEFSTOR_OK (no hit is success), ::GDEFSTOR_ERR_INVALID (a type
 *   never declared, a path that is not a personal scalar field, a MAC that is
 *   not ::GDEFSTOR_BLIND_MAC_BYTES bytes, a NULL argument),
 *   ::GDEFSTOR_ERR_UNSUPPORTED (a backend that has no blind table),
 *   ::GDEFSTOR_ERR_OOM, or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_blind_history(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * type, size_t type_len,
    const char * field, size_t field_len, const uint8_t * mac, size_t mac_len,
    GDEFSTOR_BlindHits ** out_hits);

/** @return The hits. */
GDEFSTOR_API size_t gdefstor_blind_hits_count(const GDEFSTOR_BlindHits * hits);

/** @brief Describe hit @p index. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_blind_hit(const GDEFSTOR_BlindHits * hits,
    size_t index, GDEFSTOR_BlindHit * out);

/** @brief Release hits. NULL is ignored. */
GDEFSTOR_API void gdefstor_blind_hits_free(GDEFSTOR_BlindHits * hits);

/** What gdefstor_subject_lookup() found. Opaque. */
typedef struct GDEFSTOR_SubjectHits GDEFSTOR_SubjectHits;

/** One value version naming the subject. Pointers are valid until freed. */
typedef struct GDEFSTOR_SubjectHit {
  size_t size;              ///< Set by the caller: `sizeof(GDEFSTOR_SubjectHit)`.
  const char * type;        ///< The entity's type.
  size_t type_len;          ///< Its length.
  uint64_t row;             ///< The entity's internal row.
  const char * public_id;   ///< The entity's public id.
  size_t public_id_len;     ///< Its length.
  const char * field;       ///< The personal field's dotted path.
  size_t field_len;         ///< Its length.
  uint64_t item;            ///< The item.
  uint64_t valid_from;      ///< The first revision that held this version.
  uint64_t valid_to;        ///< The revision that superseded it, or 0 (current).
  const char * language;    ///< The language of the tree the version is in.
  size_t language_len;      ///< Its length.
} GDEFSTOR_SubjectHit;

/**
 * @brief The subject lookup, "everything about this person" (AD-19): every
 * stored personal value version that names @p subject, of every type, across
 * history, as of the last commit.
 *
 * It reads the subject recorded beside each ciphertext (an indexed lookup on
 * the subject columns), never the blind table, so it still finds every value
 * after gdefstor_blind_delete_subject(). Retired fields are included; drafts,
 * which are JSON outside the revision tables, are not. Ordered by type, row,
 * field (declaration order), item and valid_from.
 *
 * @return ::GDEFSTOR_OK (no hit is success), ::GDEFSTOR_ERR_INVALID (a subject
 *   that is not a public id, a NULL argument), ::GDEFSTOR_ERR_UNSUPPORTED,
 *   ::GDEFSTOR_ERR_CORRUPT (a stored value no declared field holds),
 *   ::GDEFSTOR_ERR_OOM, or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_subject_lookup(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, GDEFSTOR_SubjectHits ** out_hits);

/** @return The hits. */
GDEFSTOR_API size_t gdefstor_subject_hits_count(
    const GDEFSTOR_SubjectHits * hits);

/** @brief Describe hit @p index. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_subject_hit(
    const GDEFSTOR_SubjectHits * hits, size_t index, GDEFSTOR_SubjectHit * out);

/** @brief Release hits. NULL is ignored. */
GDEFSTOR_API void gdefstor_subject_hits_free(GDEFSTOR_SubjectHits * hits);

/**
 * @brief Delete every blind row of @p subject, of every type, in this write:
 * the data-store step of erasure (AD-19), after the key store has destroyed
 * the subject's key and appended the ledger.
 *
 * Idempotent: a subject with no blind row deletes nothing and succeeds, so
 * the ledger replay may run it again. Afterwards blind equality and the
 * history lookup find nothing of the subject's; the ciphertext rows, and so
 * gdefstor_subject_lookup(), are unchanged.
 *
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (a subject that is not a
 *   public id, a write that is not usable), ::GDEFSTOR_ERR_UNSUPPORTED, or a
 *   backend's error (which breaks the write).
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_blind_delete_subject(
    GDEFSTOR_Write * write, const char * subject, size_t subject_len);

/**
 * @brief Put back the blind row of one stored personal value version, in
 * this write: the blind index rebuilt from decryptable values (AD-14,
 * CAP-S1), which only the caller can do because it needs the keys.
 *
 * Blind rows are not in a snapshot (AD-23), so a restored tenant has none,
 * and a save writes them only for the versions it writes. This writes the
 * row of the version that already exists: entity @p public_id of @p type,
 * personal scalar @p field (a dotted path, retired or not), item @p item,
 * the version that first held in revision @p valid_from, in @p language (as
 * gdefstor_subject_lookup() reports each version). The row takes the
 * subject the version records; the MAC is the caller's, computed as for
 * gdefstor_tree_set_blind(). A row the version already has is replaced, so
 * putting the same rows again changes nothing. The caller decides which
 * subjects are indexed: a subject whose key is destroyed is never put back
 * (AD-19), and gdefstor_blind_delete_subject() removes this row as any
 * other.
 *
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (a type never declared, a
 *   path that is not a personal scalar field, an entity of another type or
 *   none, no such version or one with no value there, a MAC of another
 *   length or a @ref GDEFSTOR_BlindMac::size that is not this header's, a
 *   language that is not one), ::GDEFSTOR_ERR_UNSUPPORTED (a backend with no
 *   blind table, or one built before this call), or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_blind_put(GDEFSTOR_Write * write,
    const char * type, size_t type_len, const char * public_id,
    size_t public_id_len, const char * field, size_t field_len, uint64_t item,
    uint64_t valid_from, const char * language, size_t language_len,
    const GDEFSTOR_BlindMac * mac);

/* ---- records, counters and the outbox (AD-31, AD-33) ---------------------
 *
 * Three families that live in the entity write beside entities, so that one
 * GDEFSTOR_Write commits an entity save, a record, a counter and an outbox row
 * together or none of them (model.md, "Records, counters and the outbox").
 *
 * Names: a record's namespace, a counter's name and an outbox stream are 1 to
 * ::GDEFSTOR_SYSTEM_MAX_NAME_BYTES bytes of `[A-Za-z0-9_.:-]`; a record's key is
 * 1 to ::GDEFSTOR_RECORD_MAX_KEY_BYTES bytes of the same; an outbox format is
 * 1 to ::GDEFSTOR_OUTBOX_MAX_FORMAT_BYTES bytes of the same or `/` (AD-34
 * names a format `<name>/<version>`). Keys and names compare and sort by
 * bytes. A store whose backend has no such families answers every call below
 * ::GDEFSTOR_ERR_UNSUPPORTED. */

/** Bytes of a record namespace, a counter name or an outbox stream. */
#define GDEFSTOR_SYSTEM_MAX_NAME_BYTES 128
/** Bytes of a record key. */
#define GDEFSTOR_RECORD_MAX_KEY_BYTES 256
/** Bytes of a record's value. */
#define GDEFSTOR_RECORD_MAX_BYTES (1024u * 1024u)
/** Records one gdefstor_record_list() returns at most. */
#define GDEFSTOR_RECORD_MAX_PAGE 1000
/** Bytes of an outbox row's format name. */
#define GDEFSTOR_OUTBOX_MAX_FORMAT_BYTES 128
/** Bytes of an outbox row's payload. */
#define GDEFSTOR_OUTBOX_MAX_PAYLOAD_BYTES (1024u * 1024u)
/** Rows one gdefstor_model_outbox_claim() takes at most. */
#define GDEFSTOR_OUTBOX_MAX_CLAIM 1000
/**
 * Failures after which an outbox row is parked (AD-33): the
 * GDEFSTOR_OUTBOX_PARK_FAILURES th gdefstor_model_outbox_failed() of a row
 * parks it. A parked row is never claimed and never deleted; the admin counts
 * it (gdefstor_model_outbox_parked()).
 */
#define GDEFSTOR_OUTBOX_PARK_FAILURES 5

/**
 * @brief Insert or replace a record: non-revisioned bytes under
 * (@p ns, @p key), in this write.
 *
 * A record is truth, with no history: the newest put is all there is. It
 * holds no personal value (AD-19): a record has no subject, so erasure could
 * not find one there; personal data goes in a personal field of an entity.
 * gdefstor_model_rebuild() never touches records, and a snapshot export
 * includes them (AD-23; the format is the export's).
 *
 * @param bytes The value, copied. May be NULL when @p len is 0: an empty
 *   value, which is a record (gdefstor_record_get() finds it).
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (a name, a key or a write that
 *   is not usable), ::GDEFSTOR_ERR_LIMIT (more than
 *   ::GDEFSTOR_RECORD_MAX_BYTES), ::GDEFSTOR_ERR_UNSUPPORTED, or a backend's
 *   error (which breaks the write).
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_record_put(GDEFSTOR_Write * write,
    const char * ns, size_t ns_len, const char * key, size_t key_len,
    const uint8_t * bytes, size_t len);

/**
 * @brief Delete a record in this write. A record that is not there deletes
 * nothing and succeeds.
 * @param out_deleted Set to 1 when a record was deleted, 0 when there was
 *   none. May be NULL.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_record_delete(GDEFSTOR_Write * write,
    const char * ns, size_t ns_len, const char * key, size_t key_len,
    int * out_deleted);

/**
 * @brief Read a record as of the last commit.
 *
 * A missing record is not an error: it is ::GDEFSTOR_OK with @p out_found 0
 * and nothing allocated, apart from a record whose value is empty (found, 0
 * bytes).
 *
 * @param out_found Receives 1 when the record exists, else 0.
 * @param out_bytes Receives the value when found (free with
 *   gdefstor_bytes_free() and the store's allocator; a non-NULL block even for
 *   an empty value), else NULL.
 * @param out_len Receives its length.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, ::GDEFSTOR_ERR_UNSUPPORTED,
 *   ::GDEFSTOR_ERR_OOM, or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_record_get(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * ns, size_t ns_len,
    const char * key, size_t key_len, int * out_found, uint8_t ** out_bytes,
    size_t * out_len);

/** A page of records. Opaque. */
typedef struct GDEFSTOR_RecordList GDEFSTOR_RecordList;

/**
 * @brief List the records of @p ns whose key starts with @p prefix, as of the
 * last commit, ordered by key (bytes), skipping @p offset and returning at
 * most @p limit.
 *
 * @param prefix May be empty (every key of the namespace); otherwise as a key.
 * @param limit 1 to ::GDEFSTOR_RECORD_MAX_PAGE.
 * @return ::GDEFSTOR_OK (no record is an empty list), ::GDEFSTOR_ERR_INVALID
 *   (a limit of 0, a name or prefix that is not one), ::GDEFSTOR_ERR_LIMIT (a
 *   limit above the page), ::GDEFSTOR_ERR_UNSUPPORTED, ::GDEFSTOR_ERR_OOM,
 *   ::GDEFSTOR_ERR_CORRUPT (a backend that answered out of order or outside
 *   the prefix), or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_record_list(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * ns, size_t ns_len,
    const char * prefix, size_t prefix_len, uint64_t offset, size_t limit,
    GDEFSTOR_RecordList ** out_list);

/** @return Records in the page. */
GDEFSTOR_API size_t gdefstor_record_list_count(const GDEFSTOR_RecordList * list);

/**
 * @brief Record @p index of the page. Borrowed until the list is freed.
 * @param out_key Receives its key (not NUL-terminated by contract).
 * @param out_bytes Receives its value; may be NULL for an empty value.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_record_list_item(
    const GDEFSTOR_RecordList * list, size_t index, const char ** out_key,
    size_t * out_key_len, const uint8_t ** out_bytes, size_t * out_len);

/** @brief Release a page. NULL is ignored. */
GDEFSTOR_API void gdefstor_record_list_free(GDEFSTOR_RecordList * list);

/**
 * @brief Take the next value of a named, monotonic counter, in this write:
 * its value plus one, stored in the same write (the config epoch of AD-31 is
 * one).
 *
 * A counter starts at 0, so the first value taken is 1. It never goes
 * backwards and never hands a value out twice: a value taken in a write that
 * then aborts (gdefstor_write_abort(), a broken write, a failed commit) is
 * skipped, never taken again, while gdefstor_counter_peek() goes on reading the
 * last committed value. So committed values may have gaps.
 *
 * @param out_value Receives the value. Written only on success.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, ::GDEFSTOR_ERR_LIMIT (the
 *   counter is at INT64_MAX), ::GDEFSTOR_ERR_UNSUPPORTED, or a backend's error
 *   (which breaks the write).
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_counter_next(GDEFSTOR_Write * write,
    const char * name, size_t name_len, uint64_t * out_value);

/**
 * @brief A counter's value as of the last commit: the value the newest
 * committed write took, or 0 when none ever has.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_counter_peek(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, const char * name,
    size_t name_len, uint64_t * out_value);

/**
 * @brief Append an outbox row to @p stream in this write (AD-33): work that
 * must happen after the commit, or a write to a secondary store (AD-13).
 *
 * The row gets the stream's next sequence (1, 2, 3 ... per tenant and
 * stream), in the write; it appears when the write commits and never when it
 * aborts. The payload is opaque bytes in @p format: the port never reads it.
 * Payloads carry references, or personal values only as ciphertext (AD-33).
 *
 * @param public_id The row's public id, unique in the tenant (sinks
 *   deduplicate by it). One already used is ::GDEFSTOR_ERR_INVALID.
 * @param out_seq Receives the sequence. May be NULL.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, ::GDEFSTOR_ERR_LIMIT (a payload
 *   over ::GDEFSTOR_OUTBOX_MAX_PAYLOAD_BYTES), ::GDEFSTOR_ERR_UNSUPPORTED, or a
 *   backend's error (which breaks the write).
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_model_outbox_append(
    GDEFSTOR_Write * write, const char * stream, size_t stream_len,
    const char * public_id, size_t public_id_len, const char * format,
    size_t format_len, const uint8_t * payload, size_t payload_len,
    uint64_t * out_seq);

/** Rows a claim took. Opaque. */
typedef struct GDEFSTOR_OutboxClaim GDEFSTOR_OutboxClaim;

/** One claimed row. Pointers are valid until the claim is freed. */
typedef struct GDEFSTOR_OutboxEntry {
  size_t size;              ///< Set by the caller: `sizeof(GDEFSTOR_OutboxEntry)`.
  const char * stream;      ///< Its stream.
  size_t stream_len;        ///< Its length.
  uint64_t seq;             ///< Its sequence in the stream.
  const char * public_id;   ///< Its public id.
  size_t public_id_len;     ///< Its length.
  const char * format;      ///< Its payload's format.
  size_t format_len;        ///< Its length.
  const uint8_t * payload;  ///< The payload; may be NULL when empty.
  size_t payload_len;       ///< Its length.
  uint32_t failures;        ///< gdefstor_model_outbox_failed() calls so far.
  int64_t lease_until_ms;   ///< The claim's lease ends here (the caller's clock).
} GDEFSTOR_OutboxEntry;

/**
 * @brief Claim rows of one stream for delivery, under a lease that ends at
 * @p now_ms + @p lease_ms (the caller's clock).
 *
 * A stream has at most one live claim, which is what keeps delivery in order
 * (AD-33): while any row of the stream that is not parked has a lease that
 * has not ended (`lease_until_ms > now_ms`), a claim takes nothing. Otherwise
 * it takes the stream's lowest-sequence rows that are not parked, in sequence
 * order, at most @p max: rows never claimed, and rows whose lease ended
 * without a delivery (claimable again, so delivery is at least once).
 *
 * The claim runs as a write of its own: it waits for the tenant's open write
 * as gdefstor_write_begin() does (::GDEFSTOR_ERR_BUSY for a caller that is not
 * a fiber), and two stores on one database never hold live leases on one
 * stream at once.
 *
 * @param lease_ms Greater than 0.
 * @param max 1 to ::GDEFSTOR_OUTBOX_MAX_CLAIM.
 * @param out_claim Receives the rows, perhaps none.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (an argument, a lease end past
 *   INT64_MAX), ::GDEFSTOR_ERR_LIMIT (a max above the claim),
 *   ::GDEFSTOR_ERR_UNSUPPORTED, ::GDEFSTOR_ERR_BUSY, ::GDEFSTOR_ERR_OOM,
 *   ::GDEFSTOR_ERR_CORRUPT (a backend that answered out of order), or a
 *   backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_model_outbox_claim(
    GDEFSTOR_Store * store, const char * tenant, size_t tenant_len,
    const char * stream, size_t stream_len, int64_t lease_ms, int64_t now_ms,
    size_t max, GDEFSTOR_OutboxClaim ** out_claim);

/** @return Rows in the claim. */
GDEFSTOR_API size_t gdefstor_model_outbox_claim_count(
    const GDEFSTOR_OutboxClaim * claim);

/** @brief Describe row @p index of the claim (0 is the lowest sequence). */
GDEFSTOR_API GDEFSTOR_Result gdefstor_model_outbox_claim_entry(
    const GDEFSTOR_OutboxClaim * claim, size_t index,
    GDEFSTOR_OutboxEntry * out);

/** @brief Release a claim. NULL is ignored. */
GDEFSTOR_API void gdefstor_model_outbox_claim_free(GDEFSTOR_OutboxClaim * claim);

/**
 * @brief Delete delivered rows, by public id, in a write of its own.
 *
 * A row that is not there (delivered already: delivery is at least once) is
 * skipped. A parked row is never deleted here, nor anywhere: it stays until
 * an operator acts on it.
 *
 * @param out_deleted Receives how many rows were deleted. May be NULL.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (an id that is not a public id,
 *   a NULL array with @p count above 0), ::GDEFSTOR_ERR_UNSUPPORTED,
 *   ::GDEFSTOR_ERR_BUSY, or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_model_outbox_delivered(
    GDEFSTOR_Store * store, const char * tenant, size_t tenant_len,
    const char * const * public_ids, const size_t * public_id_lens,
    size_t count, size_t * out_deleted);

/**
 * @brief Count one failed delivery of a row, in a write of its own.
 *
 * The row's lease ends at @p now_ms, so it is claimable again from then on (a
 * runner that fails a row leaves the rest of its claim too: a later row
 * delivered after an earlier one failed is delivered out of order). The
 * ::GDEFSTOR_OUTBOX_PARK_FAILURES th failure parks the row instead: it is
 * never claimed again and never deleted, and gdefstor_model_outbox_parked()
 * counts it. A parked row's failure changes nothing.
 *
 * @param out_parked Set to 1 when the row is parked now, else 0. May be NULL.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (no such row, or an argument),
 *   ::GDEFSTOR_ERR_UNSUPPORTED, ::GDEFSTOR_ERR_BUSY, or a backend's error.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_model_outbox_failed(
    GDEFSTOR_Store * store, const char * tenant, size_t tenant_len,
    const char * public_id, size_t public_id_len, int64_t now_ms,
    int * out_parked);

/**
 * @brief How many rows of @p stream are parked, as of the last commit; with
 * @p stream_len 0, of every stream of the tenant.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_model_outbox_parked(
    GDEFSTOR_Store * store, const char * tenant, size_t tenant_len,
    const char * stream, size_t stream_len, uint64_t * out_count);

/* ---- derived data --------------------------------------------------------- */

/**
 * @brief Drop every current table (and every other derived lookup) of the
 * write's tenant and rebuild it from the truth, in this write.
 *
 * The blind table is not derived and is left alone: its rows are truth for as
 * long as their subject exists. So are records, counters and outbox rows,
 * which are truth and are never touched.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_model_rebuild(GDEFSTOR_Write * write);

/** gdefstor_model_dump(): the truth (entities, states, revisions, every item
 * version with its language, declarations, drafts, blind rows, records,
 * counters, outbox rows). */
#define GDEFSTOR_MODEL_TRUTH 1
/** gdefstor_model_dump(): the derived data (current items, current state). */
#define GDEFSTOR_MODEL_CURRENT 2

/**
 * @brief A canonical text of one tenant's stored rows, for checks and
 * diagnostics: one line per row, sorted, every value spelled exactly.
 *
 * Two stores hold the same rows exactly when their dumps are equal. The
 * format (model.md) is for tests and tools, not for export (that is AD-23's).
 * Free the bytes with gdefstor_bytes_free() and the store's allocator.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_model_dump(GDEFSTOR_Store * store,
    const char * tenant, size_t tenant_len, int which, uint8_t ** out_text,
    size_t * out_len);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_MODEL_H */
