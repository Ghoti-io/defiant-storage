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
 * Records, counters and the outbox in the entity write (model.h; spine AD-31,
 * AD-33; story S6a): part of the entity conformance suite, included by
 * model.inc inside its namespace, so every backend runs it. The rules are the
 * story's: one write commits an entity save, a record, a counter and an outbox
 * row together or none of them; a counter never hands a value out twice, an
 * aborted write's value included; a stream is claimed in order and by one live
 * lease at a time; a parked row is counted, never claimed and never deleted;
 * rebuild touches none of it. A seeded sweep checks every answer against a
 * model written apart from the library (SysModel below).
 */

/* ---- helpers ------------------------------------------------------------- */

class ModelSystem : public ModelSave {
 protected:
  GDEFSTOR_Result Put(const std::string & ns, const std::string & key,
      const std::string & value) {
    return InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_record_put(w, ns.data(), ns.size(), key.data(),
          key.size(), reinterpret_cast<const uint8_t *>(value.data()),
          value.size());
    });
  }

  GDEFSTOR_Result Delete(const std::string & ns, const std::string & key,
      int * deleted = nullptr) {
    return InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_record_delete(w, ns.data(), ns.size(), key.data(),
          key.size(), deleted);
    });
  }

  /* The value, or "<missing>". */
  std::string Get(const std::string & ns, const std::string & key,
      const char * tenant = kTenant) {
    int found = -1;
    uint8_t * bytes = nullptr;
    size_t len = 0;
    GDEFSTOR_Result rc = gdefstor_record_get(store, tenant, std::strlen(tenant),
        ns.data(), ns.size(), key.data(), key.size(), &found, &bytes, &len);
    EXPECT_EQ(GDEFSTOR_OK, rc);
    if (rc != GDEFSTOR_OK) {
      return "<error>";
    }
    if (!found) {
      EXPECT_EQ(nullptr, bytes);
      return "<missing>";
    }
    EXPECT_NE(nullptr, bytes); /* even an empty value is a block */
    std::string v(reinterpret_cast<const char *>(bytes), len);
    gdefstor_bytes_free(nullptr, bytes);
    return v;
  }

  /* "key=value" per record of the page. */
  std::vector<std::string> List(const std::string & ns,
      const std::string & prefix, uint64_t offset = 0, size_t limit = 1000) {
    std::vector<std::string> out;
    GDEFSTOR_RecordList * l = nullptr;
    GDEFSTOR_Result rc = gdefstor_record_list(store, kTenant, 2, ns.data(),
        ns.size(), prefix.data(), prefix.size(), offset, limit, &l);
    EXPECT_EQ(GDEFSTOR_OK, rc);
    for (size_t i = 0; i < gdefstor_record_list_count(l); i++) {
      const char * k = nullptr;
      size_t kn = 0;
      const uint8_t * b = nullptr;
      size_t bn = 0;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_record_list_item(l, i, &k, &kn, &b, &bn));
      out.push_back(std::string(k, kn) + "=" +
                    std::string(reinterpret_cast<const char *>(b), bn));
    }
    gdefstor_record_list_free(l);
    return out;
  }

  uint64_t Peek(const std::string & name, const char * tenant = kTenant) {
    uint64_t v = UINT64_MAX;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_counter_peek(store, tenant,
        std::strlen(tenant), name.data(), name.size(), &v));
    return v;
  }

  uint64_t Next(GDEFSTOR_Write * w, const std::string & name) {
    uint64_t v = 0;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_counter_next(w, name.data(), name.size(),
        &v));
    return v;
  }

  GDEFSTOR_Result Append(GDEFSTOR_Write * w, const std::string & stream,
      const std::string & pid, const std::string & payload = "{}",
      uint64_t * seq = nullptr, const std::string & format = "test.event/1") {
    return gdefstor_model_outbox_append(w, stream.data(), stream.size(),
        pid.data(), pid.size(), format.data(), format.size(),
        reinterpret_cast<const uint8_t *>(payload.data()), payload.size(), seq);
  }

  GDEFSTOR_Result AppendOne(const std::string & stream, const std::string & pid,
      uint64_t * seq = nullptr) {
    return InWrite([&](GDEFSTOR_Write * w) {
      return Append(w, stream, pid, "{}", seq);
    });
  }

  /* "pid@seq" per claimed row, after checking the lease each says. */
  std::vector<std::string> Claim(const std::string & stream, int64_t lease,
      int64_t now, size_t max, std::vector<GDEFSTOR_OutboxEntry> * rows = nullptr,
      GDEFSTOR_OutboxClaim ** keep = nullptr) {
    std::vector<std::string> out;
    GDEFSTOR_OutboxClaim * c = nullptr;
    GDEFSTOR_Result rc = gdefstor_model_outbox_claim(store, kTenant, 2,
        stream.data(), stream.size(), lease, now, max, &c);
    EXPECT_EQ(GDEFSTOR_OK, rc);
    for (size_t i = 0; i < gdefstor_model_outbox_claim_count(c); i++) {
      GDEFSTOR_OutboxEntry e;
      std::memset(&e, 0, sizeof e);
      e.size = sizeof e;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_model_outbox_claim_entry(c, i, &e));
      EXPECT_EQ(now + lease, e.lease_until_ms);
      EXPECT_EQ(stream, std::string(e.stream, e.stream_len));
      out.push_back(std::string(e.public_id, e.public_id_len) + "@" +
                    std::to_string(e.seq));
      if (rows != nullptr) {
        rows->push_back(e);
      }
    }
    if (keep != nullptr) {
      *keep = c;
    } else {
      gdefstor_model_outbox_claim_free(c);
    }
    return out;
  }

  size_t Delivered(const std::vector<std::string> & ids) {
    std::vector<const char *> p;
    std::vector<size_t> n;
    for (const auto & id : ids) {
      p.push_back(id.data());
      n.push_back(id.size());
    }
    size_t deleted = SIZE_MAX;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_model_outbox_delivered(store, kTenant, 2,
        p.data(), n.data(), ids.size(), &deleted));
    return deleted;
  }

  int Failed(const std::string & pid, int64_t now) {
    int parked = -1;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_model_outbox_failed(store, kTenant, 2,
        pid.data(), pid.size(), now, &parked));
    return parked;
  }

  uint64_t Parked(const std::string & stream) {
    uint64_t n = UINT64_MAX;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_model_outbox_parked(store, kTenant, 2,
        stream.data(), stream.size(), &n));
    return n;
  }

  /* The dump's record (K), counter (C) and outbox (O) lines. */
  std::string SysLines() {
    std::string out;
    for (char tag : {'K', 'C', 'O'}) {
      for (const std::string & l : Lines(Dump(GDEFSTOR_MODEL_TRUTH), tag)) {
        out += l + "\n";
      }
    }
    return out;
  }
};

