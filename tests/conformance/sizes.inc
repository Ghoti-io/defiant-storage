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
 * AD-34 at the entity port: a struct with a size member is accepted at
 * exactly its own size. Included by model.inc, so every backend runs it. The
 * structs it puts to the test are the ones listed for this file in
 * size_structs.inc; the rest are run by system.inc, keystore.inc and the
 * port's own test_model.cpp.
 *
 * A struct the caller passes in is given at each size and the call is
 * expected to accept it at its own and refuse it at any other; one the port
 * fills in is asked for at each size, and counts as accepted at the size it
 * sets. tools/check-size-structs.sh keeps the list level with the headers.
 */

TEST_F(ModelBlind, EveryStructOfThePortIsAcceptedAtOnlyItsOwnSize) {
  SetUpPo();
  int declared = 0;
  /* A second revision, so there is a diff; a state change, so there is one. */
  Node now = Current("po1");
  now.kids["lines"][1].Set("qty", Integer(5));
  {
    GDEFSTOR_Tree * t = Build(now);
    uint64_t rev = 0;
    ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t, &rev));
    gdefstor_tree_free(t);
    ASSERT_EQ(2u, rev);
  }
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    int changed = 0;
    return gdefstor_entity_set_state(w, "po1", 3, 1, 0, "ed", 2, 10, &changed);
  }));
  /* A member with a name that can be looked up blind, and by its subject. */
  Put(kMember, "m1", MemberTree("ann", "s1", "x"));
  const std::string mac = MacOf("ann");
  GDEFSTOR_Type * type = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(store, kTenant, 2, kPo,
      std::strlen(kPo), &type));
  GDEFSTOR_Tree * tree = Build(Current("po1"));
  std::string json;
  {
    uint8_t * j = nullptr;
    size_t len = 0;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_to_json(type, tree, &j, &len));
    json.assign(reinterpret_cast<const char *>(j), len);
    gdefstor_bytes_free(nullptr, j);
  }
  auto put_draft = [&](size_t size) {
    GDEFSTOR_DraftTree t;
    std::memset(&t, 0, sizeof t);
    t.size = size;
    t.language = kLang;
    t.language_len = kLangLen;
    t.json = reinterpret_cast<const uint8_t *>(json.data());
    t.json_len = json.size();
    return InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_draft_put(w, nullptr, 0, "po1", 3, Head("po1"), kStamp,
          kStampLen, &t, 1);
    }) == GDEFSTOR_OK;
  };
  ASSERT_TRUE(put_draft(sizeof(GDEFSTOR_DraftTree)));
  GDEFSTOR_Draft * draft = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_draft_get(store, kTenant, 2, "po1", 3, &draft));
  GDEFSTOR_History * history = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_history(store, kTenant, 2, "po1", 3,
      nullptr, 0, &history));
  ASSERT_GE(gdefstor_history_revision_count(history), 1u);
  ASSERT_GE(gdefstor_history_state_count(history), 1u);
  GDEFSTOR_Diff * diff = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_diff(store, kTenant, 2, "po1", 3,
      nullptr, 0, 1, 2, &diff));
  ASSERT_GE(gdefstor_diff_count(diff), 1u);
  GDEFSTOR_BlindHits * blind = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_blind_history(store, kTenant, 2, kMember,
      std::strlen(kMember), "name", 4,
      reinterpret_cast<const uint8_t *>(mac.data()), mac.size(), &blind));
  ASSERT_GE(gdefstor_blind_hits_count(blind), 1u);
  GDEFSTOR_SubjectHits * subject = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_subject_lookup(store, kTenant, 2, "s1", 2,
      &subject));
  ASSERT_GE(gdefstor_subject_hits_count(subject), 1u);
  GDEFSTOR_SubjectHit version;
  std::memset(&version, 0, sizeof version);
  version.size = sizeof version;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_subject_hit(subject, 0, &version));
  auto blind_mac = [&](size_t size) {
    GDEFSTOR_BlindMac m;
    std::memset(&m, 0, sizeof m);
    m.size = size;
    m.mac = reinterpret_cast<const uint8_t *>(mac.data());
    m.mac_len = mac.size();
    m.key_id = kMacKey;
    m.version = kMacVersion;
    return m;
  };
  const std::map<std::string, size_structs::Probe> probes = {
      {"GDEFSTOR_TypeSpec", [&](size_t size) {
         SpecHolder h("site.sized" + std::to_string(declared++),
             {F("title", GDEFSTOR_FIELD_TEXT)});
         h.spec.size = size;
         return InWrite([&](GDEFSTOR_Write * w) {
           return gdefstor_type_declare(w, &h.spec);
         }) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_FieldSpec", [&](size_t size) {
         /* The size on a field of the root, and on one inside a group. */
         SpecHolder top("site.sized" + std::to_string(declared++),
             {F("title", GDEFSTOR_FIELD_TEXT)});
         top.arrays[0][0].size = size;
         bool plain = InWrite([&](GDEFSTOR_Write * w) {
           return gdefstor_type_declare(w, &top.spec);
         }) == GDEFSTOR_OK;
         SpecHolder group("site.sized" + std::to_string(declared++),
             {G("lines", GDEFSTOR_FIELD_MULTIPLE,
                 {F("sku", GDEFSTOR_FIELD_TEXT)})});
         group.arrays[1][0].size = size;
         bool nested = InWrite([&](GDEFSTOR_Write * w) {
           return gdefstor_type_declare(w, &group.spec);
         }) == GDEFSTOR_OK;
         EXPECT_EQ(plain, nested) << "a nested field is read at another size";
         return plain;
       }},
      {"GDEFSTOR_FieldInfo", [&](size_t size) {
         GDEFSTOR_FieldInfo info;
         std::memset(&info, 0, sizeof info);
         info.size = size;
         return gdefstor_type_field(type, 0, &info) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_TreeItem", [&](size_t size) {
         GDEFSTOR_TreeItem item;
         std::memset(&item, 0, sizeof item);
         item.size = size;
         return gdefstor_tree_item(tree, 0, &item) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_BlindMac", [&](size_t size) {
         GDEFSTOR_BlindMac m = blind_mac(size);
         GDEFSTOR_Tree * t = nullptr;
         EXPECT_EQ(GDEFSTOR_OK, gdefstor_tree_create(nullptr, &t));
         GDEFSTOR_Value v = Personal(GDEFSTOR_FIELD_TEXT, "c", "s1");
         EXPECT_EQ(GDEFSTOR_OK, gdefstor_tree_set(t, 0, "name", 4, &v));
         bool in_tree =
             gdefstor_tree_set_blind(t, 0, "name", 4, &m) == GDEFSTOR_OK;
         gdefstor_tree_free(t);
         /* And the row put back for a version that exists. */
         bool put = InWrite([&](GDEFSTOR_Write * w) {
           return gdefstor_blind_put(w, version.type, version.type_len,
               version.public_id, version.public_id_len, version.field,
               version.field_len, version.item, version.valid_from,
               version.language, version.language_len, &m);
         }) == GDEFSTOR_OK;
         EXPECT_EQ(in_tree, put) << "the two calls read the MAC at different sizes";
         return in_tree;
       }},
      {"GDEFSTOR_EntityInfo", [&](size_t size) {
         GDEFSTOR_Entity * e = nullptr;
         EXPECT_EQ(GDEFSTOR_OK,
             gdefstor_entity_get(store, kTenant, 2, "po1", 3, &e));
         GDEFSTOR_EntityInfo info;
         std::memset(&info, 0, sizeof info);
         info.size = size;
         bool ok = gdefstor_entity_info(e, &info) == GDEFSTOR_OK;
         gdefstor_entity_free(e);
         return ok;
       }},
      {"GDEFSTOR_RevisionMeta", [&](size_t size) {
         GDEFSTOR_RevisionMeta m = Meta("u1", 9000);
         m.size = size;
         uint64_t got = 0;
         return InWrite([&](GDEFSTOR_Write * w) {
           return gdefstor_entity_save(w, "po1", 3, Head("po1"), &m, kLang,
               kLangLen, tree, &got);
         }) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_LanguageTree", [&](size_t size) {
         GDEFSTOR_RevisionMeta m = Meta("u1", 9000);
         GDEFSTOR_LanguageTree lt;
         std::memset(&lt, 0, sizeof lt);
         lt.size = size;
         lt.language = kLang;
         lt.language_len = kLangLen;
         lt.tree = tree;
         uint64_t got = 0;
         return InWrite([&](GDEFSTOR_Write * w) {
           return gdefstor_entity_save_languages(w, "po1", 3, Head("po1"), &m,
               &lt, 1, &got);
         }) == GDEFSTOR_OK;
       }},
      /* The port fills these: whether it filled the size asked about. */
      {"GDEFSTOR_RevisionInfo", [&](size_t size) {
         GDEFSTOR_RevisionInfo r;
         std::memset(&r, 0, sizeof r);
         r.size = size;
         return gdefstor_history_revision(history, 0, &r) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_StateInfo", [&](size_t size) {
         GDEFSTOR_StateInfo s;
         std::memset(&s, 0, sizeof s);
         s.size = size;
         return gdefstor_history_state(history, 0, &s) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_DiffItem", [&](size_t size) {
         GDEFSTOR_DiffItem d;
         std::memset(&d, 0, sizeof d);
         d.size = size;
         return gdefstor_diff_item(diff, 0, &d) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_DraftTree", [&](size_t size) {
         GDEFSTOR_DraftTree got;
         std::memset(&got, 0, sizeof got);
         got.size = size;
         bool read = gdefstor_draft_tree(draft, 0, &got) == GDEFSTOR_OK;
         /* And the same struct handed in with a draft. */
         EXPECT_EQ(read, put_draft(size))
             << "reading and putting a draft disagree about its size";
         return read;
       }},
      {"GDEFSTOR_DraftInfo", [&](size_t size) {
         GDEFSTOR_DraftInfo info;
         std::memset(&info, 0, sizeof info);
         info.size = size;
         return gdefstor_draft_info(draft, &info) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_BlindHit", [&](size_t size) {
         GDEFSTOR_BlindHit hit;
         std::memset(&hit, 0, sizeof hit);
         hit.size = size;
         return gdefstor_blind_hit(blind, 0, &hit) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_SubjectHit", [&](size_t size) {
         GDEFSTOR_SubjectHit hit;
         std::memset(&hit, 0, sizeof hit);
         hit.size = size;
         return gdefstor_subject_hit(subject, 0, &hit) == GDEFSTOR_OK;
       }},
      {"GDEFSTOR_Query", [&](size_t size) {
         GDEFSTOR_Query q;
         std::memset(&q, 0, sizeof q);
         q.size = size;
         q.language = kLang;
         q.language_len = kLangLen;
         GDEFSTOR_QueryResult * r = nullptr;
         GDEFSTOR_Result rc = gdefstor_entity_query(store, kTenant, 2, kPo,
             std::strlen(kPo), &q, &r);
         gdefstor_query_result_free(r);
         return rc == GDEFSTOR_OK;
       }},
  };
  size_structs::Run(size_structs::Storage(), "tests/conformance/sizes.inc",
      probes);
  gdefstor_subject_hits_free(subject);
  gdefstor_blind_hits_free(blind);
  gdefstor_diff_free(diff);
  gdefstor_history_free(history);
  gdefstor_draft_free(draft);
  gdefstor_tree_free(tree);
  gdefstor_type_free(type);
}
