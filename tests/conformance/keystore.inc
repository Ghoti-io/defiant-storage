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
 * The key store port's conformance suite (keystore.h). Installed beside the
 * port; the including file defines
 *
 *     GDEFSTOR_Result KeyStoreOpen(const GDEFSTOR_KeyCipher * cipher,
 *         GDEFSTOR_KeyStore ** out);
 *
 * and includes this file after gtest. A backend that seals in process uses
 * the cipher it is given; one that does not (an external key service) may
 * ignore it.
 */

#include <cstdint>
#include <cstring>
#include <string>

namespace keystore_conformance {

/* NOT A CIPHER. A keyed checksum and a keyed XOR, so the suite can tell
 * sealed from open, a wrong key from a right one, and one nonce from the
 * next, without the port's tests depending on a security library. */
struct ToyCipher {
  uint64_t counter = 0;
};

extern "C" {
static GDEFSTOR_Result toy_random(void * ctx, uint8_t * out, size_t n) {
  ToyCipher * c = static_cast<ToyCipher *>(ctx);
  for (size_t i = 0; i < n; i++) {
    c->counter = c->counter * 6364136223846793005ull + 1442695040888963407ull;
    out[i] = static_cast<uint8_t>(c->counter >> 56);
  }
  return GDEFSTOR_OK;
}

static uint8_t toy_stream(const uint8_t * key, const uint8_t * nonce,
    size_t i) {
  return static_cast<uint8_t>(key[i % 16] ^ nonce[i % 8] ^ (i * 131u));
}

static uint8_t toy_tag(const uint8_t * key, const uint8_t * nonce,
    const uint8_t * plain, size_t n) {
  uint8_t t = 0x5a;
  for (size_t i = 0; i < 16; i++) {
    t = static_cast<uint8_t>(t * 33u + key[i]);
  }
  for (size_t i = 0; i < 8; i++) {
    t = static_cast<uint8_t>(t * 33u + nonce[i]);
  }
  for (size_t i = 0; i < n; i++) {
    t = static_cast<uint8_t>(t * 33u + plain[i]);
  }
  return t;
}

static GDEFSTOR_Result toy_seal(void * ctx, const GDEFSTOR_Allocator * alloc,
    const uint8_t * key, const uint8_t * in, size_t n, uint8_t ** out,
    size_t * out_len) {
  uint8_t * o = static_cast<uint8_t *>(gcu_allocator_malloc(alloc, n + 9));
  if (o == nullptr) {
    return GDEFSTOR_ERR_OOM;
  }
  toy_random(ctx, o, 8);
  for (size_t i = 0; i < n; i++) {
    o[8 + i] = static_cast<uint8_t>(in[i] ^ toy_stream(key, o, i));
  }
  o[8 + n] = toy_tag(key, o, in, n);
  *out = o;
  *out_len = n + 9;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result toy_open(void * ctx, const GDEFSTOR_Allocator * alloc,
    const uint8_t * key, const uint8_t * in, size_t n, uint8_t ** out,
    size_t * out_len) {
  (void)ctx;
  if (n < 9) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  uint8_t * o = static_cast<uint8_t *>(gcu_allocator_malloc(alloc, n - 9 + 1));
  if (o == nullptr) {
    return GDEFSTOR_ERR_OOM;
  }
  for (size_t i = 0; i < n - 9; i++) {
    o[i] = static_cast<uint8_t>(in[8 + i] ^ toy_stream(key, in, i));
  }
  if (toy_tag(key, in, o, n - 9) != in[n - 1]) {
    gcu_allocator_free(alloc, o);
    return GDEFSTOR_ERR_CORRUPT;
  }
  *out = o;
  *out_len = n - 9;
  return GDEFSTOR_OK;
}
}

class KeyStore : public ::testing::Test {
 protected:
  ToyCipher toy;
  GDEFSTOR_KeyCipher cipher;
  GDEFSTOR_KeyStore * ks = nullptr;

  void SetUp() override {
    std::memset(&cipher, 0, sizeof cipher);
    cipher.size = sizeof cipher;
    cipher.ctx = &toy;
    cipher.key_bytes = 16;
    cipher.random = toy_random;
    cipher.seal = toy_seal;
    cipher.open = toy_open;
    ASSERT_EQ(GDEFSTOR_OK, KeyStoreOpen(&cipher, &ks));
  }
  void TearDown() override {
    gdefstor_keystore_destroy(ks);
  }

  std::string Wrap(const char * subject, const std::string & plain,
      GDEFSTOR_Result * rc_out = nullptr, int * erased_out = nullptr) {
    uint8_t * out = nullptr;
    size_t len = 0;
    int erased = 0;
    GDEFSTOR_Result rc = gdefstor_keystore_wrap(ks, "t1", 2, subject,
        std::strlen(subject), reinterpret_cast<const uint8_t *>(plain.data()),
        plain.size(), &erased, &out, &len);
    if (rc_out != nullptr) {
      *rc_out = rc;
    }
    if (erased_out != nullptr) {
      *erased_out = erased;
    }
    std::string s(reinterpret_cast<const char *>(out), out == nullptr ? 0 : len);
    gdefstor_bytes_free(nullptr, out);
    return s;
  }

  std::string Unwrap(const char * subject, const std::string & sealed,
      GDEFSTOR_Result * rc_out, int * erased_out) {
    uint8_t * out = nullptr;
    size_t len = 0;
    int erased = 0;
    *rc_out = gdefstor_keystore_unwrap(ks, "t1", 2, subject,
        std::strlen(subject), reinterpret_cast<const uint8_t *>(sealed.data()),
        sealed.size(), &erased, &out, &len);
    *erased_out = erased;
    std::string s(reinterpret_cast<const char *>(out), out == nullptr ? 0 : len);
    gdefstor_bytes_free(nullptr, out);
    return s;
  }
};

TEST_F(KeyStore, WrapOpensOnlyUnderItsSubject) {
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "p1", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "p2", 2));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_create(ks, "t1", 2,
      "p1", 2));
  std::string a = Wrap("p1", "data key");
  std::string b = Wrap("p1", "data key");
  EXPECT_NE(a, b) << "a fresh nonce each time";
  EXPECT_EQ(std::string::npos, a.find("data key"));
  GDEFSTOR_Result rc;
  int erased = 0;
  EXPECT_EQ("data key", Unwrap("p1", a, &rc, &erased));
  EXPECT_EQ(GDEFSTOR_OK, rc);
  EXPECT_EQ(0, erased);
  Unwrap("p2", a, &rc, &erased);
  EXPECT_EQ(GDEFSTOR_ERR_CORRUPT, rc);
  Unwrap("p9", a, &rc, &erased);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, rc);
  /* The tenant is part of the subject's name. */
  uint8_t * out = nullptr;
  size_t len = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_unwrap(ks, "t2", 2, "p1", 2,
      reinterpret_cast<const uint8_t *>(a.data()), a.size(), &erased, &out,
      &len));
}