/* ---- records ------------------------------------------------------------- */

class ModelRecords : public ModelSystem {};

TEST_F(ModelRecords, PutGetReplaceAndDelete) {
  EXPECT_EQ("<missing>", Get("core.module", "blog"));
  ASSERT_EQ(GDEFSTOR_OK, Put("core.module", "blog", "enabled"));
  EXPECT_EQ("enabled", Get("core.module", "blog"));
  ASSERT_EQ(GDEFSTOR_OK, Put("core.module", "blog", "disabled:2"));
  EXPECT_EQ("disabled:2", Get("core.module", "blog"));
  int deleted = -1;
  ASSERT_EQ(GDEFSTOR_OK, Delete("core.module", "blog", &deleted));
  EXPECT_EQ(1, deleted);
  EXPECT_EQ("<missing>", Get("core.module", "blog"));
  /* Deleting what is not there is success that says so. */
  ASSERT_EQ(GDEFSTOR_OK, Delete("core.module", "blog", &deleted));
  EXPECT_EQ(0, deleted);
  /* The same key in another namespace is another record. */
  ASSERT_EQ(GDEFSTOR_OK, Put("core.module", "x", "1"));
  ASSERT_EQ(GDEFSTOR_OK, Put("core.name", "x", "2"));
  EXPECT_EQ("1", Get("core.module", "x"));
  EXPECT_EQ("2", Get("core.name", "x"));
  /* Values are bytes. */
  std::string bin("\0\xff\x01 {", 5);
  ASSERT_EQ(GDEFSTOR_OK, Put("core.module", "bin", bin));
  EXPECT_EQ(bin, Get("core.module", "bin"));
}

TEST_F(ModelRecords, AMissingRecordIsADistinctResultNotAnError) {
  /* An empty value is a record; no record is not an empty value. */
  ASSERT_EQ(GDEFSTOR_OK, Put("core.flag", "on", ""));
  EXPECT_EQ("", Get("core.flag", "on"));
  EXPECT_EQ("<missing>", Get("core.flag", "off"));
  EXPECT_EQ("<missing>", Get("core.flag", "on", "t2")); /* another tenant */
  EXPECT_EQ("<missing>", Get("core.never", "on"));
}

TEST_F(ModelRecords, AListIsOneNamespaceAndPrefixInKeyOrderPagedByOffsetAndLimit) {
  const char * const keys[] = {"b.1", "a:b", "ab", "a", "B", "a_b", "b", "a.b",
      "a-b"};
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    for (const char * k : keys) {
      GDEFSTOR_Result rc = gdefstor_record_put(w, "core.map", 8, k,
          std::strlen(k), reinterpret_cast<const uint8_t *>(k), std::strlen(k));
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    return gdefstor_record_put(w, "core.other", 10, "a", 1,
        reinterpret_cast<const uint8_t *>("other"), 5);
  }));
  /* By bytes: "B" before "a", and "-" "." ":" "_" in their byte order. */
  std::vector<std::string> all = {"B=B", "a=a", "a-b=a-b", "a.b=a.b",
      "a:b=a:b", "a_b=a_b", "ab=ab", "b=b", "b.1=b.1"};
  EXPECT_EQ(all, List("core.map", ""));
  EXPECT_EQ((std::vector<std::string>{"a=a", "a-b=a-b", "a.b=a.b", "a:b=a:b",
                "a_b=a_b", "ab=ab"}),
      List("core.map", "a"));
  EXPECT_EQ((std::vector<std::string>{"a.b=a.b"}), List("core.map", "a."));
  EXPECT_EQ((std::vector<std::string>{"b=b", "b.1=b.1"}), List("core.map", "b"));
  EXPECT_TRUE(List("core.map", "c").empty());
  EXPECT_EQ((std::vector<std::string>{"a=other"}), List("core.other", ""));
  EXPECT_TRUE(List("core.none", "").empty());
  /* Pages put back together are the whole list, in order. */
  std::vector<std::string> paged;
  for (uint64_t off = 0; off < 12; off += 2) {
    std::vector<std::string> p = List("core.map", "", off, 2);
    EXPECT_LE(p.size(), 2u);
    paged.insert(paged.end(), p.begin(), p.end());
  }
  EXPECT_EQ(all, paged);
  EXPECT_EQ((std::vector<std::string>{"a.b=a.b", "a:b=a:b"}),
      List("core.map", "a", 2, 2));
  EXPECT_TRUE(List("core.map", "a", 6, 5).empty());
}

TEST_F(ModelRecords, NamesValuesAndPagesAreChecked) {
  std::string big(GDEFSTOR_RECORD_MAX_BYTES + 1, 'x');
  std::string long_ns(GDEFSTOR_SYSTEM_MAX_NAME_BYTES + 1, 'n');
  std::string long_key(GDEFSTOR_RECORD_MAX_KEY_BYTES + 1, 'k');
  std::string max_key(GDEFSTOR_RECORD_MAX_KEY_BYTES, 'k');
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    const uint8_t v[1] = {'v'};
    struct Bad {
      std::string ns;
      std::string key;
    };
    const Bad bad[] = {{"", "k"}, {"core x", "k"}, {"core/x", "k"},
        {long_ns, "k"}, {"core.x", ""}, {"core.x", "a/b"}, {"core.x", "a b"},
        {"core.x", long_key}, {"core.x", std::string("a\0b", 3)},
        {"core.\xc3\xa9", "k"}};
    for (const Bad & b : bad) {
      if (gdefstor_record_put(w, b.ns.data(), b.ns.size(), b.key.data(),
              b.key.size(), v, 1) != GDEFSTOR_ERR_INVALID) {
        ADD_FAILURE() << "accepted ns [" << b.ns << "] key [" << b.key << "]";
      }
    }
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_record_put(w, "core.x", 6, "k", 1,
        nullptr, 1));
    EXPECT_EQ(GDEFSTOR_ERR_LIMIT, gdefstor_record_put(w, "core.x", 6, "k", 1,
        reinterpret_cast<const uint8_t *>(big.data()), big.size()));
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_record_delete(w, "", 0, "k", 1,
        nullptr));
    /* Refused calls leave the write usable. */
    return gdefstor_record_put(w, "core.x", 6, max_key.data(), max_key.size(),
        v, 1);
  }));
  EXPECT_EQ("v", Get("core.x", max_key));
  GDEFSTOR_RecordList * l = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_record_list(store, kTenant, 2,
      "core.x", 6, "", 0, 0, 0, &l));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, gdefstor_record_list(store, kTenant, 2,
      "core.x", 6, "", 0, 0, GDEFSTOR_RECORD_MAX_PAGE + 1, &l));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_record_list(store, kTenant, 2,
      "core.x", 6, "a/", 2, 0, 1, &l));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_record_list(store, kTenant, 2,
      "core x", 6, "", 0, 0, 1, &l));
  EXPECT_EQ(nullptr, l);
  int found = 0;
  uint8_t * b = nullptr;
  size_t n = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_record_get(store, kTenant, 2,
      "core.x", 6, "a b", 3, &found, &b, &n));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_record_get(store, kTenant, 2,
      "core.x", 6, "k", 1, nullptr, &b, &n));
}

