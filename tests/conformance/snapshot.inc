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
 * The logical snapshot of the entity port's truth (story S6a3; snapshot.h,
 * AD-23): export a tenant, import it into an empty tenant of a second store
 * of the same backend, and require the truth back exactly. Included by
 * model.inc inside its namespace; the cross-backend round trips (memory,
 * SQLite, Postgres) use snapshot_fixture.inc from the backends' own tests.
 */

/* model.inc includes the archive headers and snapshot_fixture.inc before
 * its namespace. */

/* A snapshot's members, by name, in the order the archive holds them. */
inline std::vector<std::pair<std::string, std::string>> ZipMembers(
    const std::string & zip) {
  std::vector<std::pair<std::string, std::string>> out;
  GARC_Stream * st = nullptr;
  GARC_Archive * ar = nullptr;
  GARC_Limits lim;
  garc_limits_default(&lim);
  if (garc_stream_create_memory(zip.data(), zip.size(), &st) != GARC_OK) {
    ADD_FAILURE() << "not a zip";
    return out;
  }
  if (garc_open(st, &lim, &ar) != GARC_OK) {
    garc_stream_destroy(st);
    ADD_FAILURE() << "not a zip";
    return out;
  }
  for (;;) {
    const GARC_Member * m = nullptr;
    if (garc_next(ar, &m) != GARC_OK) {
      break;
    }
    std::string body(static_cast<size_t>(m->size), '\0');
    size_t got_total = 0;
    while (got_total < body.size()) {
      size_t got = 0;
      if (garc_read_member(ar, &body[got_total], body.size() - got_total,
              &got) != GARC_OK ||
          got == 0) {
        break;
      }
      got_total += got;
    }
    out.emplace_back(std::string(m->name, m->name_length), body);
  }
  garc_close(ar);
  garc_stream_destroy(st);
  return out;
}

inline std::string ZipOf(
    const std::vector<std::pair<std::string, std::string>> & members) {
  GARC_Sink * sink = nullptr;
  GARC_Writer * w = nullptr;
  GARC_Writer_Options opt;
  const void * data = nullptr;
  size_t size = 0;
  std::string out;
  if (garc_sink_create_memory(&sink) != GARC_OK) {
    return out;
  }
  garc_writer_options_default(&opt);
  if (garc_writer_create(sink, GARC_FORMAT_ZIP, &opt, &w) != GARC_OK) {
    garc_sink_destroy(sink);
    return out;
  }
  for (const auto & m : members) {
    GARC_Member gm;
    std::memset(&gm, 0, sizeof gm);
    gm.name = m.first.data();
    gm.name_length = m.first.size();
    gm.type = GARC_MEMBER_FILE;
    gm.size = m.second.size();
    if (garc_writer_add(w, &gm) != GARC_OK ||
        (!m.second.empty() &&
            garc_writer_write(w, m.second.data(), m.second.size()) !=
                GARC_OK)) {
      break;
    }
  }
  if (garc_writer_finish(w) == GARC_OK &&
      garc_sink_data(sink, &data, &size) == GARC_OK) {
    out.assign(static_cast<const char *>(data), size);
  }
  garc_writer_destroy(w);
  garc_sink_destroy(sink);
  return out;
}

inline size_t CountOf(const std::string & hay, const std::string & needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos;
       at = hay.find(needle, at + 1)) {
    n += 1;
  }
  return n;
}