TEST_F(KeyStore, ErasureIsFinalAndLedgered) {
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "p1", 2));
  std::string a = Wrap("p1", "secret");
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "p1", 2, 77));
  GDEFSTOR_Result rc;
  int erased = 0;
  EXPECT_EQ("", Unwrap("p1", a, &rc, &erased));
  EXPECT_EQ(GDEFSTOR_OK, rc);
  EXPECT_EQ(1, erased);
  Wrap("p1", "more", &rc, &erased);
  EXPECT_EQ(GDEFSTOR_OK, rc);
  EXPECT_EQ(1, erased);
  /* No new key for an erased subject, and a second erasure adds nothing. */
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_create(ks, "t1", 2,
      "p1", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "p1", 2, 99));
  GDEFSTOR_Ledger * l = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_ledger(ks, "t1", 2, &l));
  ASSERT_EQ(1u, gdefstor_ledger_count(l));
  const char * s = nullptr;
  size_t sl = 0;
  int64_t when = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_ledger_row(l, 0, &s, &sl, &when));
  EXPECT_EQ("p1", std::string(s, sl));
  EXPECT_EQ(77, when);
  gdefstor_ledger_free(l);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_ledger(ks, "t2", 2, &l));
  EXPECT_EQ(0u, gdefstor_ledger_count(l));
  gdefstor_ledger_free(l);
}

TEST_F(KeyStore, ALedgerReplayErasesToo) {
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "p1", 2));
  std::string a = Wrap("p1", "secret");
  /* Restored from a snapshot whose ledger says p1 and p3 were erased. */
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_ledger_append(ks, "t1", 2, "p1", 2, 5));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_ledger_append(ks, "t1", 2, "p3", 2, 6));
  GDEFSTOR_Result rc;
  int erased = 0;
  Unwrap("p1", a, &rc, &erased);
  EXPECT_EQ(1, erased);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_create(ks, "t1", 2,
      "p3", 2));
  GDEFSTOR_Ledger * l = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_ledger(ks, "t1", 2, &l));
  EXPECT_EQ(2u, gdefstor_ledger_count(l));
  gdefstor_ledger_free(l);
}