/* A record is written in the write's transaction: an abort leaves no record
 * put, no record replaced and no record deleted. */
TEST_F(ModelRecords, AnAbortedWriteLeavesNoRecord) {
  ASSERT_EQ(GDEFSTOR_OK, Put("core.module", "kept", "old"));
  std::string before = SysLines();
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.module", 11, "new", 3,
      reinterpret_cast<const uint8_t *>("x"), 1));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.module", 11, "kept", 4,
      reinterpret_cast<const uint8_t *>("changed"), 7));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_delete(w, "core.module", 11, "kept", 4,
      nullptr));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.module", 11, "kept", 4,
      reinterpret_cast<const uint8_t *>("again"), 5));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
  EXPECT_EQ("<missing>", Get("core.module", "new"));
  EXPECT_EQ("old", Get("core.module", "kept"));
  EXPECT_EQ(before, SysLines());
  /* A write that takes a counter after its records is aborted the same. */
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.module", 11, "new", 3,
      reinterpret_cast<const uint8_t *>("y"), 1));
  Next(w, "core.epoch");
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
  EXPECT_EQ("<missing>", Get("core.module", "new"));
  EXPECT_EQ(before, SysLines());
}

TEST_F(ModelRecords, AnOpenWriteIsInvisibleToReaders) {
  ASSERT_EQ(GDEFSTOR_OK, Put("core.module", "a", "1"));
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.module", 11, "a", 1,
      reinterpret_cast<const uint8_t *>("2"), 1));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.module", 11, "b", 1,
      reinterpret_cast<const uint8_t *>("3"), 1));
  EXPECT_EQ("1", Get("core.module", "a"));
  EXPECT_EQ("<missing>", Get("core.module", "b"));
  EXPECT_EQ((std::vector<std::string>{"a=1"}), List("core.module", ""));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(w));
  EXPECT_EQ((std::vector<std::string>{"a=2", "b=3"}), List("core.module", ""));
}

/* ---- counters ------------------------------------------------------------ */

class ModelCounters : public ModelSystem {};

TEST_F(ModelCounters, TheFirstValueIsOneAndEachNextIsOneMore) {
  EXPECT_EQ(0u, Peek("core.epoch"));
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  EXPECT_EQ(1u, Next(w, "core.epoch"));
  EXPECT_EQ(2u, Next(w, "core.epoch"));
  EXPECT_EQ(0u, Peek("core.epoch")); /* the last commit */
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(w));
  EXPECT_EQ(2u, Peek("core.epoch"));
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * x) {
    EXPECT_EQ(3u, Next(x, "core.epoch"));
    return GDEFSTOR_OK;
  }));
  EXPECT_EQ(3u, Peek("core.epoch"));
}

/* The value an aborted write took is never taken again: the next write takes
 * the one after it, and a peek still reads the last commit. */
TEST_F(ModelCounters, AnAbortedWritesValueIsSkippedNotReused) {
  std::set<uint64_t> given;
  uint64_t last = 0;
  auto take = [&](GDEFSTOR_Write * w) {
    uint64_t v = Next(w, "core.epoch");
    EXPECT_GT(v, last) << "a counter went backwards";
    EXPECT_TRUE(given.insert(v).second) << "value " << v << " handed out twice";
    last = v;
    return v;
  };
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    take(w);
    return GDEFSTOR_OK;
  }));
  EXPECT_EQ(1u, Peek("core.epoch"));
  for (int round = 0; round < 3; round++) {
    GDEFSTOR_Write * w = nullptr;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
    take(w);
    take(w);
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
    EXPECT_EQ(1u, Peek("core.epoch")) << "an aborted value was committed";
  }
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    EXPECT_EQ(8u, take(w));
    return GDEFSTOR_OK;
  }));
  EXPECT_EQ(8u, Peek("core.epoch"));
  /* A write broken by a refused commit is an abort too. */
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  take(w);
  ASSERT_EQ(GDEFSTOR_OK, Append(w, "core.events", "dup1"));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "core.events", "dup1"));
  EXPECT_NE(GDEFSTOR_OK, gdefstor_write_commit(w));
  EXPECT_EQ(8u, Peek("core.epoch"));
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * x) {
    EXPECT_EQ(10u, take(x));
    return GDEFSTOR_OK;
  }));
}

TEST_F(ModelCounters, CountersAreApartByNameAndTenant) {
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    EXPECT_EQ(1u, Next(w, "a"));
    EXPECT_EQ(1u, Next(w, "b"));
    EXPECT_EQ(2u, Next(w, "a"));
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_counter_next(w, "a b", 3,
        nullptr));
    uint64_t v = 0;
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_counter_next(w, "", 0, &v));
    return GDEFSTOR_OK;
  }));
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, "t2", 2, nullptr, &w));
  EXPECT_EQ(1u, Next(w, "a"));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(w));
  EXPECT_EQ(2u, Peek("a"));
  EXPECT_EQ(1u, Peek("b"));
  EXPECT_EQ(1u, Peek("a", "t2"));
  EXPECT_EQ(0u, Peek("b", "t2"));
  /* An outbox stream's sequence is not a counter of the same name. */
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("a", "e1"));
  EXPECT_EQ(2u, Peek("a"));
}