/* A tree as its JSON, for comparing two stores' reads. */
inline std::string TreeText(GDEFSTOR_Store * s, const char * pid,
    const std::string & lang, uint64_t revision) {
  GDEFSTOR_Tree * t = nullptr;
  GDEFSTOR_Result rc = gdefstor_entity_tree(s, kTenant, 2, pid,
      std::strlen(pid), lang.data(), lang.size(), revision, &t);
  if (rc != GDEFSTOR_OK) {
    return std::string("<") + gdefstor_result_string(rc) + ">";
  }
  GDEFSTOR_Entity * e = nullptr;
  GDEFSTOR_EntityInfo info;
  std::memset(&info, 0, sizeof info);
  info.size = sizeof info;
  std::string type;
  if (gdefstor_entity_get(s, kTenant, 2, pid, std::strlen(pid), &e) ==
      GDEFSTOR_OK) {
    gdefstor_entity_info(e, &info);
    type.assign(info.type, info.type_len);
    gdefstor_entity_free(e);
  }
  GDEFSTOR_Type * ty = nullptr;
  std::string out = "<no type>";
  if (gdefstor_type_get(s, kTenant, 2, type.data(), type.size(), &ty) ==
      GDEFSTOR_OK) {
    uint8_t * json = nullptr;
    size_t len = 0;
    if (gdefstor_tree_to_json(ty, t, &json, &len) == GDEFSTOR_OK) {
      out.assign(reinterpret_cast<const char *>(json), len);
      gdefstor_bytes_free(nullptr, json);
    }
    gdefstor_type_free(ty);
  }
  gdefstor_tree_free(t);
  /* Order keys too: a tree's JSON carries ids and order, not keys. */
  return out;
}

inline std::vector<std::string> LanguagesAt(GDEFSTOR_Store * s,
    const char * pid, uint64_t revision) {
  std::vector<std::string> out;
  GDEFSTOR_Languages * l = nullptr;
  if (gdefstor_entity_languages(s, kTenant, 2, pid, std::strlen(pid), revision,
          &l) != GDEFSTOR_OK) {
    out.push_back("<failed>");
    return out;
  }
  for (size_t i = 0; i < gdefstor_languages_count(l); i++) {
    const char * p = nullptr;
    size_t n = 0;
    gdefstor_languages_get(l, i, &p, &n);
    out.emplace_back(p, n);
  }
  gdefstor_languages_free(l);
  return out;
}

inline std::string HistoryText(GDEFSTOR_Store * s, const char * pid) {
  GDEFSTOR_History * h = nullptr;
  if (gdefstor_entity_history(s, kTenant, 2, pid, std::strlen(pid), nullptr, 0,
          &h) != GDEFSTOR_OK) {
    return "<failed>";
  }
  std::ostringstream os;
  for (size_t i = 0; i < gdefstor_history_revision_count(h); i++) {
    GDEFSTOR_RevisionInfo r;
    std::memset(&r, 0, sizeof r);
    r.size = sizeof r;
    gdefstor_history_revision(h, i, &r);
    os << "r" << r.revision << " c" << r.commit_seq << " "
       << std::string(r.who, r.who_len) << " " << r.when_ms << " "
       << std::string(r.why, r.why_len) << " "
       << std::string(r.languages, r.languages_len) << "\n";
  }
  for (size_t i = 0; i < gdefstor_history_state_count(h); i++) {
    GDEFSTOR_StateInfo st;
    std::memset(&st, 0, sizeof st);
    st.size = sizeof st;
    gdefstor_history_state(h, i, &st);
    os << "s" << st.published << st.deleted << " "
       << std::string(st.actor, st.actor_len) << " " << st.when_ms << "\n";
  }
  gdefstor_history_free(h);
  return os.str();
}

inline std::string DiffText(GDEFSTOR_Store * s, const char * pid,
    uint64_t from, uint64_t to) {
  GDEFSTOR_Diff * d = nullptr;
  if (gdefstor_entity_diff(s, kTenant, 2, pid, std::strlen(pid), nullptr, 0,
          from, to, &d) != GDEFSTOR_OK) {
    return "<failed>";
  }
  std::ostringstream os;
  for (size_t i = 0; i < gdefstor_diff_count(d); i++) {
    GDEFSTOR_DiffItem it;
    std::memset(&it, 0, sizeof it);
    it.size = sizeof it;
    gdefstor_diff_item(d, i, &it);
    os << it.kind << "/" << it.what << " " << it.item << " "
       << std::string(it.field, it.field_len) << " "
       << std::string(it.language, it.language_len) << "\n";
  }
  gdefstor_diff_free(d);
  return os.str();
}

class ModelSnapshot : public ModelTest {
 public:
  GDEFSTOR_Store * copy = nullptr;

