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
 * A tenant that holds every kind of entity truth (story S6a3), written
 * through the public port only, so that every backend and every cross-backend
 * test builds the same one: two types (typed scalars, a personal field, a
 * multiple scalar, a multiple group; a field retired after data was saved
 * in it), entities with several revisions in two languages (a reorder, a
 * language added and removed, a revert), states, a draft of an entity and a
 * draft that reserves an id, records (text, binary, empty), counters (one
 * with an aborted value above it) and outbox rows (claimed, failed, parked,
 * in two streams).
 *
 * Installed beside model.inc; it needs gtest and the defiant-storage
 * headers, nothing else. `snapshot_fixture::Populate(store, tenant, mac)`
 * builds it; with @p mac the personal values carry blind-index MACs.
 */

#ifndef GDEFSTOR_SNAPSHOT_FIXTURE_INC
#define GDEFSTOR_SNAPSHOT_FIXTURE_INC

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace snapshot_fixture {

inline GDEFSTOR_Value V(int type) {
  GDEFSTOR_Value v;
  std::memset(&v, 0, sizeof v);
  v.type = type;
  return v;
}

inline GDEFSTOR_Value Txt(const char * s, int type = GDEFSTOR_FIELD_TEXT) {
  GDEFSTOR_Value v = V(type);
  v.bytes = reinterpret_cast<const uint8_t *>(s);
  v.len = std::strlen(s);
  return v;
}

inline GDEFSTOR_Value Num(int type, int64_t i) {
  GDEFSTOR_Value v = V(type);
  v.integer = i;
  return v;
}

inline GDEFSTOR_Value Flt(double d) {
  GDEFSTOR_Value v = V(GDEFSTOR_FIELD_FLOAT);
  v.real = d;
  return v;
}

/* A personal value: ciphertext bytes the caller made, and its subject. */
inline GDEFSTOR_Value Secret(const std::string & cipher,
    const char * subject) {
  GDEFSTOR_Value v = V(GDEFSTOR_FIELD_TEXT);
  v.bytes = reinterpret_cast<const uint8_t *>(cipher.data());
  v.len = cipher.size();
  v.subject = reinterpret_cast<const uint8_t *>(subject);
  v.subject_len = std::strlen(subject);
  return v;
}

inline GDEFSTOR_FieldSpec Spec(const char * name, int type,
    unsigned flags = 0, int scale = 0) {
  GDEFSTOR_FieldSpec f;
  std::memset(&f, 0, sizeof f);
  f.size = sizeof f;
  f.name = name;
  f.name_len = std::strlen(name);
  f.type = type;
  f.flags = flags;
  f.scale = scale;
  return f;
}

/* Run @p body in one write of @p tenant and commit it, or abort it with
 * @p commit false. */
inline GDEFSTOR_Result InWrite(GDEFSTOR_Store * s, const char * tenant,
    const std::function<GDEFSTOR_Result(GDEFSTOR_Write *)> & body,
    bool commit = true) {
  GDEFSTOR_Write * w = nullptr;
  GDEFSTOR_Result rc =
      gdefstor_write_begin(s, tenant, std::strlen(tenant), nullptr, &w);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = body(w);
  if (rc != GDEFSTOR_OK || !commit) {
    gdefstor_write_abort(w);
    return rc;
  }
  return gdefstor_write_commit(w);
}

inline GDEFSTOR_RevisionMeta Meta(const char * who, int64_t when,
    const char * why = "") {
  GDEFSTOR_RevisionMeta m;
  std::memset(&m, 0, sizeof m);
  m.size = sizeof m;
  m.who = who;
  m.who_len = std::strlen(who);
  m.when_ms = when;
  m.why = why;
  m.why_len = std::strlen(why);
  return m;
}

/* The declarations. `shop.order` in two shapes: the second retires nothing,
 * `shop.cust` in two: the second leaves `ref` out, retiring it. */
inline GDEFSTOR_Result DeclareOrder(GDEFSTOR_Write * w) {
  std::vector<GDEFSTOR_FieldSpec> line = {
      Spec("sku", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_INDEXED),
      Spec("qty", GDEFSTOR_FIELD_INTEGER),
      Spec("price", GDEFSTOR_FIELD_DECIMAL, 0, 2),
  };
  std::vector<GDEFSTOR_FieldSpec> root = {
      Spec("number", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_INDEXED),
      Spec("total", GDEFSTOR_FIELD_DECIMAL, 0, 2),
      Spec("placed", GDEFSTOR_FIELD_DATE),
      Spec("paid", GDEFSTOR_FIELD_BOOLEAN),
      Spec("weight", GDEFSTOR_FIELD_FLOAT),
      Spec("note", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_PERSONAL),
      Spec("tags", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_MULTIPLE),
      Spec("lines", GDEFSTOR_FIELD_GROUP, GDEFSTOR_FIELD_MULTIPLE),
  };
  root.back().children = line.data();
  root.back().child_count = line.size();
  GDEFSTOR_TypeSpec t;
  std::memset(&t, 0, sizeof t);
  t.size = sizeof t;
  t.name = "shop.order";
  t.name_len = 10;
  t.fields = root.data();
  t.field_count = root.size();
  return gdefstor_type_declare(w, &t);
}

inline GDEFSTOR_Result DeclareCust(GDEFSTOR_Write * w, bool with_ref) {
  std::vector<GDEFSTOR_FieldSpec> root = {
      Spec("name", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_INDEXED),
      Spec("born", GDEFSTOR_FIELD_DATETIME),
  };
  if (with_ref) {
    root.push_back(Spec("ref", GDEFSTOR_FIELD_REFERENCE));
  }
  GDEFSTOR_TypeSpec t;
  std::memset(&t, 0, sizeof t);
  t.size = sizeof t;
  t.name = "shop.cust";
  t.name_len = 9;
  t.fields = root.data();
  t.field_count = root.size();
  return gdefstor_type_declare(w, &t);
}

/* The id of the item under @p field whose own value (a multiple scalar) or
 * whose @p sub value is @p text, in @p pid's current tree of @p lang; 0 when
 * there is none. */
inline uint64_t IdOf(GDEFSTOR_Store * s, const char * tenant, const char * pid,
    const char * lang, const char * field, const char * sub,
    const char * text) {
  GDEFSTOR_Tree * t = nullptr;
  uint64_t id = 0;
  if (gdefstor_entity_tree(s, tenant, std::strlen(tenant), pid,
          std::strlen(pid), lang, std::strlen(lang), 0, &t) != GDEFSTOR_OK) {
    return 0;
  }
  for (size_t i = 0; i < gdefstor_tree_item_count(t) && id == 0; i++) {
    GDEFSTOR_TreeItem it;
    std::memset(&it, 0, sizeof it);
    it.size = sizeof it;
    if (gdefstor_tree_item(t, i, &it) != GDEFSTOR_OK ||
        std::string(it.field, it.field_len) != field) {
      continue;
    }
    GDEFSTOR_Value v;
    std::memset(&v, 0, sizeof v);
    if (gdefstor_tree_get(t, i, sub, sub == nullptr ? 0 : std::strlen(sub),
            &v) == GDEFSTOR_OK &&
        std::string(reinterpret_cast<const char *>(v.bytes), v.len) == text) {
      id = it.id;
    }
  }
  gdefstor_tree_free(t);
  return id;
}

/* One order line. */
struct Line {
  uint64_t id;
  const char * sku;
  int64_t qty;
  int64_t price;
};

/* An order tree. */
inline GDEFSTOR_Tree * Order(const char * number, int64_t total, bool paid,
    const std::string & cipher, const std::vector<std::pair<uint64_t,
        const char *>> & tags, const std::vector<Line> & lines,
    const GDEFSTOR_BlindMac * mac) {
  GDEFSTOR_Tree * t = nullptr;
  if (gdefstor_tree_create(nullptr, &t) != GDEFSTOR_OK) {
    return nullptr;
  }
  GDEFSTOR_Value v = Txt(number);
  gdefstor_tree_set(t, 0, "number", 6, &v);
  v = Num(GDEFSTOR_FIELD_DECIMAL, total);
  gdefstor_tree_set(t, 0, "total", 5, &v);
  v = Num(GDEFSTOR_FIELD_DATE, 20000);
  gdefstor_tree_set(t, 0, "placed", 6, &v);
  v = Num(GDEFSTOR_FIELD_BOOLEAN, paid ? 1 : 0);
  gdefstor_tree_set(t, 0, "paid", 4, &v);
  v = Flt(1.0 / 3.0);
  gdefstor_tree_set(t, 0, "weight", 6, &v);
  if (!cipher.empty()) {
    v = Secret(cipher, "subj1");
    gdefstor_tree_set(t, 0, "note", 4, &v);
    if (mac != nullptr) {
      gdefstor_tree_set_blind(t, 0, "note", 4, mac);
    }
  }
  for (const auto & tag : tags) {
    size_t at = 0;
    gdefstor_tree_add(t, 0, "tags", 4, tag.first, &at);
    v = Txt(tag.second);
    gdefstor_tree_set(t, at, nullptr, 0, &v);
  }
  for (const Line & l : lines) {
    size_t at = 0;
    gdefstor_tree_add(t, 0, "lines", 5, l.id, &at);
    v = Txt(l.sku);
    gdefstor_tree_set(t, at, "sku", 3, &v);
    v = Num(GDEFSTOR_FIELD_INTEGER, l.qty);
    gdefstor_tree_set(t, at, "qty", 3, &v);
    v = Num(GDEFSTOR_FIELD_DECIMAL, l.price);
    gdefstor_tree_set(t, at, "price", 5, &v);
  }
  return t;
}

inline GDEFSTOR_Tree * Cust(const char * name, const char * ref) {
  GDEFSTOR_Tree * t = nullptr;
  if (gdefstor_tree_create(nullptr, &t) != GDEFSTOR_OK) {
    return nullptr;
  }
  GDEFSTOR_Value v = Txt(name);
  gdefstor_tree_set(t, 0, "name", 4, &v);
  v = Num(GDEFSTOR_FIELD_DATETIME, INT64_C(1000000000123));
  gdefstor_tree_set(t, 0, "born", 4, &v);
  if (ref != nullptr) {
    v = Txt(ref, GDEFSTOR_FIELD_REFERENCE);
    gdefstor_tree_set(t, 0, "ref", 3, &v);
  }
  return t;
}

/* Save @p trees (language, tree; a NULL tree removes the language) on
 * @p base, freeing the trees. */
inline GDEFSTOR_Result SaveTrees(GDEFSTOR_Store * s, const char * tenant,
    const char * pid, uint64_t base, int64_t when,
    std::vector<std::pair<const char *, GDEFSTOR_Tree *>> trees,
    const char * why = "") {
  std::vector<GDEFSTOR_LanguageTree> lt;
  for (const auto & p : trees) {
    GDEFSTOR_LanguageTree x;
    std::memset(&x, 0, sizeof x);
    x.size = sizeof x;
    x.language = p.first;
    x.language_len = std::strlen(p.first);
    x.tree = p.second;
    lt.push_back(x);
  }
  GDEFSTOR_RevisionMeta m = Meta("u1", when, why);
  uint64_t got = 0;
  GDEFSTOR_Result rc = InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    return gdefstor_entity_save_languages(w, pid, std::strlen(pid), base, &m,
        lt.data(), lt.size(), &got);
  });
  for (const auto & p : trees) {
    gdefstor_tree_free(p.second);
  }
  if (rc == GDEFSTOR_OK && got != base + 1) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  return rc;
}

