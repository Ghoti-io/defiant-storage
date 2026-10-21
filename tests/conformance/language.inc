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
 * The language axis of the entity port (model.h, "Languages"; story S6a2):
 * one item tree per language, one revision history for all of them, and a
 * query that reads one language. Included by model.inc, inside its namespace,
 * so every backend runs it. The rules stated here are the design's
 * (notes/defiant-storage/ENTITY-STORAGE-DESIGN.md, "Languages"), and the
 * sweep at the end checks seeded multilingual histories against a model
 * written apart from the library.
 */

const char * const kArticle = "site.article";

inline std::vector<Field> ArticleType() {
  return {F("title", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_INDEXED),
      F("rank", GDEFSTOR_FIELD_INTEGER, GDEFSTOR_FIELD_INDEXED),
      F("tags", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_MULTIPLE),
      G("parts", GDEFSTOR_FIELD_MULTIPLE,
          {F("heading", GDEFSTOR_FIELD_TEXT), F("words", GDEFSTOR_FIELD_INTEGER)})};
}

inline Node Article(const char * title, int64_t rank,
    std::vector<const char *> tags = {}) {
  Node n;
  n.Set("title", Text(title)).Set("rank", Integer(rank));
  for (const char * t : tags) {
    n.Add("tags", One(Text(t)));
  }
  return n;
}

class ModelLanguage : public ModelSave {
 protected:
  void SetUp() override {
    ModelSave::SetUp();
    ASSERT_EQ(GDEFSTOR_OK, Declare(kArticle, ArticleType()));
  }

  /* Save every (language, tree) in one revision: a null tree removes. */
  GDEFSTOR_Result SaveLangs(const std::string & pid, uint64_t base,
      const std::vector<std::pair<std::string, const Node *>> & langs,
      uint64_t * out = nullptr, const char * why = "") {
    std::vector<GDEFSTOR_Tree *> trees;
    std::vector<GDEFSTOR_LanguageTree> lt;
    for (const auto & l : langs) {
      GDEFSTOR_LanguageTree x;
      std::memset(&x, 0, sizeof x);
      x.size = sizeof x;
      x.language = l.first.data();
      x.language_len = l.first.size();
      x.tree = l.second == nullptr ? nullptr : Build(*l.second);
      trees.push_back(const_cast<GDEFSTOR_Tree *>(x.tree));
      lt.push_back(x);
    }
    uint64_t got = 0;
    GDEFSTOR_RevisionMeta m = Meta("u1", 5000 + static_cast<int64_t>(base),
        why);
    GDEFSTOR_Result rc = InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_save_languages(w, pid.data(), pid.size(), base,
          &m, lt.empty() ? nullptr : lt.data(), lt.size(), &got);
    });
    for (GDEFSTOR_Tree * t : trees) {
      gdefstor_tree_free(t);
    }
    if (out != nullptr) {
      *out = got;
    }
    return rc;
  }

  GDEFSTOR_Result Remove(const std::string & pid, uint64_t base,
      const std::string & lang, uint64_t * out = nullptr) {
    uint64_t got = 0;
    GDEFSTOR_RevisionMeta m = Meta("u1", 6000, "remove");
    GDEFSTOR_Result rc = InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_remove_language(w, pid.data(), pid.size(), base,
          &m, lang.data(), lang.size(), &got);
    });
    if (out != nullptr) {
      *out = got;
    }
    return rc;
  }

  /* The tree of @p lang at @p rev (0: current), or rc on failure. */
  GDEFSTOR_Result TreeIn(const std::string & pid, const std::string & lang,
      uint64_t rev, Node * out) {
    GDEFSTOR_Tree * t = nullptr;
    GDEFSTOR_Result rc = gdefstor_entity_tree(store, kTenant, 2, pid.data(),
        pid.size(), lang.data(), lang.size(), rev, &t);
    if (rc == GDEFSTOR_OK) {
      *out = Read(t);
    }
    gdefstor_tree_free(t);
    return rc;
  }

  Node In(const std::string & pid, const std::string & lang, uint64_t rev = 0) {
    Node n;
    EXPECT_EQ(GDEFSTOR_OK, TreeIn(pid, lang, rev, &n)) << pid << " " << lang;
    return n;
  }

  std::vector<std::string> Langs(const std::string & pid, uint64_t rev = 0) {
    std::vector<std::string> out;
    GDEFSTOR_Languages * l = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_languages(store, kTenant, 2,
        pid.data(), pid.size(), rev, &l));
    for (size_t i = 0; i < gdefstor_languages_count(l); i++) {
      const char * s = nullptr;
      size_t n = 0;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_languages_get(l, i, &s, &n));
      out.push_back(std::string(s, n));
    }
    gdefstor_languages_free(l);
    return out;
  }

  /* "revision:languages" of each revision, optionally of one language. */
  std::vector<std::string> RevLangs(const std::string & pid,
      const char * lang = nullptr) {
    std::vector<std::string> out;
    GDEFSTOR_History * h = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_history(store, kTenant, 2,
        pid.data(), pid.size(), lang, lang == nullptr ? 0 : std::strlen(lang),
        &h));
    for (size_t i = 0; i < gdefstor_history_revision_count(h); i++) {
      GDEFSTOR_RevisionInfo r;
      std::memset(&r, 0, sizeof r);
      r.size = sizeof r;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_history_revision(h, i, &r));
      out.push_back(std::to_string(r.revision) + ":" +
          std::string(r.languages, r.languages_len));
    }
    gdefstor_history_free(h);
    return out;
  }

  /* The languages of the items the dump's new "I" rows are in. */
  std::set<std::string> NewRowLangs(const std::string & before,
      const std::string & after) {
    std::set<std::string> was;
    std::set<std::string> out;
    for (const std::string & l : Lines(before, 'I')) {
      was.insert(l);
    }
    for (const std::string & l : Lines(after, 'I')) {
      if (was.count(l) == 0) {
        out.insert(Words(l).back());
      }
    }
    return out;
  }

  Answer AskIn(const char * lang, const GDEFSTOR_QueryNode * where,
      std::vector<GDEFSTOR_QuerySort> sorts = {}) {
    return Ask(store, kArticle, where, sorts, 0, 0, lang);
  }

  /* a1: en "Hello" / fr "Bonjour"; a2: en "Bonjour" / fr "Salut"; a3: en
   * "Hello" only. */
  void ThreeArticles() {
    Node en1 = Article("Hello", 3, {"news"});
    Node fr1 = Article("Bonjour", 1, {"nouvelles"});
    Node en2 = Article("Bonjour", 1);
    Node fr2 = Article("Salut", 2);
    Node en3 = Article("Hello", 2);
    ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a1"));
    ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 0, {{"en", &en1}, {"fr", &fr1}}));
    ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a2"));
    ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a2", 0, {{"en", &en2}, {"fr", &fr2}}));
    ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a3"));
    ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a3", 0, {{"en", &en3}}));
  }
};