  void SetUp() override {
    ModelTest::SetUp();
    ASSERT_EQ(GDEFSTOR_OK, ModelOpen(&copy, nullptr));
  }
  void TearDown() override {
    ModelClose(copy);
    ModelTest::TearDown();
  }

  GDEFSTOR_Result InWriteOn(GDEFSTOR_Store * s,
      const std::function<GDEFSTOR_Result(GDEFSTOR_Write *)> & body) {
    return snapshot_fixture::InWrite(s, kTenant, body);
  }

  /* The fixture in `store`, exported, and imported into `copy`. */
  std::string RoundTrip(bool mac = false) {
    EXPECT_EQ(GDEFSTOR_OK, snapshot_fixture::Populate(store, kTenant, mac));
    std::string zip = snapshot_fixture::Export(store, kTenant);
    std::string why;
    EXPECT_EQ(GDEFSTOR_OK, snapshot_fixture::Import(copy, kTenant, zip, &why))
        << why;
    return zip;
  }
};

/* Every truth row comes back as it was, and the current tables a live store
 * holds are rebuilt to the same rows. */
TEST_F(ModelSnapshot, ARoundTripKeepsEveryTruthRow) {
  RoundTrip();
  std::string truth = snapshot_fixture::Dump(store, kTenant, GDEFSTOR_MODEL_TRUTH);
  EXPECT_FALSE(Lines(truth, 'I').empty());
  EXPECT_FALSE(Lines(truth, 'K').empty());
  EXPECT_FALSE(Lines(truth, 'C').empty());
  EXPECT_FALSE(Lines(truth, 'O').empty());
  EXPECT_FALSE(Lines(truth, 'D').empty());
  EXPECT_EQ(truth, snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_TRUTH));
  EXPECT_EQ(snapshot_fixture::Dump(store, kTenant, GDEFSTOR_MODEL_CURRENT),
      snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_CURRENT));
}

/* The same truth gives the same bytes: twice from one store, and once more
 * from the store it was imported into. */
TEST_F(ModelSnapshot, ExportingTheSameTruthGivesTheSameBytes) {
  std::string zip = RoundTrip();
  ASSERT_FALSE(zip.empty());
  EXPECT_EQ(zip, snapshot_fixture::Export(store, kTenant));
  EXPECT_EQ(zip, snapshot_fixture::Export(copy, kTenant));
  /* And the taken time is the one asked for. */
  int found = 0;
  int64_t taken = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_snapshot_taken(copy,
      reinterpret_cast<const uint8_t *>(zip.data()), zip.size(), nullptr,
      &found, &taken));
  EXPECT_EQ(1, found);
  EXPECT_EQ(1234, taken);
}

/* Every revision of every language reads back as of itself, and the
 * history and every step's difference are unchanged. */
TEST_F(ModelSnapshot, EveryRevisionReadsBackAsOfAfterImport) {
  RoundTrip();
  const char * pids[] = {"o1", "o2", "c1", "c2"};
  size_t reads = 0;
  for (const char * pid : pids) {
    EXPECT_EQ(HistoryText(store, pid), HistoryText(copy, pid)) << pid;
    uint64_t head = 0;
    {
      GDEFSTOR_Entity * e = nullptr;
      GDEFSTOR_EntityInfo info;
      std::memset(&info, 0, sizeof info);
      info.size = sizeof info;
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_get(store, kTenant, 2, pid,
          std::strlen(pid), &e));
      gdefstor_entity_info(e, &info);
      head = info.revision;
      gdefstor_entity_free(e);
    }
    for (uint64_t r = 1; r <= head; r++) {
      EXPECT_EQ(DiffText(store, pid, r - 1, r), DiffText(copy, pid, r - 1, r))
          << pid << " " << r;
      for (const std::string & lang : LanguagesAt(store, pid, r)) {
        EXPECT_EQ(TreeText(store, pid, lang, r), TreeText(copy, pid, lang, r))
            << pid << " " << lang << " as of " << r;
        reads += 1;
      }
    }
  }
  EXPECT_GE(reads, 12u);
}