/* ---- the outbox ---------------------------------------------------------- */

class ModelOutbox : public ModelSystem {};

TEST_F(ModelOutbox, AppendsAreSequencedPerStream) {
  uint64_t s[4] = {0, 0, 0, 0};
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = Append(w, "core.bus", "m1", "{}", &s[0]);
    if (rc == GDEFSTOR_OK) {
      rc = Append(w, "core.log", "m2", "{}", &s[1]);
    }
    if (rc == GDEFSTOR_OK) {
      rc = Append(w, "core.bus", "m3", "{}", &s[2]);
    }
    return rc;
  }));
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "m4", &s[3]));
  EXPECT_EQ(1u, s[0]);
  EXPECT_EQ(1u, s[1]);
  EXPECT_EQ(2u, s[2]);
  EXPECT_EQ(3u, s[3]);
  /* A sequence is not given out again after its row was delivered. */
  EXPECT_EQ((std::vector<std::string>{"m1@1", "m3@2", "m4@3"}),
      Claim("core.bus", 10, 0, 5));
  EXPECT_EQ(3u, Delivered({"m1", "m3", "m4"}));
  uint64_t again = 0;
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "m5", &again));
  EXPECT_EQ(4u, again);
}

TEST_F(ModelOutbox, ThreeAppendsAreClaimedInSequence) {
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = Append(w, "core.bus", "a1");
    return rc == GDEFSTOR_OK ? Append(w, "core.bus", "a2") : rc;
  }));
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "a3"));
  EXPECT_EQ((std::vector<std::string>{"a1@1"}), Claim("core.bus", 10, 100, 1));
  EXPECT_EQ(1u, Delivered({"a1"}));
  EXPECT_EQ((std::vector<std::string>{"a2@2"}), Claim("core.bus", 10, 100, 1));
  EXPECT_EQ(1u, Delivered({"a2"}));
  EXPECT_EQ((std::vector<std::string>{"a3@3"}), Claim("core.bus", 10, 100, 1));
  EXPECT_EQ(1u, Delivered({"a3"}));
  EXPECT_TRUE(Claim("core.bus", 10, 100, 1).empty());
  /* Several at once come in sequence too. */
  for (const char * id : {"b1", "b2", "b3"}) {
    ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.log", id));
  }
  EXPECT_EQ((std::vector<std::string>{"b1@1", "b2@2", "b3@3"}),
      Claim("core.log", 10, 100, 3));
  /* After the lease ends, the claim starts again from the lowest. */
  EXPECT_EQ((std::vector<std::string>{"b1@1", "b2@2"}),
      Claim("core.log", 10, 110, 2));
}

TEST_F(ModelOutbox, ASecondClaimerDuringALiveLeaseGetsNothing) {
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "d1"));
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "d2"));
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.other", "o1"));
  EXPECT_EQ((std::vector<std::string>{"d1@1"}), Claim("core.bus", 500, 1000, 1));
  /* The lease holds the stream, unclaimed rows and all. */
  EXPECT_TRUE(Claim("core.bus", 500, 1200, 1).empty());
  EXPECT_TRUE(Claim("core.bus", 500, 1499, 5).empty());
  /* Another stream is not held. */
  EXPECT_EQ((std::vector<std::string>{"o1@1"}), Claim("core.other", 5, 1200, 1));
  /* The lease ended at 1500: the row is claimable again, and first. */
  std::vector<GDEFSTOR_OutboxEntry> rows;
  EXPECT_EQ((std::vector<std::string>{"d1@1"}),
      Claim("core.bus", 500, 1500, 1, &rows));
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ(0u, rows[0].failures);
  EXPECT_TRUE(Claim("core.bus", 500, 1600, 1).empty());
  EXPECT_EQ(1u, Delivered({"d1"}));
  /* Delivering the held row releases nothing else: d2 was never leased. */
  EXPECT_EQ((std::vector<std::string>{"d2@2"}), Claim("core.bus", 500, 1600, 1));
}

TEST_F(ModelOutbox, AFailingRowIsParkedCountedNeverClaimedAndKept) {
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "p1"));
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "p2"));
  for (uint32_t i = 1; i <= GDEFSTOR_OUTBOX_PARK_FAILURES; i++) {
    int64_t now = 100 * static_cast<int64_t>(i);
    std::vector<GDEFSTOR_OutboxEntry> rows;
    ASSERT_EQ((std::vector<std::string>{"p1@1"}),
        Claim("core.bus", 10, now, 1, &rows))
        << "failure " << i;
    EXPECT_EQ(i - 1, rows[0].failures);
    EXPECT_EQ(i == GDEFSTOR_OUTBOX_PARK_FAILURES ? 1 : 0, Failed("p1", now + 1))
        << "failure " << i;
    EXPECT_EQ(i == GDEFSTOR_OUTBOX_PARK_FAILURES ? 1u : 0u, Parked("core.bus"));
  }
  EXPECT_EQ(1u, Parked(""));
  EXPECT_EQ(0u, Parked("core.other"));
  /* Parked: skipped by every claim, and the stream goes on. */
  EXPECT_EQ((std::vector<std::string>{"p2@2"}), Claim("core.bus", 10, 1000, 5));
  /* Never deleted: not by a delivery, not by another failure. */
  EXPECT_EQ(0u, Delivered({"p1"}));
  EXPECT_EQ(1, Failed("p1", 1001));
  EXPECT_EQ(1u, Delivered({"p2"}));
  EXPECT_TRUE(Claim("core.bus", 10, 2000, 5).empty());
  EXPECT_EQ(1u, Parked("core.bus"));
  std::vector<std::string> left = Lines(Dump(GDEFSTOR_MODEL_TRUTH), 'O');
  ASSERT_EQ(1u, left.size());
  std::vector<std::string> w = Words(left[0]);
  ASSERT_EQ(9u, w.size());
  EXPECT_EQ(HexOf("p1"), w[3]);
  EXPECT_EQ(std::to_string(GDEFSTOR_OUTBOX_PARK_FAILURES), w[7]);
  EXPECT_EQ("1", w[8]);
}