TEST_F(ModelLanguage, TwoLanguagesAreOneRevision) {
  ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a1"));
  Node en = Article("Hello", 1, {"news", "world"});
  en.Add("parts").Set("heading", Text("Intro")).Set("words", Integer(40));
  Node fr = Article("Bonjour", 1, {"nouvelles"});
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 0, {{"en", &en}, {"fr", &fr}}, &rev));
  EXPECT_EQ(1u, rev);
  std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
  /* One revision row, for both languages. */
  EXPECT_EQ(1u, NewRows(before, after, 'R'));
  EXPECT_EQ((std::vector<std::string>{"1:en,fr"}), RevLangs("a1"));
  EXPECT_EQ(NoIds(en), NoIds(In("a1", "en")));
  EXPECT_EQ(NoIds(fr), NoIds(In("a1", "fr")));
  EXPECT_EQ((std::vector<std::string>{"en", "fr"}), Langs("a1"));
  EXPECT_EQ((std::vector<std::string>{"en", "fr"}), Langs("a1", 1));
  /* Each language has its own root, and ids are the entity's: none shared. */
  Node a = In("a1", "en");
  Node b = In("a1", "fr");
  EXPECT_NE(a.id, b.id);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, TreeIn("a1", "de", 0, &a));
  /* The rows name their language. */
  EXPECT_EQ((std::set<std::string>{HexOf("en"), HexOf("fr")}),
      NewRowLangs(before, after));
}

TEST_F(ModelLanguage, ChangingOneLanguageWritesOnlyItsItems) {
  ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a1"));
  Node en = Article("Hello", 1, {"news", "world"});
  Node fr = Article("Bonjour", 1, {"nouvelles"});
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 0, {{"en", &en}, {"fr", &fr}}));
  /* Both named, en restated exactly (with its ids), fr changed. */
  Node en_now = In("a1", "en");
  Node fr_now = In("a1", "fr");
  fr_now.Set("title", Text("Salut"));
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 1, {{"en", &en_now}, {"fr", &fr_now}},
      &rev));
  EXPECT_EQ(2u, rev);
  std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ("", AppendOnly(before, after));
  /* Only fr's root changed: one fr version, no en row at all. */
  EXPECT_EQ(1u, NewRows(before, after, 'I'));
  EXPECT_EQ((std::set<std::string>{HexOf("fr")}), NewRowLangs(before, after));
  EXPECT_EQ((std::vector<std::string>{"1:en,fr", "2:fr"}), RevLangs("a1"));
  for (const std::string & l : Lines(before, 'I')) {
    if (Words(l).back() == HexOf("en")) {
      /* Not closed, not rewritten: the same line, still open. */
      EXPECT_NE(std::string::npos, after.find(l + "\n")) << l;
      EXPECT_EQ("0", Words(l)[5]) << l;
    }
  }
  /* A language left out keeps its tree and gets no row either. */
  Node fr_again = In("a1", "fr");
  fr_again.Set("rank", Integer(9));
  before = after;
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 2, {{"fr", &fr_again}}, &rev));
  EXPECT_EQ(3u, rev);
  after = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ((std::set<std::string>{HexOf("fr")}), NewRowLangs(before, after));
  EXPECT_EQ(NoIds(en), NoIds(In("a1", "en")));
  /* Every language unchanged: no revision. */
  Node en3 = In("a1", "en");
  Node fr3 = In("a1", "fr");
  before = after;
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 3, {{"en", &en3}, {"fr", &fr3}},
      &rev));
  EXPECT_EQ(0u, rev);
  EXPECT_EQ(before, Dump(GDEFSTOR_MODEL_TRUTH));
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 3, {}, &rev));
  EXPECT_EQ(0u, rev);
  EXPECT_EQ(before, Dump(GDEFSTOR_MODEL_TRUTH));
  EXPECT_EQ(3u, Head("a1"));
}

