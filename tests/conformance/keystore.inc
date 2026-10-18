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

} /* namespace keystore_conformance */