TEST_F(ModelOutbox, RowsOfAnAbortedWriteNeverAppear) {
  std::string before = SysLines();
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  ASSERT_EQ(GDEFSTOR_OK, Append(w, "core.bus", "r1"));
  ASSERT_EQ(GDEFSTOR_OK, Append(w, "core.bus", "r2"));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
  EXPECT_TRUE(Claim("core.bus", 10, 0, 5).empty());
  EXPECT_EQ(0u, Parked(""));
  EXPECT_EQ(before, SysLines());
  /* Its public ids were never used. */
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "r1"));
  std::vector<std::string> got = Claim("core.bus", 10, 0, 5);
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ("r1@", got[0].substr(0, 3));
}

TEST_F(ModelOutbox, ThePayloadIsOpaqueBytes) {
  std::string odd("\0\xff{not json\x01", 13);
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = Append(w, "core.bus", "x1", odd, nullptr,
        "defiant-change/1");
    return rc == GDEFSTOR_OK ? Append(w, "core.bus", "x2", "", nullptr, "e.f:g/2")
                             : rc;
  }));
  std::vector<GDEFSTOR_OutboxEntry> rows;
  GDEFSTOR_OutboxClaim * c = nullptr;
  EXPECT_EQ(2u, Claim("core.bus", 10, 0, 2, &rows, &c).size());
  ASSERT_EQ(2u, rows.size());
  EXPECT_EQ(odd, std::string(reinterpret_cast<const char *>(rows[0].payload),
                     rows[0].payload_len));
  EXPECT_EQ("defiant-change/1", std::string(rows[0].format, rows[0].format_len));
  EXPECT_EQ(0u, rows[1].payload_len);
  EXPECT_EQ("e.f:g/2", std::string(rows[1].format, rows[1].format_len));
  gdefstor_model_outbox_claim_free(c);
}

TEST_F(ModelOutbox, DeliveredSkipsWhatIsGone) {
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "g1"));
  ASSERT_EQ(GDEFSTOR_OK, AppendOne("core.bus", "g2"));
  EXPECT_EQ(0u, Delivered({}));
  EXPECT_EQ(0u, Delivered({"nobody"}));
  EXPECT_EQ(1u, Delivered({"g1", "nobody", "g1"}));
  EXPECT_EQ(0u, Delivered({"g1"}));
  EXPECT_EQ((std::vector<std::string>{"g2@2"}), Claim("core.bus", 10, 0, 5));
}

TEST_F(ModelOutbox, ArgumentsAreChecked) {
  std::string big(GDEFSTOR_OUTBOX_MAX_PAYLOAD_BYTES + 1, 'x');
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "", "i1"));
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "core bus", "i1"));
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "core/bus", "i1"));
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "core.bus", ""));
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "core.bus", "i.1"));
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "core.bus", "i1", "{}", nullptr,
        ""));
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "core.bus", "i1", "{}", nullptr,
        "a b/1"));
    EXPECT_EQ(GDEFSTOR_ERR_LIMIT, Append(w, "core.bus", "i1", big));
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_model_outbox_append(w, "s", 1,
        "i1", 2, "f", 1, nullptr, 1, nullptr));
    /* Refused before anything was written: the write goes on. */
    return Append(w, "core.bus", "i1");
  }));
  GDEFSTOR_OutboxClaim * c = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_model_outbox_claim(store, kTenant, 2,
      "core.bus", 8, 0, 0, 1, &c));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_model_outbox_claim(store, kTenant, 2,
      "core.bus", 8, 10, 0, 0, &c));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, gdefstor_model_outbox_claim(store, kTenant, 2,
      "core.bus", 8, 10, 0, GDEFSTOR_OUTBOX_MAX_CLAIM + 1, &c));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_model_outbox_claim(store, kTenant, 2,
      "core.bus", 8, 10, INT64_MAX - 5, 1, &c));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_model_outbox_claim(store, kTenant, 2,
      "core bus", 8, 10, 0, 1, &c));
  EXPECT_EQ(nullptr, c);
  int parked = -1;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_model_outbox_failed(store, kTenant, 2,
      "nobody", 6, 0, &parked));
  EXPECT_EQ(-1, parked);
  const char * bad[] = {"a b"};
  size_t bad_len[] = {3};
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_model_outbox_delivered(store,
      kTenant, 2, bad, bad_len, 1, nullptr));
  uint64_t n = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_model_outbox_parked(store, kTenant, 2,
      "a b", 3, &n));
  /* A public id already used: refused, and the write that tried is broken. */
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.x", 6, "k", 1,
      reinterpret_cast<const uint8_t *>("v"), 1));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "core.log", "i1"));
  EXPECT_NE(GDEFSTOR_OK, gdefstor_write_commit(w));
  EXPECT_EQ("<missing>", Get("core.x", "k"));
  EXPECT_EQ((std::vector<std::string>{"i1@1"}), Claim("core.bus", 10, 0, 5));
}

/* ---- all of it in one write ---------------------------------------------- */

TEST_F(ModelSystem, AnEntitySaveARecordACounterAndAnAppendCommitTogetherOrNotAtAll) {
  SetUpPo();
  std::string truth = Dump(GDEFSTOR_MODEL_TRUTH);
  std::string current = Dump(GDEFSTOR_MODEL_CURRENT);
  uint64_t taken[2] = {0, 0};
  for (int commit = 0; commit < 2; commit++) {
    GDEFSTOR_Write * w = nullptr;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
    Node now = Current("po1");
    now.Set("total", Decimal(4100 + commit));
    GDEFSTOR_Tree * t = Build(now);
    GDEFSTOR_RevisionMeta m = Meta();
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_save(w, "po1", 3, 1, &m, kLang, kLangLen, t, nullptr));
    gdefstor_tree_free(t);
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.module", 11, "blog", 4,
        reinterpret_cast<const uint8_t *>("on"), 2));
    taken[commit] = Next(w, "core.epoch");
    ASSERT_EQ(GDEFSTOR_OK, Append(w, "core.bus", "chg1"));
    if (!commit) {
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
      EXPECT_EQ(truth, Dump(GDEFSTOR_MODEL_TRUTH));
      EXPECT_EQ(current, Dump(GDEFSTOR_MODEL_CURRENT));
      EXPECT_EQ(1u, Head("po1"));
      EXPECT_EQ("<missing>", Get("core.module", "blog"));
      EXPECT_EQ(0u, Peek("core.epoch"));
      EXPECT_TRUE(Claim("core.bus", 10, 0, 5).empty());
    } else {
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(w));
    }
  }
  EXPECT_EQ(2u, Head("po1"));
  EXPECT_EQ(Val::From(Decimal(4101)), Current("po1").vals["total"]);
  EXPECT_EQ("on", Get("core.module", "blog"));
  EXPECT_GT(taken[1], taken[0]);
  EXPECT_EQ(taken[1], Peek("core.epoch"));
  EXPECT_EQ((std::vector<std::string>{"chg1@1"}), Claim("core.bus", 10, 0, 5));
}