#define SNAPFIX_TRY(x)                                                       \
  do {                                                                       \
    GDEFSTOR_Result snapfix_rc = (x);                                        \
    if (snapfix_rc != GDEFSTOR_OK) {                                         \
      ADD_FAILURE() << #x << ": " << gdefstor_result_string(snapfix_rc);     \
      return snapfix_rc;                                                     \
    }                                                                        \
  } while (0)

/* The MAC the personal values carry when Populate() is asked for them. */
inline const uint8_t * MacBytes() {
  static uint8_t m[GDEFSTOR_BLIND_MAC_BYTES];
  for (size_t i = 0; i < sizeof m; i++) {
    m[i] = static_cast<uint8_t>(0xA0 + i);
  }
  return m;
}

inline GDEFSTOR_Result Populate(GDEFSTOR_Store * s, const char * tenant,
    bool mac) {
  const size_t tl = std::strlen(tenant);
  GDEFSTOR_BlindMac bm;
  std::memset(&bm, 0, sizeof bm);
  bm.size = sizeof bm;
  bm.mac = MacBytes();
  bm.mac_len = GDEFSTOR_BLIND_MAC_BYTES;
  bm.key_id = 7;
  bm.version = 1;
  const GDEFSTOR_BlindMac * pm = mac ? &bm : nullptr;
  const std::string cipher1("\x01\xfe\x00\x7f" "c1", 6);
  const std::string cipher2("\x02\xfd" "c2", 4);
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = DeclareOrder(w);
    return rc == GDEFSTOR_OK ? DeclareCust(w, true) : rc;
  }));
  /* Entities in an order that is not their public ids' order. */
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = gdefstor_entity_create(w, "shop.order", 10, "o2", 2,
        "u1", 2, 100, nullptr);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_create(w, "shop.cust", 9, "c1", 2, "", 0, 110,
          nullptr);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_create(w, "shop.order", 10, "o1", 2, "u2", 2, 120,
          nullptr);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_create(w, "shop.cust", 9, "c2", 2, "u1", 2, 130,
          nullptr);
    }
    return rc;
  }));
  /* o1: revision 1 in en. */
  SNAPFIX_TRY(SaveTrees(s, tenant, "o1", 0, 1000,
      {{"en", Order("A-1", 1999, false, cipher1,
                  {{0, "red"}, {0, "blue"}},
                  {{0, "a", 1, 500}, {0, "b", 2, 750}}, pm)}},
      "first"));
  /* o1 revision 2: paid, a tag added, line b moved before line a. */
  {
    uint64_t red = IdOf(s, tenant, "o1", "en", "tags", nullptr, "red");
    uint64_t blue = IdOf(s, tenant, "o1", "en", "tags", nullptr, "blue");
    uint64_t la = IdOf(s, tenant, "o1", "en", "lines", "sku", "a");
    uint64_t lb = IdOf(s, tenant, "o1", "en", "lines", "sku", "b");
    if (red == 0 || blue == 0 || la == 0 || lb == 0) {
      ADD_FAILURE() << "o1's items were not found";
      return GDEFSTOR_ERR_INTERNAL;
    }
    SNAPFIX_TRY(SaveTrees(s, tenant, "o1", 1, 2000,
        {{"en", Order("A-1", 1999, true, cipher1,
                    {{red, "red"}, {blue, "blue"}, {0, "green"}},
                    {{lb, "b", 2, 750}, {la, "a", 1, 500}}, nullptr)}}));
    /* o1 revision 3: a French tree beside the unchanged English one. */
    SNAPFIX_TRY(SaveTrees(s, tenant, "o1", 2, 3000,
        {{"en", Order("A-1", 1999, true, cipher1,
                    {{red, "red"}, {blue, "blue"},
                        {IdOf(s, tenant, "o1", "en", "tags", nullptr, "green"),
                            "green"}},
                    {{lb, "b", 2, 750}, {la, "a", 1, 500}}, nullptr)},
            {"fr", Order("A-1-fr", 1999, true, cipher2, {{0, "rouge"}},
                       {{0, "c", 3, 125}}, pm)}},
        "traduction"));
  }
  /* o1 revision 4: back to revision 1 (the French tree goes). */
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    GDEFSTOR_RevisionMeta m = Meta("u2", 4000, "revert");
    uint64_t got = 0;
    return gdefstor_entity_revert(w, "o1", 2, 3, 1, &m, &got);
  }));
  /* o2: en, then fr added, then fr removed. */
  SNAPFIX_TRY(SaveTrees(s, tenant, "o2", 0, 1100,
      {{"en", Order("B-2", 500, false, "", {}, {}, nullptr)}}));
  SNAPFIX_TRY(SaveTrees(s, tenant, "o2", 1, 1200,
      {{"fr", Order("B-2-fr", 500, false, "", {{0, "bleu"}}, {}, nullptr)}}));
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    GDEFSTOR_RevisionMeta m = Meta("", 1300);
    uint64_t got = 0;
    return gdefstor_entity_remove_language(w, "o2", 2, 2, &m, "fr", 2, &got);
  }));
  /* c1: saved with a reference, then the reference field is retired and the
   * name changed: the retired value stays in the current tree. */
  SNAPFIX_TRY(SaveTrees(s, tenant, "c1", 0, 1500,
      {{"en", Cust("Ann", "o1")}}));
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    return DeclareCust(w, false);
  }));
  SNAPFIX_TRY(SaveTrees(s, tenant, "c1", 1, 1600,
      {{"en", Cust("Anne", nullptr)}}));
  /* Two saves of different entities in one write: one commit sequence. */
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    GDEFSTOR_RevisionMeta m = Meta("u3", 1700);
    GDEFSTOR_Tree * a = Cust("Anne", nullptr);
    GDEFSTOR_Tree * b = Order("B-2", 501, true, "", {}, {}, nullptr);
    uint64_t got = 0;
    GDEFSTOR_Result rc = gdefstor_entity_save(w, "c1", 2, 2, &m, "de", 2, a,
        &got);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_save(w, "o2", 2, 3, &m, "en", 2, b, &got);
    }
    gdefstor_tree_free(a);
    gdefstor_tree_free(b);
    return rc;
  }));
  /* States: o1 published then unpublished, o2 deleted, c2 published. */
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    int changed = 0;
    GDEFSTOR_Result rc = gdefstor_entity_set_state(w, "o1", 2, 1, 0, "u1", 2,
        5000, &changed);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_set_state(w, "o1", 2, 0, 0, "", 0, 5100, &changed);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_set_state(w, "o2", 2, 0, 1, "u2", 2, 5200,
          &changed);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_set_state(w, "c2", 2, 1, 0, "u1", 2, 5300,
          &changed);
    }
    return rc;
  }));
  /* Drafts: one of o1 on its head, one that reserves o9. */
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    static const char en[] =
        "{\"format\":\"defiant-tree/1\",\"root\":{\"number\":\"A-1-draft\"}}";
    static const char fr[] =
        "{\"format\":\"defiant-tree/1\",\"root\":{\"number\":\"\\u00e9t\\u00e9\"}}";
    GDEFSTOR_DraftTree t[2];
    std::memset(t, 0, sizeof t);
    t[0].size = sizeof t[0];
    t[0].language = "en";
    t[0].language_len = 2;
    t[0].json = reinterpret_cast<const uint8_t *>(en);
    t[0].json_len = std::strlen(en);
    t[1] = t[0];
    t[1].language = "fr";
    t[1].json = reinterpret_cast<const uint8_t *>(fr);
    t[1].json_len = std::strlen(fr);
    GDEFSTOR_Result rc = gdefstor_draft_put(w, nullptr, 0, "o1", 2, 4, t, 2);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_draft_put(w, "shop.order", 10, "o9", 2, 0, t, 1);
    }
    return rc;
  }));
  /* Records: text, binary and empty. */
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    static const uint8_t bin[] = {0x00, 0xff, 0x10, 0x80};
    static const char text[] = "{\"on\":true,\"name\":\"caf\xc3\xa9\"}";
    GDEFSTOR_Result rc = gdefstor_record_put(w, "core.module", 11, "alpha", 5,
        reinterpret_cast<const uint8_t *>(text), std::strlen(text));
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_record_put(w, "core.name", 9, "x:1", 3, bin, sizeof bin);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_record_put(w, "core.name", 9, "empty", 5, nullptr, 0);
    }
    return rc;
  }));
  /* Counters: core.epoch at 3 with 4 taken by a write that aborted. */
  for (int i = 0; i < 3; i++) {
    SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
      uint64_t v = 0;
      return gdefstor_counter_next(w, "core.epoch", 10, &v);
    }));
  }
  InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    uint64_t v = 0;
    return gdefstor_counter_next(w, "core.epoch", 10, &v);
  }, false);
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    uint64_t v = 0;
    return gdefstor_counter_next(w, "core.next", 9, &v);
  }));
  /* The outbox: p1 claimed, p2 failed once, p3 parked; q1 in another
   * stream. */
  SNAPFIX_TRY(InWrite(s, tenant, [&](GDEFSTOR_Write * w) {
    const char * ids[] = {"p1", "p2", "p3"};
    GDEFSTOR_Result rc = GDEFSTOR_OK;
    for (const char * id : ids) {
      std::string payload = std::string("{\"id\":\"") + id + "\"}";
      if (rc == GDEFSTOR_OK) {
        rc = gdefstor_model_outbox_append(w, "core.bus", 8, id, 2,
            "defiant-bus/1", 13,
            reinterpret_cast<const uint8_t *>(payload.data()), payload.size(),
            nullptr);
      }
    }
    if (rc == GDEFSTOR_OK) {
      static const uint8_t bin[] = {0xc0, 0x01};
      rc = gdefstor_model_outbox_append(w, "core.log", 8, "q1", 2,
          "defiant-outbox/1", 16, bin, sizeof bin, nullptr);
    }
    return rc;
  }));
  {
    GDEFSTOR_OutboxClaim * c = nullptr;
    SNAPFIX_TRY(gdefstor_model_outbox_claim(s, tenant, tl, "core.bus", 8, 50,
        100, 1, &c));
    size_t n = gdefstor_model_outbox_claim_count(c);
    gdefstor_model_outbox_claim_free(c);
    if (n != 1) {
      ADD_FAILURE() << "the claim took " << n << " rows";
      return GDEFSTOR_ERR_INTERNAL;
    }
    int parked = 0;
    SNAPFIX_TRY(gdefstor_model_outbox_failed(s, tenant, tl, "p2", 2, 120,
        &parked));
    for (int i = 0; i < GDEFSTOR_OUTBOX_PARK_FAILURES; i++) {
      SNAPFIX_TRY(gdefstor_model_outbox_failed(s, tenant, tl, "p3", 2, 130,
          &parked));
    }
    if (!parked) {
      ADD_FAILURE() << "p3 was not parked";
      return GDEFSTOR_ERR_INTERNAL;
    }
  }
  return GDEFSTOR_OK;
}