TEST_F(KeyStore, ArgumentsAreChecked) {
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_create(ks, "t1", 2,
      "bad id", 6));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_create(ks, "", 0,
      "p1", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "p1", 2));
  std::string big(GDEFSTOR_KEYSTORE_MAX_BYTES + 1, 'x');
  GDEFSTOR_Result rc;
  Wrap("p1", big, &rc);
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, rc);
}

/* ---- sealed material, tenant secrets and the key set ---------------------- */

static std::string Str(const uint8_t * p, size_t n) {
  return std::string(reinterpret_cast<const char *>(p), p == nullptr ? 0 : n);
}

static const uint8_t * U(const std::string & s) {
  return reinterpret_cast<const uint8_t *>(s.data());
}

class KeyStoreSets : public KeyStore {
 protected:
  std::string Sealed(GDEFSTOR_KeyStore * k, const char * subject,
      GDEFSTOR_Result * rc_out, int * erased_out) {
    uint8_t * out = nullptr;
    size_t len = 0;
    int erased = 0;
    *rc_out = gdefstor_keystore_subject_sealed(k, "t1", 2, subject,
        std::strlen(subject), &erased, &out, &len);
    *erased_out = erased;
    std::string s = Str(out, len);
    gdefstor_bytes_free(nullptr, out);
    return s;
  }

  std::string Secret(GDEFSTOR_KeyStore * k, const char * name, int * found) {
    uint8_t * out = nullptr;
    size_t len = 0;
    *found = -1;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_keystore_secret_get(k, "t1", 2, name,
        std::strlen(name), found, &out, &len));
    std::string s = Str(out, len);
    gdefstor_bytes_free(nullptr, out);
    return s;
  }

  std::string Export(GDEFSTOR_KeyStore * k, int64_t taken) {
    uint8_t * out = nullptr;
    size_t len = 0;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_keystore_export(k, "t1", 2, taken, &out,
        &len));
    std::string s = Str(out, len);
    gdefstor_bytes_free(nullptr, out);
    return s;
  }

  size_t LedgerRows(GDEFSTOR_KeyStore * k) {
    GDEFSTOR_Ledger * l = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_keystore_ledger(k, "t1", 2, &l));
    size_t n = gdefstor_ledger_count(l);
    gdefstor_ledger_free(l);
    return n;
  }
};

TEST_F(KeyStoreSets, SealedMaterialIsKeptAndGoesWithTheErasure) {
  const std::string m = "wrapped-under-the-callers-keyring";
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_put(ks, "t1", 2, "p1", 2,
      U(m), m.size()));
  GDEFSTOR_Result rc;
  int erased = 0;
  EXPECT_EQ(m, Sealed(ks, "p1", &rc, &erased));
  EXPECT_EQ(GDEFSTOR_OK, rc);
  EXPECT_EQ(0, erased);
  /* One key per subject, in one form. */
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_put(ks, "t1", 2,
      "p1", 2, U(m), m.size()));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_create(ks, "t1", 2,
      "p1", 2));
  /* Unknown here, and per tenant. */
  Sealed(ks, "p2", &rc, &erased);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, rc);
  {
    uint8_t * out = nullptr;
    size_t len = 0;
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_sealed(ks, "t2",
        2, "p1", 2, &erased, &out, &len));
  }
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "p1", 2, 41));
  EXPECT_EQ("", Sealed(ks, "p1", &rc, &erased));
  EXPECT_EQ(GDEFSTOR_OK, rc);
  EXPECT_EQ(1, erased);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_put(ks, "t1", 2,
      "p1", 2, U(m), m.size()));
  /* Erased without ever having material: still erased, never given it. */
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "p3", 2, 42));
  Sealed(ks, "p3", &rc, &erased);
  EXPECT_EQ(GDEFSTOR_OK, rc);
  EXPECT_EQ(1, erased);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_put(ks, "t1", 2,
      "p3", 2, U(m), m.size()));
  std::string big(GDEFSTOR_KEYSTORE_MAX_MATERIAL_BYTES + 1, 'x');
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, gdefstor_keystore_subject_put(ks, "t1", 2,
      "p4", 2, U(big), big.size()));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_put(ks, "t1", 2,
      "p4", 2, U(m), 0));
}