/* Records, counters and outbox rows are truth: a rebuild of the derived data
 * leaves every one as it was, in its own write and in a write that has just
 * written some. */
TEST_F(ModelSystem, RebuildLeavesRecordsCountersAndTheOutboxAlone) {
  SetUpPo();
  ASSERT_EQ(GDEFSTOR_OK, Put("core.module", "blog", "on"));
  ASSERT_EQ(GDEFSTOR_OK, Put("core.name", "site.page", "e7"));
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    Next(w, "core.epoch");
    Next(w, "core.epoch");
    GDEFSTOR_Result rc = Append(w, "core.bus", "q1");
    return rc == GDEFSTOR_OK ? Append(w, "core.bus", "q2") : rc;
  }));
  EXPECT_EQ((std::vector<std::string>{"q1@1"}), Claim("core.bus", 10, 0, 1));
  for (uint32_t i = 0; i < GDEFSTOR_OUTBOX_PARK_FAILURES; i++) {
    Claim("core.bus", 10, 100 + i, 1);
    Failed("q1", 100 + i);
  }
  std::string sys = SysLines();
  ASSERT_FALSE(Lines(sys, 'K').empty());
  ASSERT_FALSE(Lines(sys, 'C').empty());
  ASSERT_FALSE(Lines(sys, 'O').empty());
  ASSERT_EQ(GDEFSTOR_OK, InWrite([](GDEFSTOR_Write * w) {
    return gdefstor_model_rebuild(w);
  }));
  EXPECT_EQ(sys, SysLines());
  EXPECT_EQ("on", Get("core.module", "blog"));
  EXPECT_EQ(2u, Peek("core.epoch"));
  EXPECT_EQ(1u, Parked("core.bus"));
  /* What a write wrote before its rebuild survives the rebuild too. */
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = gdefstor_record_put(w, "core.module", 11, "shop", 4,
        reinterpret_cast<const uint8_t *>("on"), 2);
    Next(w, "core.epoch");
    if (rc == GDEFSTOR_OK) {
      rc = Append(w, "core.bus", "q3");
    }
    return rc == GDEFSTOR_OK ? gdefstor_model_rebuild(w) : rc;
  }));
  EXPECT_EQ("on", Get("core.module", "shop"));
  EXPECT_EQ("on", Get("core.module", "blog"));
  EXPECT_EQ(3u, Peek("core.epoch"));
  EXPECT_EQ((std::vector<std::string>{"q2@2", "q3@3"}),
      Claim("core.bus", 10, 1000, 5));
}

TEST_F(ModelSystem, KeyBytesRowsAndTheseFamiliesDoNotMix) {
  const uint8_t blob[] = {1, 2};
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_blob_put(w, "ab", 2, blob, 2));
  uint64_t v = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_record_put(w, "core.x", 6, "k", 1,
      blob, 2));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_counter_next(w, "c", 1, &v));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Append(w, "core.bus", "k1"));
  gdefstor_write_abort(w);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.x", 6, "k", 1, blob, 2));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_blob_put(w, "ab", 2, blob, 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(w));
  EXPECT_EQ(std::string("\x01\x02", 2), Get("core.x", "k"));
}

/* ---- a seeded sweep against a model -------------------------------------- */

/* The meaning of model.h's records, counters and outbox, written from the
 * header and nothing else. */
struct SysModel {
  struct Row {
    std::string stream;
    uint64_t seq = 0;
    std::string pid;
    std::string payload;
    int64_t until = 0;
    uint32_t fails = 0;
    bool parked = false;
  };
  std::map<std::string, std::map<std::string, std::string>> recs;
  std::map<std::string, uint64_t> counters; /* committed */
  std::map<std::string, uint64_t> floors;   /* every value ever taken */
  std::map<std::string, uint64_t> streams;  /* last sequence, committed */
  std::vector<Row> rows;                    /* by stream, then sequence */
  std::set<std::string> pids;

  void Sort() {
    std::stable_sort(rows.begin(), rows.end(), [](const Row & a, const Row & b) {
      return a.stream != b.stream ? a.stream < b.stream : a.seq < b.seq;
    });
  }

  std::vector<std::string> Claim(const std::string & st, int64_t now,
      int64_t until, size_t max) {
    std::vector<std::string> out;
    for (const Row & r : rows) {
      if (r.stream == st && !r.parked && r.until > now) {
        return out;
      }
    }
    for (Row & r : rows) {
      if (r.stream == st && !r.parked && out.size() < max) {
        r.until = until;
        out.push_back(r.pid + "@" + std::to_string(r.seq));
      }
    }
    return out;
  }
};

class ModelSystemSweep : public ModelSystem {};