TEST_F(ModelLanguage, AQueryReadsOnlyItsLanguage) {
  ThreeArticles();
  Q q;
  const GDEFSTOR_QueryNode * bonjour = q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("title"), q.Values({Text("Bonjour")}));
  EXPECT_EQ(Ids({"a1"}, 1), AskIn("fr", bonjour));
  EXPECT_EQ(Ids({"a2"}, 1), AskIn("en", bonjour));
  /* Two conditions on one entity are about the same language: a1 is
   * "Hello" in en and "Bonjour" in fr, never both in one. */
  const GDEFSTOR_QueryNode * both = q.Op(GDEFSTOR_QUERY_AND, {bonjour,
      q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("title"), q.Values({Text("Hello")}))});
  EXPECT_EQ(Ids({}, 0), AskIn("fr", both));
  EXPECT_EQ(Ids({}, 0), AskIn("en", both));
  /* Multiples and ranges, in the language. */
  EXPECT_EQ(Ids({"a1"}, 1), AskIn("fr", q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("tags"), q.Values({Text("nouvelles")}))));
  EXPECT_EQ(Ids({}, 0), AskIn("en", q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("tags"), q.Values({Text("nouvelles")}))));
  EXPECT_EQ(Ids({"a2"}, 1), AskIn("fr", q.Cmp(GDEFSTOR_QUERY_GE,
      q.Field("rank"), q.Values({Integer(2)}))));
  EXPECT_EQ(Ids({"a1", "a3"}, 2), AskIn("en", q.Cmp(GDEFSTOR_QUERY_GE,
      q.Field("rank"), q.Values({Integer(2)}))));
  /* A sort in fr orders by the fr values: Bonjour, Salut, then a3 (none). */
  EXPECT_EQ(Ids({"a3", "a1", "a2"}, 3), AskIn("fr", nullptr,
      {By(q.Field("title"))}));
  EXPECT_EQ(Ids({"a3", "a1", "a2"}, 3), AskIn("fr", nullptr,
      {By(q.Field("rank")), By(q.Field("title"))}));
  EXPECT_EQ(Ids({"a2", "a3", "a1"}, 3), AskIn("en", nullptr,
      {By(q.Field("rank"))}));
  /* An EXISTS reads the same language, the outer field too: an article
   * whose fr title is another's fr title (none), and en (a1 and a3 share
   * "Hello"). */
  const GDEFSTOR_QueryNode * twin = q.Op(GDEFSTOR_QUERY_EXISTS,
      {q.Op(GDEFSTOR_QUERY_AND, {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("title"),
           q.Field("title", true)), q.Cmp(GDEFSTOR_QUERY_NE, q.Entity(),
           q.Entity(true))})},
      kArticle);
  EXPECT_EQ(Ids({}, 0), AskIn("fr", twin));
  EXPECT_EQ(Ids({"a1", "a3"}, 2), AskIn("en", twin));
  /* And the count agrees. */
  uint64_t n = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(store, kTenant, 2, kArticle,
      std::strlen(kArticle), "fr", 2, bonjour, &n));
  EXPECT_EQ(1u, n);
}

TEST_F(ModelLanguage, AnEntityWithNoTreeInTheLanguageMatchesNoField) {
  ThreeArticles();
  Q q;
  /* a3 is "Hello" in en and has no fr tree. */
  const GDEFSTOR_QueryNode * hello = q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("title"),
      q.Values({Text("Hello")}));
  EXPECT_EQ(Ids({"a1", "a3"}, 2), AskIn("en", hello));
  EXPECT_EQ(Ids({}, 0), AskIn("fr", hello));
  EXPECT_EQ(Ids({}, 0), AskIn("de", hello));
  EXPECT_EQ(Ids({}, 0), AskIn("de", q.Cmp(GDEFSTOR_QUERY_GE, q.Field("rank"),
      q.Values({Integer(0)}))));
  /* Still an entity of the type: no predicate, or a fact, finds it. */
  EXPECT_EQ(Ids({"a1", "a2", "a3"}, 3), AskIn("de", nullptr));
  EXPECT_EQ(Ids({"a1", "a2", "a3"}, 3), AskIn("de", q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Meta(GDEFSTOR_META_PUBLISHED), q.Values({Int(GDEFSTOR_FIELD_BOOLEAN,
          0)}))));
  /* Every field absent: != and a NOT hold, as for an absent value. */
  EXPECT_EQ(Ids({"a3"}, 1), AskIn("fr", q.Op(GDEFSTOR_QUERY_AND,
      {q.Cmp(GDEFSTOR_QUERY_NE, q.Field("title"), q.Values({Text("Bonjour"),
           Text("Salut")}))})));
  /* It sorts as no value: first ascending, last descending. */
  EXPECT_EQ(Ids({"a3", "a1", "a2"}, 3), AskIn("fr", nullptr,
      {By(q.Field("rank"))}));
  EXPECT_EQ(Ids({"a2", "a1", "a3"}, 3), AskIn("fr", nullptr,
      {By(q.Field("rank"), true)}));
}