TEST_F(KeyStoreSets, AKeyTheStoreMadeIsNeverReadBack) {
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "p1", 2));
  GDEFSTOR_Result rc;
  int erased = 0;
  EXPECT_EQ("", Sealed(ks, "p1", &rc, &erased));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_subject_put(ks, "t1", 2,
      "p1", 2, U(std::string("x")), 1));
}

TEST_F(KeyStoreSets, ASecretIsWrittenOnce) {
  const std::string w = "the-tenant-keyring-wrap";
  int found = -1;
  EXPECT_EQ("", Secret(ks, "keyring", &found));
  EXPECT_EQ(0, found);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_secret_put(ks, "t1", 2, "keyring", 7,
      U(w), w.size()));
  EXPECT_EQ(w, Secret(ks, "keyring", &found));
  EXPECT_EQ(1, found);
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_keystore_secret_put(ks, "t1", 2, "keyring", 7,
      U(w), w.size()));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_secret_put(ks, "t1", 2,
      "keyring", 7, U(std::string("another")), 7));
  EXPECT_EQ(w, Secret(ks, "keyring", &found));
  {
    uint8_t * out = nullptr;
    size_t len = 0;
    found = -1;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_secret_get(ks, "t2", 2, "keyring",
        7, &found, &out, &len));
    EXPECT_EQ(0, found);
  }
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_secret_put(ks, "t1", 2,
      "no spaces", 9, U(w), w.size()));
}

TEST_F(KeyStoreSets, AKeySetCarriesEveryRowAndItsTime) {
  const std::string m = "sealed-material-of-p2";
  const std::string w = "keyring-wrap";
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "p1", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_put(ks, "t1", 2, "p2", 2,
      U(m), m.size()));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "p3", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "p3", 2, 77));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_secret_put(ks, "t1", 2, "keyring", 7,
      U(w), w.size()));
  std::string a = Wrap("p1", "data key");
  std::string set = Export(ks, 1234);
  ASSERT_EQ(0u, set.find("defiant-keyset/1\ntaken 1234\n")) << set;
  {
    GDEFSTOR_KeySet * parsed = nullptr;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_keyset_parse(nullptr, U(set), set.size(),
        &parsed));
    EXPECT_EQ(1234, gdefstor_keyset_taken(parsed));
    /* the secret, p1's key, p2's material and p3's ledger row */
    ASSERT_EQ(4u, gdefstor_keyset_count(parsed));
    int kinds[4] = {GDEFSTOR_KEYROW_SECRET, GDEFSTOR_KEYROW_KEY,
        GDEFSTOR_KEYROW_SEALED, GDEFSTOR_KEYROW_LEDGER};
    for (size_t i = 0; i < 4; i++) {
      GDEFSTOR_KeyRow row;
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_keyset_row(parsed, i, &row));
      EXPECT_EQ(kinds[i], row.kind) << i;
    }
    GDEFSTOR_KeyRow last;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_keyset_row(parsed, 3, &last));
    EXPECT_EQ("p3", std::string(last.name, last.name_len));
    EXPECT_EQ(77, last.when_ms);
    gdefstor_keyset_free(parsed);
  }
  /* Into a key store of its own: the same keys, material, secret, ledger. */
  GDEFSTOR_KeyStore * other = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, KeyStoreOpen(&cipher, &other));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_import(other, "t1", 2, U(set),
      set.size()));
  {
    uint8_t * out = nullptr;
    size_t len = 0;
    int erased = 0;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_unwrap(other, "t1", 2, "p1", 2,
        U(a), a.size(), &erased, &out, &len));
    EXPECT_EQ("data key", Str(out, len));
    gdefstor_bytes_free(nullptr, out);
  }
  GDEFSTOR_Result rc;
  int erased = 0;
  int found = 0;
  EXPECT_EQ(m, Sealed(other, "p2", &rc, &erased));
  EXPECT_EQ(GDEFSTOR_OK, rc);
  Sealed(other, "p3", &rc, &erased);
  EXPECT_EQ(1, erased);
  EXPECT_EQ(w, Secret(other, "keyring", &found));
  EXPECT_EQ(1u, LedgerRows(other));
  /* Importing it again changes nothing: the export reads the same. */
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_import(other, "t1", 2, U(set),
      set.size()));
  EXPECT_EQ(set, Export(other, 1234));
  gdefstor_keystore_destroy(other);
}