TEST_F(ModelSystemSweep, SeededOperationsAgreeWithAnIndependentModel) {
  const char * const nss[] = {"core.module", "core.name"};
  const char * const keys[] = {"a", "a.b", "a:c", "b", "B", "b-1", "c_2", "ab"};
  const char * const names[] = {"core.epoch", "core.next", "x"};
  const char * const streams[] = {"core.bus", "core.log"};
  int commits = 0;
  int aborts = 0;
  int claims_with_rows = 0;
  int reclaims = 0;
  int parks = 0;
  int empty_claims = 0;
  int list_pages = 0;
  for (uint32_t seed = 1; seed <= 6; seed++) {
    TearDown();
    SetUp();
    std::mt19937 rng(seed);
    auto pick = [&](size_t n) { return static_cast<size_t>(rng() % n); };
    SysModel m;
    int64_t clock = 0;
    int next_pid = 0;
    std::set<std::string> ever_claimed;
    for (int step = 0; step < 160 && !HasFailure(); step++) {
      int what = static_cast<int>(pick(10));
      if (what < 4) {
        /* A write of one to four operations, committed or aborted. */
        SysModel x = m;
        std::vector<uint64_t> taken;
        bool commit = pick(5) != 0;
        int ops = 1 + static_cast<int>(pick(4));
        GDEFSTOR_Write * w = nullptr;
        ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr,
            &w));
        for (int k = 0; k < ops; k++) {
          int op = static_cast<int>(pick(4));
          if (op == 0) {
            std::string ns = nss[pick(2)];
            std::string key = keys[pick(8)];
            std::string val = "v" + std::to_string(rng() % 1000);
            ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, ns.data(), ns.size(),
                key.data(), key.size(),
                reinterpret_cast<const uint8_t *>(val.data()), val.size()));
            x.recs[ns][key] = val;
          } else if (op == 1) {
            std::string ns = nss[pick(2)];
            std::string key = keys[pick(8)];
            int deleted = -1;
            ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_delete(w, ns.data(),
                ns.size(), key.data(), key.size(), &deleted));
            EXPECT_EQ(x.recs[ns].erase(key) == 1 ? 1 : 0, deleted);
          } else if (op == 2) {
            std::string n = names[pick(3)];
            uint64_t base = std::max(x.counters[n], m.floors[n]);
            uint64_t got = Next(w, n);
            EXPECT_EQ(base + 1, got) << n;
            x.counters[n] = got;
            m.floors[n] = got; /* kept whatever becomes of the write */
            x.floors[n] = got;
          } else {
            std::string st = streams[pick(2)];
            std::string pid = "p" + std::to_string(++next_pid);
            std::string payload = "{\"n\":" + std::to_string(next_pid) + "}";
            uint64_t seq = 0;
            ASSERT_EQ(GDEFSTOR_OK, Append(w, st, pid, payload, &seq));
            EXPECT_EQ(x.streams[st] + 1, seq) << st;
            x.streams[st] = seq;
            SysModel::Row r;
            r.stream = st;
            r.seq = seq;
            r.pid = pid;
            r.payload = payload;
            x.rows.push_back(r);
            x.Sort();
          }
        }
        if (commit) {
          ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(w));
          m = x;
          commits++;
        } else {
          ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
          aborts++;
        }
      } else if (what == 4) {
        std::string ns = nss[pick(2)];
        std::string key = keys[pick(8)];
        auto it = m.recs[ns].find(key);
        EXPECT_EQ(it == m.recs[ns].end() ? "<missing>" : it->second,
            Get(ns, key));
        std::string n = names[pick(3)];
        EXPECT_EQ(m.counters[n], Peek(n)) << n;
      } else if (what == 5) {
        std::string ns = nss[pick(2)];
        std::string prefix = pick(3) == 0 ? "" : std::string(keys[pick(8)], 1);
        uint64_t off = pick(4);
        size_t lim = 1 + pick(4);
        std::vector<std::string> want;
        uint64_t skipped = 0;
        for (const auto & kv : m.recs[ns]) {
          if (kv.first.compare(0, prefix.size(), prefix) != 0) {
            continue;
          }
          if (skipped++ < off) {
            continue;
          }
          if (want.size() < lim) {
            want.push_back(kv.first + "=" + kv.second);
          }
        }
        EXPECT_EQ(want, List(ns, prefix, off, lim)) << ns << " [" << prefix
                                                    << "] " << off << "+" << lim;
        list_pages += !want.empty();
      } else if (what <= 7) {
        clock += static_cast<int64_t>(pick(30));
        std::string st = streams[pick(2)];
        int64_t lease = 1 + static_cast<int64_t>(pick(40));
        size_t max = 1 + pick(3);
        std::vector<std::string> want = m.Claim(st, clock, clock + lease, max);
        std::vector<GDEFSTOR_OutboxEntry> rows;
        GDEFSTOR_OutboxClaim * c = nullptr;
        std::vector<std::string> got = Claim(st, lease, clock, max, &rows, &c);
        EXPECT_EQ(want, got) << st << " at " << clock;
        for (size_t i = 0; i < rows.size() && i < want.size(); i++) {
          for (const SysModel::Row & r : m.rows) {
            if (r.pid == std::string(rows[i].public_id, rows[i].public_id_len)) {
              EXPECT_EQ(r.payload, std::string(reinterpret_cast<const char *>(
                                                   rows[i].payload),
                                       rows[i].payload_len));
              EXPECT_EQ(r.fails, rows[i].failures);
              reclaims += !ever_claimed.insert(r.pid).second;
            }
          }
        }
        gdefstor_model_outbox_claim_free(c);
        claims_with_rows += !got.empty();
        empty_claims += got.empty();
        /* Deliver some of what was claimed, fail one, or leave it. */
        int then = static_cast<int>(pick(3));
        if (then == 0 && !got.empty()) {
          std::vector<std::string> ids;
          for (const std::string & g : got) {
            if (pick(2) == 0) {
              ids.push_back(g.substr(0, g.find('@')));
            }
          }
          size_t expect = 0;
          for (const std::string & id : ids) {
            for (size_t i = 0; i < m.rows.size(); i++) {
              if (m.rows[i].pid == id && !m.rows[i].parked) {
                m.rows.erase(m.rows.begin() + static_cast<long>(i));
                expect++;
                break;
              }
            }
          }
          EXPECT_EQ(expect, Delivered(ids));
        } else if (then == 1 && !got.empty()) {
          std::string id = got[0].substr(0, got[0].find('@'));
          for (SysModel::Row & r : m.rows) {
            if (r.pid == id) {
              r.fails += 1;
              if (r.fails >= GDEFSTOR_OUTBOX_PARK_FAILURES) {
                r.parked = true;
                r.until = 0;
                parks++;
              } else {
                r.until = clock;
              }
              EXPECT_EQ(r.parked ? 1 : 0, Failed(id, clock));
            }
          }
        }
      } else if (what == 8) {
        std::string st = streams[pick(2)];
        uint64_t want = 0;
        uint64_t all = 0;
        for (const SysModel::Row & r : m.rows) {
          want += r.parked && r.stream == st;
          all += r.parked;
        }
        EXPECT_EQ(want, Parked(st));
        EXPECT_EQ(all, Parked(""));
      } else {
        /* A rebuild changes nothing here. */
        std::string before = SysLines();
        ASSERT_EQ(GDEFSTOR_OK, InWrite([](GDEFSTOR_Write * w) {
          return gdefstor_model_rebuild(w);
        }));
        EXPECT_EQ(before, SysLines());
      }
    }
    /* At the end, every record and counter, and every row with its state. */
    for (const char * ns : nss) {
      std::vector<std::string> want;
      for (const auto & kv : m.recs[ns]) {
        want.push_back(kv.first + "=" + kv.second);
      }
      EXPECT_EQ(want, List(ns, "")) << ns;
    }
    std::vector<std::string> want_rows;
    for (const SysModel::Row & r : m.rows) {
      want_rows.push_back(HexOf(r.stream) + " " + std::to_string(r.seq) + " " +
                          HexOf(r.pid) + " " + std::to_string(r.until) + " " +
                          std::to_string(r.fails) + " " + (r.parked ? "1" : "0"));
    }
    std::vector<std::string> got_rows;
    for (const std::string & l : Lines(Dump(GDEFSTOR_MODEL_TRUTH), 'O')) {
      std::vector<std::string> w = Words(l);
      if (w.size() == 9) {
        got_rows.push_back(w[1] + " " + w[2] + " " + w[3] + " " + w[6] + " " +
                           w[7] + " " + w[8]);
      }
    }
    std::sort(want_rows.begin(), want_rows.end());
    std::sort(got_rows.begin(), got_rows.end());
    EXPECT_EQ(want_rows, got_rows) << "seed " << seed;
    if (HasFailure()) {
      break;
    }
  }
  std::printf("system sweep: %d commits, %d aborts, %d claims with rows (%d "
              "empty, %d rows claimed again), %d parks, %d list pages\n",
      commits, aborts, claims_with_rows, empty_claims, reclaims, parks,
      list_pages);
  /* Each kind of event happened, or the sweep proves nothing about it. */
  EXPECT_GE(commits, 150);
  EXPECT_GE(aborts, 30);
  EXPECT_GE(claims_with_rows, 60);
  EXPECT_GE(empty_claims, 20);
  EXPECT_GE(reclaims, 10);
  EXPECT_GE(parks, 1);
  EXPECT_GE(list_pages, 30);
}