/* Each language's trees come back, in every revision that had them, and a
 * query bound to a language answers alike. */
TEST_F(ModelSnapshot, EveryLanguageComesBack) {
  RoundTrip();
  std::set<std::string> seen;
  const char * pids[] = {"o1", "o2", "c1"};
  for (const char * pid : pids) {
    for (uint64_t r = 0; r <= 4; r++) {
      std::vector<std::string> a = LanguagesAt(store, pid, r);
      EXPECT_EQ(a, LanguagesAt(copy, pid, r)) << pid << " as of " << r;
      for (const std::string & l : a) {
        seen.insert(l);
      }
    }
  }
  EXPECT_EQ((std::set<std::string>{"<failed>", "de", "en", "fr"}), seen);
  /* Bound to a language, a query reads that language's imported items. */
  for (const char * lang : {"en", "fr", "de"}) {
    Q q;
    const GDEFSTOR_QueryNode * w = q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("number"),
        q.Values({snapshot_fixture::Txt("A-1")}));
    Answer a = Ask(store, "shop.order", w, {By(q.Field("number"))}, 0, 0, lang);
    EXPECT_EQ(GDEFSTOR_OK, a.rc) << lang;
    EXPECT_EQ(a, Ask(copy, "shop.order", w, {By(q.Field("number"))}, 0, 0,
                     lang))
        << lang;
    Answer all = Ask(copy, "shop.cust", nullptr, {By(q.Field("name"))}, 0, 0,
        lang);
    EXPECT_EQ(Ask(store, "shop.cust", nullptr, {By(q.Field("name"))}, 0, 0,
                  lang),
        all)
        << lang;
  }
  /* The French tree of o1 at revision 3, word for word. */
  EXPECT_EQ(TreeText(store, "o1", "fr", 3), TreeText(copy, "o1", "fr", 3));
  EXPECT_NE(std::string::npos, TreeText(copy, "o1", "fr", 3).find("A-1-fr"));
}

/* Records, counters and the outbox come back: the next counter value
 * continues from the exported one, outbox rows keep their sequences, leases,
 * failures and parked flags, and a stream's next append continues. */
TEST_F(ModelSnapshot, CountersContinueAndOutboxRowsKeepTheirPlace) {
  RoundTrip();
  uint64_t v = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_counter_peek(copy, kTenant, 2, "core.epoch",
      10, &v));
  EXPECT_EQ(3u, v);
  ASSERT_EQ(GDEFSTOR_OK, InWriteOn(copy, [&](GDEFSTOR_Write * w) {
    return gdefstor_counter_next(w, "core.epoch", 10, &v);
  }));
  EXPECT_EQ(4u, v);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_counter_peek(copy, kTenant, 2, "core.next",
      9, &v));
  EXPECT_EQ(1u, v);
  int found = 0;
  uint8_t * bytes = nullptr;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_get(copy, kTenant, 2, "core.name", 9,
      "x:1", 3, &found, &bytes, &len));
  EXPECT_EQ(1, found);
  EXPECT_EQ(std::string("\x00\xff\x10\x80", 4),
      std::string(reinterpret_cast<const char *>(bytes), len));
  gdefstor_bytes_free(nullptr, bytes);
  uint64_t parked = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_outbox_parked(copy, kTenant, 2,
      "core.bus", 8, &parked));
  EXPECT_EQ(1u, parked);
  /* p1's lease (until 150) still holds at 140: nothing is claimed. */
  GDEFSTOR_OutboxClaim * c = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_outbox_claim(copy, kTenant, 2,
      "core.bus", 8, 50, 140, 10, &c));
  EXPECT_EQ(0u, gdefstor_model_outbox_claim_count(c));
  gdefstor_model_outbox_claim_free(c);
  /* After it, p1 and p2 in sequence, p2 with its failure; never p3. */
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_outbox_claim(copy, kTenant, 2,
      "core.bus", 8, 50, 200, 10, &c));
  ASSERT_EQ(2u, gdefstor_model_outbox_claim_count(c));
  for (size_t i = 0; i < 2; i++) {
    GDEFSTOR_OutboxEntry e;
    std::memset(&e, 0, sizeof e);
    e.size = sizeof e;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_outbox_claim_entry(c, i, &e));
    EXPECT_EQ(i + 1, e.seq);
    EXPECT_EQ(i == 0 ? "p1" : "p2", std::string(e.public_id, e.public_id_len));
    EXPECT_EQ(i == 0 ? 0u : 1u, e.failures);
  }
  gdefstor_model_outbox_claim_free(c);
  uint64_t seq = 0;
  ASSERT_EQ(GDEFSTOR_OK, InWriteOn(copy, [&](GDEFSTOR_Write * w) {
    return gdefstor_model_outbox_append(w, "core.bus", 8, "p4", 2, "x/1", 3,
        nullptr, 0, &seq);
  }));
  EXPECT_EQ(4u, seq);
}