TEST_F(ModelLanguage, EveryPastRevisionReadsBackInEveryLanguage) {
  ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a1"));
  /* rev -> lang -> tree, as written. */
  std::map<uint64_t, std::map<std::string, Node>> want;
  Node en = Article("One", 1, {"x"});
  Node fr = Article("Un", 1);
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 0, {{"en", &en}, {"fr", &fr}}));
  want[1] = {{"en", NoIds(en)}, {"fr", NoIds(fr)}};
  Node de = Article("Eins", 1, {"y", "z"});
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 1, {{"de", &de}}));
  want[2] = want[1];
  want[2]["de"] = NoIds(de);
  Node fr2 = In("a1", "fr");
  fr2.Set("title", Text("Une"));
  fr2.Add("tags", One(Text("t")));
  Node en2 = In("a1", "en");
  en2.kids["tags"].clear();
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 2, {{"fr", &fr2}, {"en", &en2}}));
  want[3] = want[2];
  want[3]["fr"] = NoIds(fr2);
  want[3]["en"] = NoIds(en2);
  ASSERT_EQ(GDEFSTOR_OK, Remove("a1", 3, "en"));
  want[4] = want[3];
  want[4].erase("en");
  for (const auto & r : want) {
    std::vector<std::string> langs;
    for (const auto & kv : r.second) {
      langs.push_back(kv.first);
      EXPECT_EQ(kv.second, NoIds(In("a1", kv.first, r.first)))
          << "rev " << r.first << " " << kv.first;
    }
    EXPECT_EQ(langs, Langs("a1", r.first)) << "rev " << r.first;
  }
  EXPECT_EQ((std::vector<std::string>{"1:en,fr", "2:de", "3:en,fr", "4:en"}),
      RevLangs("a1"));
}

TEST_F(ModelLanguage, RemovingALanguageWritesARevisionAndHistoryKeepsIt) {
  ThreeArticles();
  Q q;
  const GDEFSTOR_QueryNode * bonjour = q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("title"), q.Values({Text("Bonjour")}));
  ASSERT_EQ(Ids({"a1"}, 1), AskIn("fr", bonjour));
  Node fr_before = In("a1", "fr");
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, Remove("a1", 1, "fr", &rev));
  EXPECT_EQ(2u, rev);
  std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ("", AppendOnly(before, after));
  EXPECT_EQ(1u, NewRows(before, after, 'R'));
  /* Closed, not deleted, and nothing inserted. */
  EXPECT_EQ(0u, NewRows(before, after, 'I'));
  EXPECT_EQ(Ids({}, 0), AskIn("fr", bonjour));
  EXPECT_EQ((std::vector<std::string>{"en"}), Langs("a1"));
  Node n;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, TreeIn("a1", "fr", 0, &n));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, TreeIn("a1", "fr", 2, &n));
  EXPECT_EQ(fr_before, In("a1", "fr", 1));
  EXPECT_EQ((std::vector<std::string>{"1:en,fr", "2:fr"}), RevLangs("a1"));
  EXPECT_EQ((std::vector<std::string>{"1:en,fr", "2:fr"}),
      RevLangs("a1", "fr"));
  EXPECT_EQ((std::vector<std::string>{"1:en,fr"}), RevLangs("a1", "en"));
  /* Removing one it does not have writes nothing. */
  ASSERT_EQ(GDEFSTOR_OK, Remove("a1", 2, "fr", &rev));
  EXPECT_EQ(0u, rev);
  ASSERT_EQ(GDEFSTOR_OK, Remove("a1", 2, "de", &rev));
  EXPECT_EQ(0u, rev);
  EXPECT_EQ(after, Dump(GDEFSTOR_MODEL_TRUTH));
  /* A later save starts a new tree, with a new root. */
  Node fr2 = Article("Coucou", 5);
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 2, {{"fr", &fr2}}, &rev));
  EXPECT_EQ(3u, rev);
  EXPECT_NE(fr_before.id, In("a1", "fr").id);
  EXPECT_EQ(NoIds(fr2), NoIds(In("a1", "fr")));
  /* The derived state of every language rebuilds to what is live. */
  std::string current = Dump(GDEFSTOR_MODEL_CURRENT);
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_model_rebuild(w);
  }));
  EXPECT_EQ(current, Dump(GDEFSTOR_MODEL_CURRENT));
}

TEST_F(ModelLanguage, ARevertRestoresEveryLanguage) {
  ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a1"));
  Node en = Article("One", 1, {"x"});
  Node fr = Article("Un", 1);
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 0, {{"en", &en}, {"fr", &fr}}));
  Node en1 = In("a1", "en", 1);
  Node fr1 = In("a1", "fr", 1);
  /* r2: fr removed; r3: de added and en changed; r4: fr back, new root. */
  ASSERT_EQ(GDEFSTOR_OK, Remove("a1", 1, "fr"));
  Node de = Article("Eins", 7);
  Node en3 = In("a1", "en");
  en3.Set("title", Text("Uno"));
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 2, {{"de", &de}, {"en", &en3}}));
  Node fr4 = Article("Deux", 2);
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 3, {{"fr", &fr4}}));
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  uint64_t rev = 0;
  GDEFSTOR_RevisionMeta m = Meta("u2", 9000, "revert");
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_entity_revert(w, "a1", 2, 4, 1, &m, &rev);
  }));
  EXPECT_EQ(5u, rev);
  std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ("", AppendOnly(before, after));
  /* Exactly revision 1's trees, under their old ids; de is gone. */
  EXPECT_EQ(en1, In("a1", "en"));
  EXPECT_EQ(fr1, In("a1", "fr"));
  EXPECT_EQ((std::vector<std::string>{"en", "fr"}), Langs("a1"));
  EXPECT_EQ("5:de,en,fr", RevLangs("a1").back());
  /* Every revision in between reads as it did. */
  EXPECT_EQ((std::vector<std::string>{"de", "en", "fr"}), Langs("a1", 4));
  EXPECT_EQ(NoIds(fr4), NoIds(In("a1", "fr", 4)));
  /* Reverting to what is current writes nothing. */
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_entity_revert(w, "a1", 2, 5, 1, &m, &rev);
  }));
  EXPECT_EQ(0u, rev);
}

