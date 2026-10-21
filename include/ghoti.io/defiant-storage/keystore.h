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
 * @file keystore.h
 * @brief The key store port: subject keys and the erasure ledger (AD-19).
 * @stability free
 *
 * A port of its own, never part of the data store (AD-13): its backends may be
 * a separate database or an external key service. A subject's key is made
 * inside the key store and never leaves it: the caller asks the store to
 * wrap (seal) and unwrap (open) bytes under it, typically a data key. Erasing
 * a subject destroys its key and appends its ledger row in one step of the
 * key store, after which every unwrap for it reports the subject erased; that
 * cannot be undone. The ledger (subject and time, nothing personal) is
 * append-only and never pruned.
 *
 * Ordinary saves never touch the key store; creating and erasing a subject do.
 * Nothing here is wired into the entity port: that is a later story's.
 *
 * Besides the keys it makes, a key store holds what a caller seals itself and
 * gives it to keep (gdefstor_keystore_subject_put(): a subject's key already
 * wrapped under the caller's keyring, which the store never opens), and
 * tenant secrets written once (gdefstor_keystore_secret_put(): the wrap of a
 * tenant's keyring). Sealed material is a subject's key like any other: it
 * goes in the same step as the ledger row when the subject is erased, and a
 * subject erased can never be given it again.
 *
 * **The key set.** gdefstor_keystore_export() writes everything a key store
 * holds for one tenant (secrets, keys, sealed material, the ledger) and when
 * it was taken, as `defiant-keyset/1`; gdefstor_keystore_import() merges one
 * back. It is the key half of a backup pair (AD-19, AD-23): the data half is a
 * snapshot taken with gdefstor_snapshot_export_at(), and a restore refuses a
 * key set older than the data set it pairs with. A key set holds keys; it is
 * never inside a snapshot.
 *
 * ```
 * defiant-keyset/1
 * taken <milliseconds since the Unix epoch>
 * secret <name> <hex>
 * key <subject> <hex>
 * sealed <subject> <hex>
 * ledger <subject> <milliseconds>
 * ```
 *
 * One row per line, each line ending in a newline, rows in that order (the
 * ledger oldest first); names are public ids as for a subject. An import never
 * brings a key back for a subject the ledger names, whether the live ledger
 * or the set's, and replays the set's ledger rows as erasures.
 */

#ifndef GHOTI_IO_GDEFSTOR_KEYSTORE_H
#define GHOTI_IO_GDEFSTOR_KEYSTORE_H

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/core.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes one wrap or unwrap takes. */
#define GDEFSTOR_KEYSTORE_MAX_BYTES (64u * 1024u)

/** Bytes of one piece of sealed material or one tenant secret, at most. */
#define GDEFSTOR_KEYSTORE_MAX_MATERIAL_BYTES 1024u

/** The kind of a row of a key set (gdefstor_keystore_export()). */
typedef enum GDEFSTOR_KeyRowKind {
  GDEFSTOR_KEYROW_SECRET = 1, ///< A tenant secret: its name and bytes.
  GDEFSTOR_KEYROW_KEY,        ///< A key the store made: the subject and it.
  GDEFSTOR_KEYROW_SEALED,     ///< Sealed material: the subject and its bytes.
  GDEFSTOR_KEYROW_LEDGER      ///< An erasure: the subject and @c when_ms.
} GDEFSTOR_KeyRowKind;

/** One row of a key set. Every pointer is borrowed. */
typedef struct GDEFSTOR_KeyRow {
  int kind;              ///< A ::GDEFSTOR_KeyRowKind.
  const char * name;     ///< The secret's name or the subject.
  size_t name_len;       ///< Its length.
  const uint8_t * bytes; ///< The bytes; NULL for a ledger row.
  size_t len;            ///< Their length; 0 for a ledger row.
  int64_t when_ms;       ///< A ledger row's time; 0 otherwise.
} GDEFSTOR_KeyRow;

/**
 * A key store backend. A backend zeroes it, sets @ref size to
 * `sizeof(GDEFSTOR_KeyStoreBackend)` and fills every member. Each call is
 * atomic on its own. Output bytes come from the allocator the key store was
 * created with.
 */
typedef struct GDEFSTOR_KeyStoreBackend {
  size_t size; ///< `sizeof(GDEFSTOR_KeyStoreBackend)`.
  void * ctx;  ///< Passed to every function.
  void (*destroy)(void * ctx);
  /** Make a key for a subject. ::GDEFSTOR_ERR_INVALID when it has one or was erased. */
  GDEFSTOR_Result (*create)(void * ctx, const char * tenant, size_t tenant_len,
      const char * subject, size_t subject_len);
  /** Seal bytes under the subject's key with a fresh nonce. Sets @p out_erased
   * and writes no bytes for an erased subject; ::GDEFSTOR_ERR_INVALID for one
   * with no key. */
  GDEFSTOR_Result (*wrap)(void * ctx, const char * tenant, size_t tenant_len,
      const char * subject, size_t subject_len, const uint8_t * bytes,
      size_t len, int * out_erased, uint8_t ** out_bytes, size_t * out_len);
  /** Open what wrap sealed. As wrap for an erased or unknown subject;
   * ::GDEFSTOR_ERR_CORRUPT for bytes that do not open. */
  GDEFSTOR_Result (*unwrap)(void * ctx, const char * tenant, size_t tenant_len,
      const char * subject, size_t subject_len, const uint8_t * bytes,
      size_t len, int * out_erased, uint8_t ** out_bytes, size_t * out_len);
  /** Destroy the subject's key, if any, and append its ledger row, if it has
   * none, as one step. Idempotent. */
  GDEFSTOR_Result (*erase)(void * ctx, const char * tenant, size_t tenant_len,
      const char * subject, size_t subject_len, int64_t when_ms);
  /** Visit every ledger row of a tenant, oldest first. */
  GDEFSTOR_Result (*ledger)(void * ctx, const char * tenant, size_t tenant_len,
      GDEFSTOR_Result (*visit)(void * vctx, const char * subject,
          size_t subject_len, int64_t when_ms),
      void * vctx);
  /* Everything below is optional, all of it or none: a backend that leaves
   * them NULL answers ::GDEFSTOR_ERR_UNSUPPORTED to every call that needs
   * them. */
  /** Keep caller-sealed material as the subject's key. ::GDEFSTOR_ERR_INVALID
   * when the subject has a key or material, or was erased. */
  GDEFSTOR_Result (*sealed_put)(void * ctx, const char * tenant,
      size_t tenant_len, const char * subject, size_t subject_len,
      const uint8_t * bytes, size_t len);
  /** The material sealed_put kept. Sets @p out_erased and writes no bytes for
   * an erased subject; ::GDEFSTOR_ERR_INVALID for one without material (a key
   * the store made is never returned). */
  GDEFSTOR_Result (*sealed_get)(void * ctx, const char * tenant,
      size_t tenant_len, const char * subject, size_t subject_len,
      int * out_erased, uint8_t ** out_bytes, size_t * out_len);
  /** Write a tenant secret once: the same bytes again change nothing, other
   * bytes are ::GDEFSTOR_ERR_INVALID. */
  GDEFSTOR_Result (*secret_put)(void * ctx, const char * tenant,
      size_t tenant_len, const char * name, size_t name_len,
      const uint8_t * bytes, size_t len);
  /** A tenant secret, or @p out_found 0. */
  GDEFSTOR_Result (*secret_get)(void * ctx, const char * tenant,
      size_t tenant_len, const char * name, size_t name_len, int * out_found,
      uint8_t ** out_bytes, size_t * out_len);
  /** Visit every row the tenant has, in key set order (secrets, keys, sealed
   * material, the ledger oldest first), from one read. */
  GDEFSTOR_Result (*dump)(void * ctx, const char * tenant, size_t tenant_len,
      GDEFSTOR_Result (*visit)(void * vctx, const GDEFSTOR_KeyRow * row),
      void * vctx);
  /** Merge rows into the tenant as one step, all of them or none: each ledger
   * row as an erasure (unless the subject is already erased); a secret that is
   * missing is written, an equal one is kept and a different one is
   * ::GDEFSTOR_ERR_INVALID; a key or sealed material for a subject either
   * ledger names is dropped, an equal one is kept, one the subject has
   * already in another form or with other bytes is ::GDEFSTOR_ERR_INVALID,
   * and a missing one is written. The rows are checked (gdefstor_keyset_parse()
   * rules) before the backend is called. */
  GDEFSTOR_Result (*restore)(void * ctx, const char * tenant,
      size_t tenant_len, const GDEFSTOR_KeyRow * rows, size_t count);
} GDEFSTOR_KeyStoreBackend;

/** An open key store. Opaque. */
typedef struct GDEFSTOR_KeyStore GDEFSTOR_KeyStore;

/**
 * @brief Open a key store over a backend. The backend is copied; its `ctx` is
 * borrowed until gdefstor_keystore_destroy(), which destroys it.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_create(
    const GDEFSTOR_KeyStoreBackend * backend,
    const GDEFSTOR_Allocator * allocator, GDEFSTOR_KeyStore ** out_keystore);

/** @brief Close a key store and destroy its backend. NULL is ignored. */
GDEFSTOR_API void gdefstor_keystore_destroy(GDEFSTOR_KeyStore * keystore);

/**
 * @brief Make a subject's key.
 * @return ::GDEFSTOR_OK, or ::GDEFSTOR_ERR_INVALID when the subject has a key,
 *   was erased, or is not a public id.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_subject_create(
    GDEFSTOR_KeyStore * keystore, const char * tenant, size_t tenant_len,
    const char * subject, size_t subject_len);

/**
 * @brief Seal @p bytes under the subject's key. Writes nothing to the store.
 *
 * @param out_erased Set to 1, and no bytes written, when the subject is erased.
 * @param out_bytes Free with gdefstor_bytes_free() and the key store's allocator.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_wrap(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, const uint8_t * bytes, size_t len, int * out_erased,
    uint8_t ** out_bytes, size_t * out_len);

/** @brief Open what gdefstor_keystore_wrap() sealed. As it for erased subjects. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_unwrap(
    GDEFSTOR_KeyStore * keystore, const char * tenant, size_t tenant_len,
    const char * subject, size_t subject_len, const uint8_t * bytes, size_t len,
    int * out_erased, uint8_t ** out_bytes, size_t * out_len);

/**
 * @brief Erase a subject: destroy its key and append the ledger row, one step.
 *
 * Erasing an erased subject changes nothing. Erasing a subject that never had
 * a key still records it, so that one restored from elsewhere stays erased.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_erase(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, const char * subject,
    size_t subject_len, int64_t when_ms);

/**
 * @brief Record an erasure learned from elsewhere (a snapshot's copy of the
 * ledger, replayed on restore): the same step as gdefstor_keystore_erase().
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_ledger_append(
    GDEFSTOR_KeyStore * keystore, const char * tenant, size_t tenant_len,
    const char * subject, size_t subject_len, int64_t when_ms);

/** A read of the ledger. Opaque. */
typedef struct GDEFSTOR_Ledger GDEFSTOR_Ledger;

/** @brief Read a tenant's ledger, oldest first. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_ledger(GDEFSTOR_KeyStore * keystore,
    const char * tenant, size_t tenant_len, GDEFSTOR_Ledger ** out_ledger);

/** @return Rows in the ledger. */
GDEFSTOR_API size_t gdefstor_ledger_count(const GDEFSTOR_Ledger * ledger);

/** @brief Row @p index: the subject (borrowed until freed) and when. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_ledger_row(const GDEFSTOR_Ledger * ledger,
    size_t index, const char ** out_subject, size_t * out_subject_len,
    int64_t * out_when_ms);

/** @brief Release a ledger read. NULL is ignored. */
GDEFSTOR_API void gdefstor_ledger_free(GDEFSTOR_Ledger * ledger);

/**
 * @brief Keep @p bytes, sealed by the caller, as the subject's key.
 *
 * The store never opens them: a caller keeps here a subject key it wrapped
 * under its own keyring, and reads it back with
 * gdefstor_keystore_subject_sealed(). Erasing the subject destroys them in the
 * same step as its ledger row. A subject has either a key the store made
 * (gdefstor_keystore_subject_create()) or sealed material, never both.
 *
 * @param len 1 to ::GDEFSTOR_KEYSTORE_MAX_MATERIAL_BYTES.
 * @return ::GDEFSTOR_OK; ::GDEFSTOR_ERR_INVALID when the subject has a key or
 *   material, was erased, or is not a public id; ::GDEFSTOR_ERR_LIMIT for
 *   @p len over the bound; ::GDEFSTOR_ERR_UNSUPPORTED on a backend without it.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_subject_put(
    GDEFSTOR_KeyStore * keystore, const char * tenant, size_t tenant_len,
    const char * subject, size_t subject_len, const uint8_t * bytes,
    size_t len);

/**
 * @brief Read the material gdefstor_keystore_subject_put() kept. Writes
 * nothing to the store.
 *
 * @param out_erased Set to 1, and no bytes written, when the subject is erased
 *   (the ledger names it, whether it ever had material here or not).
 * @param out_bytes Free with gdefstor_bytes_free() and the key store's
 *   allocator.
 * @return ::GDEFSTOR_OK; ::GDEFSTOR_ERR_INVALID for a subject with no
 *   material that is not erased (one with a key the store made included: that
 *   key never leaves); ::GDEFSTOR_ERR_UNSUPPORTED on a backend without it.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_subject_sealed(
    GDEFSTOR_KeyStore * keystore, const char * tenant, size_t tenant_len,
    const char * subject, size_t subject_len, int * out_erased,
    uint8_t ** out_bytes, size_t * out_len);

/**
 * @brief Write a tenant secret (a keyring's wrap) once.
 *
 * @param name A public id, as for a subject.
 * @param len 1 to ::GDEFSTOR_KEYSTORE_MAX_MATERIAL_BYTES.
 * @return ::GDEFSTOR_OK, also when the secret already holds exactly these
 *   bytes (nothing is written then); ::GDEFSTOR_ERR_INVALID when it holds
 *   other bytes or an argument is wrong; ::GDEFSTOR_ERR_LIMIT;
 *   ::GDEFSTOR_ERR_UNSUPPORTED.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_secret_put(
    GDEFSTOR_KeyStore * keystore, const char * tenant, size_t tenant_len,
    const char * name, size_t name_len, const uint8_t * bytes, size_t len);

/**
 * @brief Read a tenant secret.
 *
 * @param out_found 0, and no bytes written, when there is none.
 * @param out_bytes Free with gdefstor_bytes_free() and the key store's
 *   allocator.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_secret_get(
    GDEFSTOR_KeyStore * keystore, const char * tenant, size_t tenant_len,
    const char * name, size_t name_len, int * out_found, uint8_t ** out_bytes,
    size_t * out_len);

/**
 * @brief Export a tenant's key set (`defiant-keyset/1`, above), recording
 * @p taken_ms as when it was taken. Writes nothing to the store.
 *
 * @param taken_ms Milliseconds since the Unix epoch; not negative.
 * @param out_bytes Free with gdefstor_bytes_free() and the key store's
 *   allocator. They hold keys: they belong in a key backup, never in a
 *   snapshot.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID, ::GDEFSTOR_ERR_UNSUPPORTED,
 *   or the backend's failure.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_export(
    GDEFSTOR_KeyStore * keystore, const char * tenant, size_t tenant_len,
    int64_t taken_ms, uint8_t ** out_bytes, size_t * out_len);

/**
 * @brief Merge a key set into a tenant, as one step (the backend's restore,
 * above): its ledger is replayed, and no key comes back for an erased subject.
 *
 * The set's tenant is not recorded in it; @p tenant is where it goes.
 *
 * @return ::GDEFSTOR_OK; ::GDEFSTOR_ERR_FORMAT for bytes that are not a key
 *   set, ::GDEFSTOR_ERR_UNSUPPORTED for a newer version or a backend without
 *   it, ::GDEFSTOR_ERR_CORRUPT for a malformed one (nothing is written then);
 *   ::GDEFSTOR_ERR_INVALID for a secret or a key the tenant already holds
 *   with other bytes (nothing is written then).
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keystore_import(
    GDEFSTOR_KeyStore * keystore, const char * tenant, size_t tenant_len,
    const uint8_t * bytes, size_t len);

/** A parsed key set. Opaque. */
typedef struct GDEFSTOR_KeySet GDEFSTOR_KeySet;

/**
 * @brief Parse a key set, checking every row: the header, the time, the
 * kinds in order, public-id names, hex of 1 to
 * ::GDEFSTOR_KEYSTORE_MAX_MATERIAL_BYTES bytes, no name twice in one kind, and
 * no subject with both a key and sealed material.
 *
 * @param allocator NULL uses gdefstor_allocator_default().
 * @return As gdefstor_keystore_import() for the bytes, or ::GDEFSTOR_ERR_OOM.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keyset_parse(
    const GDEFSTOR_Allocator * allocator, const uint8_t * bytes, size_t len,
    GDEFSTOR_KeySet ** out_set);

/** @return When the set was taken, in milliseconds since the Unix epoch. */
GDEFSTOR_API int64_t gdefstor_keyset_taken(const GDEFSTOR_KeySet * set);

/** @return The rows in the set. */
GDEFSTOR_API size_t gdefstor_keyset_count(const GDEFSTOR_KeySet * set);

/** @brief Row @p index; its pointers are borrowed until the set is freed. */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keyset_row(const GDEFSTOR_KeySet * set,
    size_t index, GDEFSTOR_KeyRow * out_row);

/** @brief Release a parsed set, wiping its bytes. NULL is ignored. */
GDEFSTOR_API void gdefstor_keyset_free(GDEFSTOR_KeySet * set);

/**
 * @brief Write rows as a key set taken at @p taken_ms. The rows are checked
 * as gdefstor_keyset_parse() checks them, and must already be in key set
 * order.
 *
 * @param out_bytes Free with gdefstor_bytes_free() and @p allocator.
 */
GDEFSTOR_API GDEFSTOR_Result gdefstor_keyset_encode(
    const GDEFSTOR_Allocator * allocator, int64_t taken_ms,
    const GDEFSTOR_KeyRow * rows, size_t count, uint8_t ** out_bytes,
    size_t * out_len);

/**
 * The cipher a key store that seals in process is given (the memory key
 * store is one): randomness and an AEAD, from the caller's security library,
 * so this port carries no cryptography of its own.
 */
typedef struct GDEFSTOR_KeyCipher {
  size_t size;      ///< `sizeof(GDEFSTOR_KeyCipher)`.
  void * ctx;       ///< Passed to every function.
  size_t key_bytes; ///< The length of a key.
  /** Fill @p out with @p n random bytes. */
  GDEFSTOR_Result (*random)(void * ctx, uint8_t * out, size_t n);
  /** Seal @p n bytes under @p key with a fresh nonce; the output (nonce,
   * ciphertext, tag) comes from @p alloc. */
  GDEFSTOR_Result (*seal)(void * ctx, const GDEFSTOR_Allocator * alloc,
      const uint8_t * key, const uint8_t * in, size_t n, uint8_t ** out,
      size_t * out_len);
  /** Open what seal made; ::GDEFSTOR_ERR_CORRUPT when it does not open. */
  GDEFSTOR_Result (*open)(void * ctx, const GDEFSTOR_Allocator * alloc,
      const uint8_t * key, const uint8_t * in, size_t n, uint8_t ** out,
      size_t * out_len);
} GDEFSTOR_KeyCipher;

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_KEYSTORE_H */