/* A snapshot holds the truth and nothing derived: as many item versions and
 * entities as the truth has, no blind row and no MAC; the import holds no
 * blind row either, and the rest of the truth is the source's. */
TEST_F(ModelSnapshot, HoldsTheTruthAndNoDerivedRow) {
  std::string zip = RoundTrip(true);
  std::string truth = snapshot_fixture::Dump(store, kTenant, GDEFSTOR_MODEL_TRUTH);
  ASSERT_FALSE(Lines(truth, 'B').empty());
  size_t items = 0;
  size_t entities = 0;
  std::string mac(reinterpret_cast<const char *>(snapshot_fixture::MacBytes()),
      GDEFSTOR_BLIND_MAC_BYTES);
  std::string mac_hex;
  for (char ch : mac) {
    static const char hex[] = "0123456789abcdef";
    mac_hex += hex[(static_cast<unsigned char>(ch) >> 4) & 0xF];
    mac_hex += hex[static_cast<unsigned char>(ch) & 0xF];
  }
  for (const auto & m : ZipMembers(zip)) {
    items += CountOf(m.second, "{\"lang\":");
    entities += CountOf(m.second, "\"last_item\":");
    EXPECT_EQ(std::string::npos, m.second.find(mac)) << m.first;
    EXPECT_EQ(std::string::npos, m.second.find(mac_hex)) << m.first;
    EXPECT_EQ(std::string::npos, m.second.find("blind")) << m.first;
  }
  EXPECT_EQ(Lines(truth, 'I').size(), items);
  EXPECT_EQ(Lines(truth, 'E').size(), entities);
  std::string back = snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_TRUTH);
  EXPECT_TRUE(Lines(back, 'B').empty());
  std::string no_blind;
  {
    std::istringstream in(truth);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line[0] != 'B') {
        no_blind += line + "\n";
      }
    }
  }
  EXPECT_EQ(no_blind, back);
  EXPECT_EQ(snapshot_fixture::Dump(store, kTenant, GDEFSTOR_MODEL_CURRENT),
      snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_CURRENT));
}

/* Only into an empty tenant: one with any entity truth at all - here, a
 * declaration alone, or the import itself - is refused in words, and keeps
 * what it had. */
TEST_F(ModelSnapshot, ImportIntoANonEmptyTenantIsRefusedInWords) {
  std::string zip = RoundTrip();
  std::string before = snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_TRUTH);
  std::string why;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      snapshot_fixture::Import(copy, kTenant, zip, &why));
  EXPECT_NE(std::string::npos, why.find("empty tenant")) << why;
  EXPECT_EQ(before, snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_TRUTH));
  GDEFSTOR_Store * third = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, ModelOpen(&third, nullptr));
  ASSERT_EQ(GDEFSTOR_OK, snapshot_fixture::InWrite(third, kTenant,
      [](GDEFSTOR_Write * w) { return snapshot_fixture::DeclareCust(w, true); }));
  std::string one = snapshot_fixture::Dump(third, kTenant, GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      snapshot_fixture::Import(third, kTenant, zip, &why));
  EXPECT_NE(std::string::npos, why.find("empty tenant")) << why;
  EXPECT_EQ(one, snapshot_fixture::Dump(third, kTenant, GDEFSTOR_MODEL_TRUTH));
  ModelClose(third);
}