TEST_F(ModelLanguage, DiffAndHistoryNameTheLanguagesARevisionTouched) {
  ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a1"));
  Node en = Article("One", 1);
  Node fr = Article("Un", 1);
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 0, {{"en", &en}, {"fr", &fr}}));
  Node fr2 = In("a1", "fr");
  fr2.Add("tags", One(Text("t")));
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 1, {{"fr", &fr2}}));
  auto diff = [&](const char * lang, uint64_t from, uint64_t to) {
    std::vector<std::string> out;
    GDEFSTOR_Diff * d = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_diff(store, kTenant, 2, "a1", 2,
        lang, lang == nullptr ? 0 : std::strlen(lang), from, to, &d));
    for (size_t i = 0; i < gdefstor_diff_count(d); i++) {
      GDEFSTOR_DiffItem x;
      std::memset(&x, 0, sizeof x);
      x.size = sizeof x;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_diff_item(d, i, &x));
      out.push_back(std::string(x.language, x.language_len) + ":" +
          std::to_string(x.kind) + ":" + std::string(x.field, x.field_len));
    }
    gdefstor_diff_free(d);
    return out;
  };
  const std::string added = std::to_string(GDEFSTOR_DIFF_ADDED);
  EXPECT_EQ((std::vector<std::string>{"fr:" + added + ":tags"}),
      diff(nullptr, 1, 2));
  EXPECT_EQ((std::vector<std::string>{"fr:" + added + ":tags"}),
      diff("fr", 1, 2));
  EXPECT_EQ((std::vector<std::string>{}), diff("en", 1, 2));
  EXPECT_EQ((std::vector<std::string>{"en:" + added + ":"}), diff("en", 0, 1));
  EXPECT_EQ(3u, diff(nullptr, 0, 2).size());
  EXPECT_EQ((std::vector<std::string>{"1:en,fr", "2:fr"}), RevLangs("a1"));
  EXPECT_EQ((std::vector<std::string>{"1:en,fr"}), RevLangs("a1", "en"));
  EXPECT_EQ((std::vector<std::string>{}), RevLangs("a1", "de"));
}

TEST_F(ModelLanguage, ADraftHoldsEveryLanguageItChanges) {
  ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a1"));
  Node en = Article("One", 1);
  Node fr = Article("Un", 1);
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 0, {{"en", &en}, {"fr", &fr}}));
  GDEFSTOR_Type * type = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(store, kTenant, 2, kArticle,
      std::strlen(kArticle), &type));
  auto json = [&](const Node & n) {
    GDEFSTOR_Tree * t = Build(n);
    uint8_t * j = nullptr;
    size_t len = 0;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_tree_to_json(type, t, &j, &len));
    std::string s(reinterpret_cast<const char *>(j), len);
    gdefstor_bytes_free(nullptr, j);
    gdefstor_tree_free(t);
    return s;
  };
  Node fr2 = In("a1", "fr");
  fr2.Set("title", Text("Deux"));
  Node de = Article("Zwei", 2);
  /* Spaces around a tree are kept as put. */
  std::string fr_json = " " + json(fr2) + "\n";
  std::string de_json = json(de);
  GDEFSTOR_DraftTree t[2];
  std::memset(t, 0, sizeof t);
  t[0].size = sizeof t[0];
  t[0].language = "fr";
  t[0].language_len = 2;
  t[0].json = reinterpret_cast<const uint8_t *>(fr_json.data());
  t[0].json_len = fr_json.size();
  t[1] = t[0];
  t[1].language = "de";
  t[1].json = reinterpret_cast<const uint8_t *>(de_json.data());
  t[1].json_len = de_json.size();
  std::string truth = Dump(GDEFSTOR_MODEL_TRUTH);
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_draft_put(w, nullptr, 0, "a1", 2, 1, kStamp, kStampLen, t, 2);
  }));
  /* One draft for the entity, holding both, in language order, as put. */
  EXPECT_EQ(1u, NewRows(truth, Dump(GDEFSTOR_MODEL_TRUTH), 'D'));
  GDEFSTOR_Draft * d = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_draft_get(store, kTenant, 2, "a1", 2, &d));
  GDEFSTOR_DraftInfo info;
  std::memset(&info, 0, sizeof info);
  info.size = sizeof info;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_draft_info(d, &info));
  ASSERT_EQ(2u, info.language_count);
  GDEFSTOR_DraftTree got;
  std::memset(&got, 0, sizeof got);
  got.size = sizeof got;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_draft_tree(d, 0, &got));
  EXPECT_EQ("de", std::string(got.language, got.language_len));
  EXPECT_EQ(de_json, std::string(reinterpret_cast<const char *>(got.json),
      got.json_len));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_draft_tree(d, 1, &got));
  EXPECT_EQ("fr", std::string(got.language, got.language_len));
  EXPECT_EQ(fr_json, std::string(reinterpret_cast<const char *>(got.json),
      got.json_len));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_draft_tree(d, 2, &got));
  gdefstor_draft_free(d);
  /* Applied: one revision with both; en, left out, keeps its tree. */
  GDEFSTOR_RevisionMeta m = Meta("ed", 7000, "apply");
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_draft_apply(w, "a1", 2, &m, &rev);
  }));
  EXPECT_EQ(2u, rev);
  EXPECT_EQ("2:de,fr", RevLangs("a1").back());
  EXPECT_EQ(NoIds(en), NoIds(In("a1", "en")));
  EXPECT_EQ(NoIds(fr2), NoIds(In("a1", "fr")));
  EXPECT_EQ(NoIds(de), NoIds(In("a1", "de")));
  /* A language twice, or a tag that is not one, is refused at put. */
  t[1].language = "fr";
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_draft_put(w, nullptr, 0, "a1", 2, 2, kStamp, kStampLen, t, 2);
  }));
  t[1].language = "f_r";
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_draft_put(w, nullptr, 0, "a1", 2, 2, kStamp, kStampLen, t, 2);
  }));
  /* A reserved id: the entity and its two trees in one revision. */
  t[1].language = "de";
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_draft_put(w, kArticle, std::strlen(kArticle), "a9", 2, 0,
        kStamp, kStampLen,
        t, 2);
  }));
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_draft_apply(w, "a9", 2, &m, &rev);
  }));
  EXPECT_EQ(1u, rev);
  EXPECT_EQ((std::vector<std::string>{"de", "fr"}), Langs("a9"));
  gdefstor_type_free(type);
}

