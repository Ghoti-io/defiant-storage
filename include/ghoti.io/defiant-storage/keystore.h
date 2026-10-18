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