/* ---- every call under a failing allocator -------------------------------- */

inline GDEFSTOR_Result SystemOomScenario(const GDEFSTOR_Allocator * alloc) {
  GDEFSTOR_Store * store = nullptr;
  GDEFSTOR_Result rc = ModelOpen(&store, alloc);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  GDEFSTOR_Write * w = nullptr;
  uint64_t v = 0;
  int found = 0;
  uint8_t * bytes = nullptr;
  size_t len = 0;
  rc = gdefstor_write_begin(store, kTenant, 2, nullptr, &w);
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_record_put(w, "core.m", 6, "a", 1,
        reinterpret_cast<const uint8_t *>("x"), 1);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_record_put(w, "core.m", 6, "b", 1,
          reinterpret_cast<const uint8_t *>("y"), 1);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_record_delete(w, "core.m", 6, "b", 1, nullptr);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_counter_next(w, "core.epoch", 10, &v);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_model_outbox_append(w, "core.bus", 8, "o1", 2, "f/1", 3,
          reinterpret_cast<const uint8_t *>("{}"), 2, nullptr);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_model_outbox_append(w, "core.bus", 8, "o2", 2, "f/1", 3,
          reinterpret_cast<const uint8_t *>("{}"), 2, nullptr);
    }
    GDEFSTOR_Result end = rc == GDEFSTOR_OK ? gdefstor_write_commit(w)
                                            : gdefstor_write_abort(w);
    rc = rc == GDEFSTOR_OK ? end : rc;
  }
  if (rc == GDEFSTOR_OK) {
    /* An aborted counter value, kept. */
    rc = gdefstor_write_begin(store, kTenant, 2, nullptr, &w);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_counter_next(w, "core.epoch", 10, &v);
      gdefstor_write_abort(w);
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_record_get(store, kTenant, 2, "core.m", 6, "a", 1, &found,
        &bytes, &len);
    gdefstor_bytes_free(alloc, bytes);
    bytes = nullptr;
  }
  if (rc == GDEFSTOR_OK) {
    GDEFSTOR_RecordList * l = nullptr;
    rc = gdefstor_record_list(store, kTenant, 2, "core.m", 6, "", 0, 0, 10, &l);
    gdefstor_record_list_free(l);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_counter_peek(store, kTenant, 2, "core.epoch", 10, &v);
  }
  if (rc == GDEFSTOR_OK) {
    GDEFSTOR_OutboxClaim * c = nullptr;
    rc = gdefstor_model_outbox_claim(store, kTenant, 2, "core.bus", 8, 10, 0, 2,
        &c);
    gdefstor_model_outbox_claim_free(c);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_outbox_failed(store, kTenant, 2, "o1", 2, 5, nullptr);
  }
  if (rc == GDEFSTOR_OK) {
    const char * ids[] = {"o2"};
    size_t lens[] = {2};
    rc = gdefstor_model_outbox_delivered(store, kTenant, 2, ids, lens, 1,
        nullptr);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_outbox_parked(store, kTenant, 2, "", 0, &v);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_dump(store, kTenant, 2, GDEFSTOR_MODEL_TRUTH, &bytes,
        &len);
    gdefstor_bytes_free(alloc, bytes);
    bytes = nullptr;
  }
  ModelClose(store);
  return rc;
}

TEST(ModelOom, EveryRecordCounterAndOutboxCallReportsAnAllocationFailure) {
  OomCount count;
  GCU_Allocator alloc;
  count.base = gcu_allocator_default();
  alloc.ctx = &count;
  alloc.malloc_fn = oom_malloc;
  alloc.calloc_fn = oom_calloc;
  alloc.realloc_fn = oom_realloc;
  alloc.free_fn = oom_free;
  ASSERT_EQ(GDEFSTOR_OK,
      SystemOomScenario(reinterpret_cast<const GDEFSTOR_Allocator *>(&alloc)));
  EXPECT_EQ(0, count.live);
  long budget = count.calls;
  ASSERT_GT(budget, 20);
  for (long i = 1; i <= budget; i++) {
    count = OomCount();
    count.base = gcu_allocator_default();
    count.fail_at = i;
    GDEFSTOR_Result rc =
        SystemOomScenario(reinterpret_cast<const GDEFSTOR_Allocator *>(&alloc));
    EXPECT_TRUE(rc == GDEFSTOR_ERR_OOM || rc == GDEFSTOR_OK)
        << "allocation " << i << ": " << gdefstor_result_string(rc);
    EXPECT_EQ(0, count.live) << "allocation " << i;
    EXPECT_EQ(0, count.underflow) << "allocation " << i;
    if (HasFailure()) {
      break;
    }
  }
}