TEST_F(ModelLanguage, TagsAreCheckedAndComparedByBytes) {
  ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a1"));
  Node en = Article("lower", 1);
  Node EN = Article("upper", 2);
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 0, {{"en", &en}, {"EN", &EN}}));
  EXPECT_EQ((std::vector<std::string>{"EN", "en"}), Langs("a1"));
  EXPECT_EQ(NoIds(EN), NoIds(In("a1", "EN")));
  EXPECT_EQ(NoIds(en), NoIds(In("a1", "en")));
  std::string truth = Dump(GDEFSTOR_MODEL_TRUTH);
  const std::string longest(GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES, 'x');
  Node any = Article("t", 1);
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 1, {{longest, &any}, {"pt-BR", &any}}));
  truth = Dump(GDEFSTOR_MODEL_TRUTH);
  for (const std::string & bad : {std::string(""), longest + "x",
           std::string("en_US"), std::string("en US"), std::string("\xc3\xa9"),
           std::string("e\0n", 3)}) {
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, SaveLangs("a1", 2, {{bad, &any}}));
    Node n;
    EXPECT_EQ(GDEFSTOR_ERR_INVALID, TreeIn("a1", bad, 0, &n));
    if (bad.find('\0') == std::string::npos) {
      EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kArticle, nullptr, {}, 0, 0,
          bad.c_str()).rc);
    }
  }
  /* A language named twice in one save. */
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, SaveLangs("a1", 2, {{"de", &any},
      {"de", &any}}));
  EXPECT_EQ(truth, Dump(GDEFSTOR_MODEL_TRUTH));
  /* A query binds one language: none is refused, and so is a count's. */
  GDEFSTOR_Query q;
  std::memset(&q, 0, sizeof q);
  q.size = sizeof q;
  GDEFSTOR_QueryResult * r = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_query(store, kTenant, 2,
      kArticle, std::strlen(kArticle), &q, &r));
  uint64_t n = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_count(store, kTenant, 2,
      kArticle, std::strlen(kArticle), nullptr, 0, nullptr, &n));
  /* History and diff take a tag or none. */
  GDEFSTOR_History * h = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_history(store, kTenant, 2,
      "a1", 2, "en_US", 5, &h));
  GDEFSTOR_Diff * d = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_diff(store, kTenant, 2,
      "a1", 2, "", 1, 0, 1, &d));
}

TEST_F(ModelLanguage, TheLanguagesOfOneEntityAreCappedWithAnError) {
  ASSERT_EQ(GDEFSTOR_OK, Create(kArticle, "a1"));
  Node any = Article("t", 1);
  std::vector<std::string> tags;
  for (int i = 0; i <= GDEFSTOR_MODEL_MAX_LANGUAGES; i++) {
    char b[16];
    std::snprintf(b, sizeof b, "l%03d", i);
    tags.push_back(b);
  }
  /* One past the cap in one save: refused, nothing written. */
  std::vector<std::pair<std::string, const Node *>> all;
  for (const std::string & t : tags) {
    all.push_back({t, &any});
  }
  std::string truth = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, SaveLangs("a1", 0, all));
  EXPECT_EQ(truth, Dump(GDEFSTOR_MODEL_TRUTH));
  /* The cap itself is fine, in one save. */
  all.pop_back();
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 0, all, &rev));
  EXPECT_EQ(1u, rev);
  EXPECT_EQ(static_cast<size_t>(GDEFSTOR_MODEL_MAX_LANGUAGES),
      Langs("a1").size());
  /* One more, in a save of its own, is past it. */
  truth = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, SaveLangs("a1", 1, {{tags.back(), &any}}));
  EXPECT_EQ(truth, Dump(GDEFSTOR_MODEL_TRUTH));
  /* Removing one and adding another in the same revision stays at it. */
  ASSERT_EQ(GDEFSTOR_OK, SaveLangs("a1", 1, {{tags.front(), nullptr},
      {tags.back(), &any}}, &rev));
  EXPECT_EQ(2u, rev);
  EXPECT_EQ(static_cast<size_t>(GDEFSTOR_MODEL_MAX_LANGUAGES),
      Langs("a1").size());
}