/* A newer format is refused in words naming both versions, and so is a
 * member that is not what the format holds; nothing is written. */
TEST_F(ModelSnapshot, ANewerFormatOrABadMemberIsRefusedInWords) {
  ASSERT_EQ(GDEFSTOR_OK, snapshot_fixture::Populate(store, kTenant, false));
  std::string zip = snapshot_fixture::Export(store, kTenant);
  auto members = ZipMembers(zip);
  ASSERT_FALSE(members.empty());
  ASSERT_EQ("manifest", members[0].first);
  auto newer = members;
  size_t at = newer[0].second.find("defiant-snapshot/2");
  ASSERT_NE(std::string::npos, at);
  newer[0].second.replace(at, 18, "defiant-snapshot/3");
  std::string why;
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED,
      snapshot_fixture::Import(copy, kTenant, ZipOf(newer), &why));
  EXPECT_NE(std::string::npos, why.find("defiant-snapshot/3")) << why;
  EXPECT_NE(std::string::npos, why.find("defiant-snapshot/2")) << why;
  EXPECT_EQ("", snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_TRUTH));
  /* An item version moved to a revision the entity does not have. */
  auto bad = members;
  bool changed = false;
  for (auto & m : bad) {
    size_t f = m.second.find("\"from\":1,");
    if (m.first.rfind("model/", 0) == 0 && f != std::string::npos) {
      m.second.replace(f, 9, "\"from\":9,");
      changed = true;
      break;
    }
  }
  ASSERT_TRUE(changed);
  EXPECT_EQ(GDEFSTOR_ERR_CORRUPT,
      snapshot_fixture::Import(copy, kTenant, ZipOf(bad), &why));
  EXPECT_FALSE(why.empty());
  EXPECT_EQ("", snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_TRUTH));
  /* The untouched archive still imports. */
  EXPECT_EQ(GDEFSTOR_OK,
      snapshot_fixture::Import(copy, kTenant, ZipOf(members), &why))
      << why;
  EXPECT_EQ(snapshot_fixture::Dump(store, kTenant, GDEFSTOR_MODEL_TRUTH),
      snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_TRUTH));
}

/* A tenant with no entity truth exports no entity member, and a snapshot of
 * one imports into an empty tenant as nothing. */
TEST_F(ModelSnapshot, AnEmptyTenantHasNoEntityPart) {
  std::string zip = snapshot_fixture::Export(store, kTenant);
  auto members = ZipMembers(zip);
  ASSERT_EQ(1u, members.size());
  EXPECT_NE(std::string::npos, members[0].second.find("\"model\":[]"));
  EXPECT_EQ(GDEFSTOR_OK, snapshot_fixture::Import(copy, kTenant, zip));
  EXPECT_EQ("", snapshot_fixture::Dump(copy, kTenant, GDEFSTOR_MODEL_TRUTH));
}

/* Every allocation an export or an import makes may fail: each failure is
 * reported (::GDEFSTOR_ERR_OOM), an export that failed returns nothing, and
 * an import that failed writes nothing - the next attempt would be refused
 * as an import into a tenant that is not empty if it had - so the attempt
 * after the last failure imports the whole snapshot. Leaks are the
 * sanitizer arm's to find. (A tenant whose first write failed may not be
 * readable until a write succeeds on some backends, so the tenant is not
 * dumped between attempts.) */