/* gdefstor_model_dump() as a string, or "<dump failed: ...>". */
inline std::string Dump(GDEFSTOR_Store * s, const char * tenant, int which) {
  uint8_t * text = nullptr;
  size_t len = 0;
  GDEFSTOR_Result rc = gdefstor_model_dump(s, tenant, std::strlen(tenant),
      which, &text, &len);
  if (rc != GDEFSTOR_OK) {
    return std::string("<dump failed: ") + gdefstor_result_string(rc) + ">";
  }
  std::string out(reinterpret_cast<const char *>(text), len);
  gdefstor_bytes_free(nullptr, text);
  return out;
}

/* The tenant's snapshot, taken at @p taken_ms; empty on failure (which is
 * reported). */
inline std::string Export(GDEFSTOR_Store * s, const char * tenant,
    int64_t taken_ms = 1234) {
  uint8_t * zip = nullptr;
  size_t len = 0;
  GDEFSTOR_Result rc = gdefstor_snapshot_export_at(s, tenant,
      std::strlen(tenant), nullptr, taken_ms, &zip, &len);
  if (rc != GDEFSTOR_OK) {
    ADD_FAILURE() << "export: " << gdefstor_result_string(rc);
    return std::string();
  }
  std::string out(reinterpret_cast<const char *>(zip), len);
  gdefstor_bytes_free(nullptr, zip);
  return out;
}

/* Import @p zip; @p why receives the refusal's sentence. */
inline GDEFSTOR_Result Import(GDEFSTOR_Store * s, const char * tenant,
    const std::string & zip, std::string * why = nullptr) {
  char * reason = nullptr;
  size_t len = 0;
  GDEFSTOR_Result rc = gdefstor_snapshot_import_explain(s, tenant,
      std::strlen(tenant), reinterpret_cast<const uint8_t *>(zip.data()),
      zip.size(), nullptr, &reason, &len);
  if (why != nullptr) {
    *why = reason == nullptr ? std::string() : std::string(reason, len);
  }
  gdefstor_bytes_free(nullptr, reason);
  return rc;
}

}  // namespace snapshot_fixture

#endif /* GDEFSTOR_SNAPSHOT_FIXTURE_INC */