class ModelLanguageBlind : public ModelBlind {};

TEST_F(ModelLanguageBlind, BlindRowsAndSubjectsNameTheirLanguage) {
  ASSERT_EQ(GDEFSTOR_OK, Create(kMember, "m1"));
  Node en = MemberTree("ann", "s1", "x");
  Node fr = MemberTree("anne", "s1", "x");
  std::vector<GDEFSTOR_Tree *> trees = {Build(en), Build(fr)};
  GDEFSTOR_LanguageTree lt[2];
  std::memset(lt, 0, sizeof lt);
  lt[0].size = sizeof lt[0];
  lt[0].language = "en";
  lt[0].language_len = 2;
  lt[0].tree = trees[0];
  lt[1] = lt[0];
  lt[1].language = "fr";
  lt[1].tree = trees[1];
  GDEFSTOR_RevisionMeta m = Meta();
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_entity_save_languages(w, "m1", 2, 0, &m, lt, 2, nullptr);
  }));
  for (GDEFSTOR_Tree * t : trees) {
    gdefstor_tree_free(t);
  }
  /* Blind equality reads the query's language. */
  Q q;
  auto blind = [&](const char * lang, const std::string & plain) {
    return Ask(store, kMember, q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("name"),
        q.Mac(MacOf(plain))), {}, 0, 0, lang);
  };
  EXPECT_EQ(Ids({"m1"}, 1), blind("en", "ann"));
  EXPECT_EQ(Ids({}, 0), blind("fr", "ann"));
  EXPECT_EQ(Ids({"m1"}, 1), blind("fr", "anne"));
  EXPECT_EQ(Ids({}, 0), blind("de", "anne"));
  /* The history lookup finds every language, each hit naming its own. */
  std::string mac = MacOf("anne");
  GDEFSTOR_BlindHits * h = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_blind_history(store, kTenant, 2, kMember,
      std::strlen(kMember), "name", 4,
      reinterpret_cast<const uint8_t *>(mac.data()), mac.size(), &h));
  ASSERT_EQ(1u, gdefstor_blind_hits_count(h));
  GDEFSTOR_BlindHit x;
  std::memset(&x, 0, sizeof x);
  x.size = sizeof x;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_blind_hit(h, 0, &x));
  EXPECT_EQ("fr", std::string(x.language, x.language_len));
  gdefstor_blind_hits_free(h);
  /* The subject lookup: both versions, each naming its language. */
  GDEFSTOR_SubjectHits * s = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_subject_lookup(store, kTenant, 2, "s1", 2,
      &s));
  std::set<std::string> langs;
  for (size_t i = 0; i < gdefstor_subject_hits_count(s); i++) {
    GDEFSTOR_SubjectHit y;
    std::memset(&y, 0, sizeof y);
    y.size = sizeof y;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_subject_hit(s, i, &y));
    langs.insert(std::string(y.language, y.language_len));
  }
  gdefstor_subject_hits_free(s);
  EXPECT_EQ((std::set<std::string>{"en", "fr"}), langs);
  /* The dump's blind rows carry the language too. */
  std::set<std::string> blangs;
  for (const std::string & l : Lines(Dump(GDEFSTOR_MODEL_TRUTH), 'B')) {
    blangs.insert(Words(l).back());
  }
  EXPECT_EQ((std::set<std::string>{HexOf("en"), HexOf("fr")}), blangs);
}

/* ---- the sweep ------------------------------------------------------------ */

/* Seeded multilingual histories (saves of some languages, removals, reverts)
 * checked against a model written here, apart from the library: every
 * language's tree at every revision, the languages each revision touched,
 * and queries that bind each language, with a filter and a sort. */
class ModelLanguageSweep : public ModelLanguage {};