TEST_F(KeyStoreSets, AnImportNeverBringsBackAnErasedKey) {
  const std::string m = "sealed";
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "p1", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_put(ks, "t1", 2, "p2", 2,
      U(m), m.size()));
  std::string before = Export(ks, 5);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "p1", 2, 6));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "p2", 2, 7));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_import(ks, "t1", 2, U(before),
      before.size()));
  GDEFSTOR_Result rc;
  int erased = 0;
  Wrap("p1", "x", &rc, &erased);
  EXPECT_EQ(GDEFSTOR_OK, rc);
  EXPECT_EQ(1, erased);
  Sealed(ks, "p2", &rc, &erased);
  EXPECT_EQ(1, erased);
  EXPECT_EQ(2u, LedgerRows(ks));
  /* And a set whose ledger is newer erases what the store still holds. */
  GDEFSTOR_KeyStore * other = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, KeyStoreOpen(&cipher, &other));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_import(other, "t1", 2, U(before),
      before.size()));
  Sealed(other, "p2", &rc, &erased);
  ASSERT_EQ(0, erased);
  std::string after = Export(ks, 8);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_import(other, "t1", 2, U(after),
      after.size()));
  Sealed(other, "p2", &rc, &erased);
  EXPECT_EQ(1, erased);
  EXPECT_EQ(2u, LedgerRows(other));
  gdefstor_keystore_destroy(other);
}

TEST_F(KeyStoreSets, AnImportWithAConflictWritesNothing) {
  const std::string w = "wrap-one";
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_secret_put(ks, "t1", 2, "keyring", 7,
      U(w), w.size()));
  GDEFSTOR_KeyStore * other = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, KeyStoreOpen(&cipher, &other));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_secret_put(other, "t1", 2, "keyring",
      7, U(std::string("wrap-two")), 8));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_put(other, "t1", 2, "p9", 2,
      U(std::string("m9")), 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(other, "t1", 2, "p8", 2, 3));
  std::string theirs = Export(other, 9);
  std::string mine = Export(ks, 9);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_import(ks, "t1", 2,
      U(theirs), theirs.size()));
  EXPECT_EQ(mine, Export(ks, 9)) << "a refused import wrote something";
  gdefstor_keystore_destroy(other);
}

TEST_F(KeyStoreSets, AMalformedKeySetIsRefused) {
  std::string good = Export(ks, 1);
  struct Bad {
    const char * text;
    GDEFSTOR_Result rc;
  } bad[] = {
      {"", GDEFSTOR_ERR_FORMAT},
      {"defiant-snapshot/1\n", GDEFSTOR_ERR_FORMAT},
      {"defiant-keyset/2\ntaken 1\n", GDEFSTOR_ERR_UNSUPPORTED},
      {"defiant-keyset/1\n", GDEFSTOR_ERR_CORRUPT},
      {"defiant-keyset/1\ntaken -1\n", GDEFSTOR_ERR_CORRUPT},
      {"defiant-keyset/1\ntaken 1\nsealed p1 0a", GDEFSTOR_ERR_CORRUPT},
      {"defiant-keyset/1\ntaken 1\nsealed p1 0g\n", GDEFSTOR_ERR_CORRUPT},
      {"defiant-keyset/1\ntaken 1\nsealed p1 abc\n", GDEFSTOR_ERR_CORRUPT},
      {"defiant-keyset/1\ntaken 1\nsealed p.1 0a\n", GDEFSTOR_ERR_CORRUPT},
      {"defiant-keyset/1\ntaken 1\nledger p1 7\nsealed p1 0a\n",
          GDEFSTOR_ERR_CORRUPT},
      {"defiant-keyset/1\ntaken 1\nkey p1 0a\nsealed p1 0a\n",
          GDEFSTOR_ERR_CORRUPT},
      {"defiant-keyset/1\ntaken 1\nsealed p1 0a\nsealed p1 0b\n",
          GDEFSTOR_ERR_CORRUPT},
      {"defiant-keyset/1\ntaken 1\nwhat p1 0a\n", GDEFSTOR_ERR_CORRUPT},
  };
  for (const Bad & b : bad) {
    std::string t = b.text;
    EXPECT_EQ(b.rc, gdefstor_keystore_import(ks, "t1", 2, U(t), t.size()))
        << t;
  }
  EXPECT_EQ(good, Export(ks, 1)) << "a refused key set wrote something";
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_keystore_import(ks, "t1", 2,
      U(std::string("defiant-keyset/1\ntaken 1\nsealed p1 0a\n")), 38));
}

} /* namespace keystore_conformance */