TEST(ModelSnapshotOom, EveryAllocationFailureIsReportedAndWritesNothing) {
  OomCount count;
  GCU_Allocator alloc;
  count.base = gcu_allocator_default();
  alloc.ctx = &count;
  alloc.malloc_fn = oom_malloc;
  alloc.calloc_fn = oom_calloc;
  alloc.realloc_fn = oom_realloc;
  alloc.free_fn = oom_free;
  const GDEFSTOR_Allocator * a =
      reinterpret_cast<const GDEFSTOR_Allocator *>(&alloc);
  GDEFSTOR_Store * src = nullptr;
  GDEFSTOR_Store * dst = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, ModelOpen(&src, a));
  ASSERT_EQ(GDEFSTOR_OK, snapshot_fixture::Populate(src, kTenant, true));
  std::string want = snapshot_fixture::Export(src, kTenant);
  ASSERT_FALSE(want.empty());
  auto import = [&](GDEFSTOR_Store * into) {
    char * why = nullptr;
    size_t why_len = 0;
    GDEFSTOR_Result rc = gdefstor_snapshot_import_explain(into, kTenant, 2,
        reinterpret_cast<const uint8_t *>(want.data()), want.size(), nullptr,
        &why, &why_len);
    gdefstor_bytes_free(a, why);
    return rc;
  };
  /* How many allocations each makes when none fails. A backend may get
   * past a failed allocation it can do without (a cache entry), so the
   * whole budget is walked, not only up to the first success. */
  long base = count.calls;
  {
    uint8_t * zip = nullptr;
    size_t len = 0;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_snapshot_export_at(src, kTenant, 2,
        nullptr, 1234, &zip, &len));
    gdefstor_bytes_free(a, zip);
  }
  long export_budget = count.calls - base;
  ASSERT_EQ(GDEFSTOR_OK, ModelOpen(&dst, a));
  base = count.calls;
  ASSERT_EQ(GDEFSTOR_OK, import(dst));
  long import_budget = count.calls - base;
  ModelClose(dst);
  long exports = 0;
  for (long i = 1; i <= export_budget; i++) {
    uint8_t * zip = nullptr;
    size_t len = 0;
    count.fail_at = count.calls + i;
    GDEFSTOR_Result rc = gdefstor_snapshot_export_at(src, kTenant, 2, nullptr,
        1234, &zip, &len);
    count.fail_at = 0;
    if (rc == GDEFSTOR_OK) {
      EXPECT_EQ(want, std::string(reinterpret_cast<const char *>(zip), len))
          << "export, allocation " << i;
      gdefstor_bytes_free(a, zip);
      continue;
    }
    exports += 1;
    ASSERT_EQ(GDEFSTOR_ERR_OOM, rc) << "export, allocation " << i << ": "
                                    << gdefstor_result_string(rc);
    ASSERT_EQ(nullptr, zip);
  }
  /* An import that failed wrote nothing: the next attempt into the same
   * tenant would be refused as one into a tenant that is not empty if it
   * had. One that got past its failure is a whole import; the walk goes on
   * in a new store. (A tenant whose first write failed may not be readable
   * until a write succeeds on some backends, so it is not dumped between
   * attempts.) */
  long imports = 0;
  ASSERT_EQ(GDEFSTOR_OK, ModelOpen(&dst, a));
  for (long i = 1; i <= import_budget; i++) {
    count.fail_at = count.calls + i;
    GDEFSTOR_Result rc = import(dst);
    count.fail_at = 0;
    if (rc == GDEFSTOR_OK) {
      ModelClose(dst);
      dst = nullptr;
      ASSERT_EQ(GDEFSTOR_OK, ModelOpen(&dst, a));
      continue;
    }
    imports += 1;
    ASSERT_EQ(GDEFSTOR_ERR_OOM, rc) << "import, allocation " << i << ": "
                                    << gdefstor_result_string(rc);
  }
  ASSERT_EQ(GDEFSTOR_OK, import(dst));
  std::printf("snapshot allocation failures: %ld of %ld in export, %ld of "
              "%ld in import\n",
      exports, export_budget, imports, import_budget);
  EXPECT_GT(exports, 100);
  EXPECT_GT(imports, 100);
  std::string truth = snapshot_fixture::Dump(src, kTenant, GDEFSTOR_MODEL_TRUTH);
  EXPECT_FALSE(Lines(truth, 'B').empty());
  std::string want_back;
  {
    std::istringstream in(truth);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line[0] != 'B') {
        want_back += line + "\n";
      }
    }
  }
  EXPECT_EQ(want_back,
      snapshot_fixture::Dump(dst, kTenant, GDEFSTOR_MODEL_TRUTH));
  ModelClose(dst);
  ModelClose(src);
}