TEST_F(ModelLanguageSweep, SeededHistoriesAgreeWithAModel) {
  const std::vector<std::string> kLangs = {"de", "en", "fr"};
  const std::vector<std::string> kTitles = {"a", "b", "c"};
  const int kEntities = 4;
  const int kSteps = 30;
  for (uint32_t seed = 1; seed <= 3; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    const std::string run = "s" + std::to_string(seed);
    /* A type per seed, so each run's queries see only its own entities. */
    const std::string type = "site.sweep" + std::to_string(seed);
    ASSERT_EQ(GDEFSTOR_OK, Declare(type, ArticleType()));
    std::mt19937 rng(seed);
    auto pick = [&](size_t n) {
      return static_cast<size_t>(rng() % n);
    };
    /* entity -> revision -> language -> (title, rank). */
    typedef std::map<std::string, std::pair<std::string, int64_t>> State;
    std::vector<std::vector<State>> hist(kEntities);
    std::vector<std::vector<std::string>> touched(kEntities);
    for (int e = 0; e < kEntities; e++) {
      std::string pid = run + "e" + std::to_string(e);
      ASSERT_EQ(GDEFSTOR_OK, Create(type, pid));
      hist[e].push_back(State());
    }
    for (int step = 0; step < kSteps; step++) {
      int e = static_cast<int>(pick(kEntities));
      std::string pid = run + "e" + std::to_string(e);
      uint64_t head = hist[e].size() - 1;
      State now = hist[e].back();
      State next = now;
      int what = static_cast<int>(pick(10));
      uint64_t rev = 0;
      if (what < 7 || now.empty()) {
        /* A save of one to three languages, some perhaps unchanged. */
        std::vector<std::pair<std::string, const Node *>> langs;
        std::deque<Node> nodes;
        std::vector<std::string> order = kLangs;
        std::shuffle(order.begin(), order.end(), rng);
        size_t n = 1 + pick(3);
        for (size_t k = 0; k < n; k++) {
          const std::string & l = order[k];
          std::pair<std::string, int64_t> v = now.count(l) != 0
              ? now[l] : std::make_pair(kTitles[pick(3)],
                             static_cast<int64_t>(pick(3)));
          if (pick(3) != 0) {
            v.first = kTitles[pick(3)];
            v.second = static_cast<int64_t>(pick(3));
          }
          next[l] = v;
          nodes.push_back(Article(v.first.c_str(), v.second));
          langs.push_back({l, &nodes.back()});
        }
        ASSERT_EQ(GDEFSTOR_OK, SaveLangs(pid, head, langs, &rev));
      } else if (what < 9) {
        const std::string & l = kLangs[pick(3)];
        next.erase(l);
        ASSERT_EQ(GDEFSTOR_OK, Remove(pid, head, l, &rev));
      } else {
        uint64_t to = 1 + pick(static_cast<size_t>(head == 0 ? 1 : head));
        if (head == 0) {
          continue;
        }
        next = hist[e][to];
        GDEFSTOR_RevisionMeta m = Meta("u2", 3000, "revert");
        ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
          return gdefstor_entity_revert(w, pid.data(), pid.size(), head, to,
              &m, &rev);
        }));
      }
      std::string langs;
      for (const std::string & l : kLangs) {
        if (now.count(l) != next.count(l) ||
            (now.count(l) != 0 && now[l] != next[l])) {
          langs += (langs.empty() ? "" : ",") + l;
        }
      }
      if (langs.empty()) {
        ASSERT_EQ(0u, rev) << "step " << step;
        continue;
      }
      ASSERT_EQ(head + 1, rev) << "step " << step;
      hist[e].push_back(next);
      touched[e].push_back(std::to_string(rev) + ":" + langs);
    }
    for (int e = 0; e < kEntities; e++) {
      std::string pid = run + "e" + std::to_string(e);
      EXPECT_EQ(touched[e], RevLangs(pid)) << pid;
      for (uint64_t r = 1; r < hist[e].size(); r++) {
        std::vector<std::string> want;
        for (const auto & kv : hist[e][r]) {
          want.push_back(kv.first);
          Node n = NoIds(In(pid, kv.first, r));
          EXPECT_EQ(NoIds(Article(kv.second.first.c_str(), kv.second.second)),
              n) << pid << " r" << r << " " << kv.first;
        }
        EXPECT_EQ(want, Langs(pid, r)) << pid << " r" << r;
      }
    }
    /* Queries in each language: title = t, sorted by rank, then title. */
    for (const std::string & l : kLangs) {
      for (const std::string & t : kTitles) {
        std::vector<std::pair<std::pair<int64_t, std::string>, int>> rows;
        for (int e = 0; e < kEntities; e++) {
          const State & s = hist[e].back();
          auto it = s.find(l);
          if (it != s.end() && it->second.first == t) {
            rows.push_back({{it->second.second, it->second.first}, e});
          }
        }
        std::sort(rows.begin(), rows.end());
        std::vector<std::string> ids;
        for (const auto & r : rows) {
          ids.push_back(run + "e" + std::to_string(r.second));
        }
        Q q;
        EXPECT_EQ(Ids(ids, ids.size()), Ask(store, type.c_str(), q.Cmp(
            GDEFSTOR_QUERY_EQ, q.Field("title"), q.Values({Text(t.c_str())})),
            {By(q.Field("rank")), By(q.Field("title"))}, 0, 0, l.c_str()))
            << l << " " << t;
      }
      /* A sort over every entity: no tree in the language sorts as none. */
      std::vector<std::pair<std::pair<int, int64_t>, int>> rows;
      for (int e = 0; e < kEntities; e++) {
        const State & s = hist[e].back();
        auto it = s.find(l);
        rows.push_back({{it == s.end() ? 0 : 1,
            it == s.end() ? 0 : it->second.second}, e});
      }
      std::sort(rows.begin(), rows.end());
      std::vector<std::string> ids;
      for (const auto & r : rows) {
        ids.push_back(run + "e" + std::to_string(r.second));
      }
      Q q;
      EXPECT_EQ(Ids(ids, ids.size()), Ask(store, type.c_str(), nullptr,
          {By(q.Field("rank"))}, 0, 0, l.c_str())) << l;
    }
  }
}
