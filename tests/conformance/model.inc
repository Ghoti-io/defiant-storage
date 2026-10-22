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
 * The entity port's conformance suite (model.h): every backend runs it, the
 * memory backend now and each later one as it is built. It is installed beside
 * the port; the including file defines
 *
 *     GDEFSTOR_Result ModelOpen(GDEFSTOR_Store ** out,
 *         const GDEFSTOR_Allocator * alloc);
 *     void ModelClose(GDEFSTOR_Store * store);
 *
 * and includes this file after gtest. Tests state the design's rules
 * (notes/defiant-storage/ENTITY-STORAGE-DESIGN.md, spine AD-14), not what the
 * code happens to do, and a sweep checks seeded random histories against a
 * model written apart from the library (Model below).
 */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <unistd.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/* For the snapshot cases (snapshot.inc). */
#include <ghoti.io/archive/reader.h>
#include <ghoti.io/archive/sink.h>
#include <ghoti.io/archive/stream.h>
#include <ghoti.io/archive/writer.h>
#include <ghoti.io/cutil/fiber.h>

#include "snapshot_fixture.inc"

namespace model_conformance {

const char * const kTenant = "t1";
/* The language of every tree a test does not say the language of. */
const char * const kLang = "en";
const size_t kLangLen = 2;

/* ---- values -------------------------------------------------------------- */

inline GDEFSTOR_Value Text(const char * s) {
  GDEFSTOR_Value v;
  std::memset(&v, 0, sizeof v);
  v.type = GDEFSTOR_FIELD_TEXT;
  v.bytes = reinterpret_cast<const uint8_t *>(s);
  v.len = std::strlen(s);
  return v;
}

inline GDEFSTOR_Value Ref(const char * s) {
  GDEFSTOR_Value v = Text(s);
  v.type = GDEFSTOR_FIELD_REFERENCE;
  return v;
}

inline GDEFSTOR_Value Int(int type, int64_t i) {
  GDEFSTOR_Value v;
  std::memset(&v, 0, sizeof v);
  v.type = type;
  v.integer = i;
  return v;
}

inline GDEFSTOR_Value Integer(int64_t i) {
  return Int(GDEFSTOR_FIELD_INTEGER, i);
}

inline GDEFSTOR_Value Decimal(int64_t unscaled) {
  return Int(GDEFSTOR_FIELD_DECIMAL, unscaled);
}

inline GDEFSTOR_Value Real(double d) {
  GDEFSTOR_Value v;
  std::memset(&v, 0, sizeof v);
  v.type = GDEFSTOR_FIELD_FLOAT;
  v.real = d;
  return v;
}

inline GDEFSTOR_Value Personal(int type, const char * cipher,
    const char * subject) {
  GDEFSTOR_Value v = Text(cipher);
  v.type = type;
  v.subject = reinterpret_cast<const uint8_t *>(subject);
  v.subject_len = std::strlen(subject);
  return v;
}

/* A value the test owns. */
struct Val {
  int type = 0;
  int64_t i = 0;
  double f = 0.0;
  std::string b;
  std::string s;

  static Val From(const GDEFSTOR_Value & v) {
    Val x;
    x.type = v.type;
    if (v.subject_len > 0) {
      x.s.assign(reinterpret_cast<const char *>(v.subject), v.subject_len);
      x.b.assign(reinterpret_cast<const char *>(v.bytes), v.len);
      return x;
    }
    switch (v.type) {
    case GDEFSTOR_FIELD_TEXT:
    case GDEFSTOR_FIELD_REFERENCE:
      x.b.assign(reinterpret_cast<const char *>(v.bytes), v.len);
      break;
    case GDEFSTOR_FIELD_FLOAT:
      x.f = v.real == 0.0 ? 0.0 : v.real;
      break;
    default:
      x.i = v.integer;
    }
    return x;
  }

  GDEFSTOR_Value Lend() const {
    GDEFSTOR_Value v;
    std::memset(&v, 0, sizeof v);
    v.type = type;
    v.integer = i;
    v.real = f;
    v.bytes = reinterpret_cast<const uint8_t *>(b.data());
    v.len = b.size();
    v.subject = reinterpret_cast<const uint8_t *>(s.data());
    v.subject_len = s.size();
    return v;
  }

  bool operator==(const Val & o) const {
    return type == o.type && i == o.i && f == o.f && b == o.b && s == o.s;
  }
  bool operator!=(const Val & o) const { return !(*this == o); }
  bool operator<(const Val & o) const {
    if (type != o.type) {
      return type < o.type;
    }
    if (type == GDEFSTOR_FIELD_TEXT || type == GDEFSTOR_FIELD_REFERENCE) {
      return b < o.b;
    }
    if (type == GDEFSTOR_FIELD_FLOAT) {
      return f < o.f;
    }
    return i < o.i;
  }
};

/* ---- declarations -------------------------------------------------------- */

struct Field {
  std::string name;
  int type = GDEFSTOR_FIELD_TEXT;
  int scale = 0;
  unsigned flags = 0;
  std::string subject;
  std::vector<Field> kids;
};

inline Field F(const char * name, int type, unsigned flags = 0, int scale = 0) {
  Field f;
  f.name = name;
  f.type = type;
  f.flags = flags;
  f.scale = scale;
  return f;
}

inline Field G(const char * name, unsigned flags, std::vector<Field> kids) {
  Field f;
  f.name = name;
  f.type = GDEFSTOR_FIELD_GROUP;
  f.flags = flags;
  f.kids = std::move(kids);
  return f;
}

/* The GDEFSTOR_FieldSpec arrays of a declaration, kept alive together with
 * the fields they point into. */
struct SpecHolder {
  std::vector<Field> owned;
  std::deque<std::vector<GDEFSTOR_FieldSpec>> arrays;
  GDEFSTOR_TypeSpec spec;
  std::string name;

  const GDEFSTOR_FieldSpec * Build(const std::vector<Field> & fields) {
    arrays.emplace_back(fields.size());
    std::vector<GDEFSTOR_FieldSpec> & out = arrays.back();
    for (size_t i = 0; i < fields.size(); i++) {
      std::memset(&out[i], 0, sizeof out[i]);
      out[i].size = sizeof out[i];
      out[i].name = fields[i].name.data();
      out[i].name_len = fields[i].name.size();
      out[i].type = fields[i].type;
      out[i].scale = fields[i].scale;
      out[i].flags = fields[i].flags;
      if (!fields[i].subject.empty()) {
        out[i].subject = fields[i].subject.data();
        out[i].subject_len = fields[i].subject.size();
      }
      if (!fields[i].kids.empty()) {
        out[i].children = Build(fields[i].kids);
        out[i].child_count = fields[i].kids.size();
      }
    }
    return out.empty() ? nullptr : out.data();
  }

  SpecHolder(const std::string & type, const std::vector<Field> & fields)
      : owned(fields), name(type) {
    std::memset(&spec, 0, sizeof spec);
    spec.size = sizeof spec;
    spec.name = name.data();
    spec.name_len = name.size();
    spec.fields = Build(owned);
    spec.field_count = owned.size();
  }
  SpecHolder(const SpecHolder &) = delete;
  SpecHolder & operator=(const SpecHolder &) = delete;
};

/* ---- the store ----------------------------------------------------------- */

inline GDEFSTOR_RevisionMeta Meta(const char * who = "u1", int64_t when = 1000,
    const char * why = "") {
  GDEFSTOR_RevisionMeta m;
  std::memset(&m, 0, sizeof m);
  m.size = sizeof m;
  m.who = who;
  m.who_len = std::strlen(who);
  m.when_ms = when;
  m.why = why;
  m.why_len = std::strlen(why);
  m.schema = kStamp;
  m.schema_len = kStampLen;
  return m;
}

/* A draft of one language's tree. */
inline GDEFSTOR_Result DraftPut(GDEFSTOR_Write * w, const char * type,
    size_t type_len, const char * pid, size_t pid_len, uint64_t base,
    const uint8_t * json, size_t json_len, const char * lang = kLang) {
  GDEFSTOR_DraftTree t;
  std::memset(&t, 0, sizeof t);
  t.size = sizeof t;
  t.language = lang;
  t.language_len = std::strlen(lang);
  t.json = json;
  t.json_len = json_len;
  return gdefstor_draft_put(w, type, type_len, pid, pid_len, base, kStamp, kStampLen, &t, 1);
}

class ModelTest : public ::testing::Test {
 public:
  GDEFSTOR_Store * store = nullptr;

  void SetUp() override {
    ASSERT_EQ(GDEFSTOR_OK, ModelOpen(&store, nullptr));
  }
  void TearDown() override {
    ModelClose(store);
  }

  /* Run @p body in one write and commit it; the body's result, else the
   * commit's. */
  GDEFSTOR_Result InWrite(const std::function<GDEFSTOR_Result(GDEFSTOR_Write *)> & body) {
    GDEFSTOR_Write * w = nullptr;
    GDEFSTOR_Result rc = gdefstor_write_begin(store, kTenant, 2, nullptr, &w);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    rc = body(w);
    if (rc != GDEFSTOR_OK) {
      gdefstor_write_abort(w);
      return rc;
    }
    return gdefstor_write_commit(w);
  }

  GDEFSTOR_Result Declare(const std::string & type,
      const std::vector<Field> & fields) {
    SpecHolder h(type, fields);
    return InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_type_declare(w, &h.spec);
    });
  }

  GDEFSTOR_Result Create(const std::string & type, const std::string & pid,
      const char * creator = "u1", int64_t when = 500) {
    return InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_create(w, type.data(), type.size(), pid.data(),
          pid.size(), creator, std::strlen(creator), when, nullptr);
    });
  }

  GDEFSTOR_Result Save(const std::string & pid, uint64_t base,
      const GDEFSTOR_Tree * tree, uint64_t * out = nullptr,
      const char * who = "u1") {
    uint64_t got = 0;
    GDEFSTOR_RevisionMeta m = Meta(who, 2000 + static_cast<int64_t>(base));
    GDEFSTOR_Result rc = InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_save(w, pid.data(), pid.size(), base, &m, kLang, kLangLen, tree,
          &got);
    });
    if (out != nullptr) {
      *out = got;
    }
    return rc;
  }

  GDEFSTOR_Result Revert(const std::string & pid, uint64_t base, uint64_t to,
      uint64_t * out = nullptr) {
    uint64_t got = 0;
    GDEFSTOR_RevisionMeta m = Meta("u2", 3000, "revert");
    GDEFSTOR_Result rc = InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_revert(w, pid.data(), pid.size(), base, to, &m,
          &got);
    });
    if (out != nullptr) {
      *out = got;
    }
    return rc;
  }

  uint64_t Head(const std::string & pid) {
    GDEFSTOR_Entity * e = nullptr;
    GDEFSTOR_EntityInfo info;
    std::memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (gdefstor_entity_get(store, kTenant, 2, pid.data(), pid.size(), &e) !=
        GDEFSTOR_OK) {
      return UINT64_MAX;
    }
    gdefstor_entity_info(e, &info);
    uint64_t r = info.revision;
    gdefstor_entity_free(e);
    return r;
  }

  std::string Dump(int which) {
    uint8_t * text = nullptr;
    size_t len = 0;
    if (gdefstor_model_dump(store, kTenant, 2, which, &text, &len) !=
        GDEFSTOR_OK) {
      return "<dump failed>";
    }
    std::string s(reinterpret_cast<const char *>(text), len);
    gdefstor_bytes_free(nullptr, text);
    return s;
  }
};

/* Lines of a dump that start with @p tag. */
inline std::vector<std::string> Lines(const std::string & dump, char tag) {
  std::vector<std::string> out;
  std::istringstream in(dump);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line[0] == tag) {
      out.push_back(line);
    }
  }
  return out;
}

inline std::vector<std::string> Words(const std::string & line) {
  std::vector<std::string> out;
  std::istringstream in(line);
  std::string w;
  while (in >> w) {
    out.push_back(w);
  }
  return out;
}

/* Truth only grows: every revision and state row stays as it was, and every
 * item version stays as it was except that an open valid_to (0) may be set
 * once. Returns "" or what broke the rule. */
inline std::string AppendOnly(const std::string & before,
    const std::string & after) {
  for (char tag : {'R', 'S', 'T'}) {
    std::set<std::string> now;
    for (const std::string & l : Lines(after, tag)) {
      now.insert(l);
    }
    for (const std::string & l : Lines(before, tag)) {
      if (tag != 'T' && now.count(l) == 0) {
        return std::string("a row changed or went: ") + l;
      }
    }
  }
  std::map<std::string, std::vector<std::string>> versions;
  for (const std::string & l : Lines(after, 'I')) {
    std::vector<std::string> w = Words(l);
    /* I row group item from to ... */
    std::string key = w[1] + " " + w[2] + " " + w[3] + " " + w[4];
    versions[key] = w;
  }
  for (const std::string & l : Lines(before, 'I')) {
    std::vector<std::string> w = Words(l);
    std::string key = w[1] + " " + w[2] + " " + w[3] + " " + w[4];
    auto it = versions.find(key);
    if (it == versions.end()) {
      return "an item version went: " + l;
    }
    std::vector<std::string> a = w;
    std::vector<std::string> b = it->second;
    if (a.size() != b.size()) {
      return "an item version changed: " + l;
    }
    for (size_t k = 0; k < a.size(); k++) {
      if (k == 5) {
        if (a[k] != b[k] && a[k] != "0") {
          return "a valid_to was written a second time: " + l;
        }
        continue;
      }
      if (a[k] != b[k]) {
        return "an item version changed: " + l;
      }
    }
  }
  return "";
}

/* Rows of a tag in @p after that are not in @p before. */
inline size_t NewRows(const std::string & before, const std::string & after,
    char tag) {
  std::set<std::string> was;
  size_t n = 0;
  if (tag == 'I') {
    for (const std::string & l : Lines(before, 'I')) {
      std::vector<std::string> w = Words(l);
      was.insert(w[1] + " " + w[2] + " " + w[3] + " " + w[4]);
    }
    for (const std::string & l : Lines(after, 'I')) {
      std::vector<std::string> w = Words(l);
      if (was.count(w[1] + " " + w[2] + " " + w[3] + " " + w[4]) == 0) {
        n += 1;
      }
    }
    return n;
  }
  for (const std::string & l : Lines(before, tag)) {
    was.insert(l);
  }
  for (const std::string & l : Lines(after, tag)) {
    if (was.count(l) == 0) {
      n += 1;
    }
  }
  return n;
}

/* ---- trees as values the test can compare -------------------------------- */

struct Node {
  uint64_t id = 0;
  std::map<std::string, Val> vals;                 /* "" is an item's own value */
  std::map<std::string, std::vector<Node>> kids;   /* by field, in order */
  /* Blind-index MACs to give values when the tree is built; never read back,
   * so not part of what a tree is. */
  std::map<std::string, std::string> macs;

  /* An empty list of items under a field is no items: the same tree. */
  bool operator==(const Node & o) const {
    if (id != o.id || vals != o.vals) {
      return false;
    }
    for (const auto & kv : kids) {
      auto it = o.kids.find(kv.first);
      if (it == o.kids.end() ? !kv.second.empty() : it->second != kv.second) {
        return false;
      }
    }
    for (const auto & kv : o.kids) {
      if (kids.count(kv.first) == 0 && !kv.second.empty()) {
        return false;
      }
    }
    return true;
  }
  bool operator!=(const Node & o) const { return !(*this == o); }

  Node & Set(const std::string & field, const GDEFSTOR_Value & v) {
    vals[field] = Val::From(v);
    macs.erase(field);
    return *this;
  }
  /* Give the value of @p field its MAC (set the value first). */
  Node & Mac(const std::string & field, const std::string & mac) {
    macs[field] = mac;
    return *this;
  }
  Node & Add(const std::string & field, Node n) {
    kids[field].push_back(std::move(n));
    return kids[field].back();
  }
  Node & Add(const std::string & field);
};

inline Node & Node::Add(const std::string & field) {
  return Add(field, Node());
}

inline void PrintTo(const Val & v, std::ostream * os) {
  *os << "v" << v.type << ":";
  if (v.type == GDEFSTOR_FIELD_TEXT || v.type == GDEFSTOR_FIELD_REFERENCE ||
      !v.s.empty()) {
    *os << '"' << v.b << '"';
    if (!v.s.empty()) {
      *os << "@" << v.s;
    }
  } else if (v.type == GDEFSTOR_FIELD_FLOAT) {
    *os << v.f;
  } else {
    *os << v.i;
  }
}

inline void PrintTo(const Node & n, std::ostream * os) {
  *os << "{#" << n.id;
  for (const auto & kv : n.vals) {
    *os << " " << kv.first << "=";
    PrintTo(kv.second, os);
  }
  for (const auto & kv : n.kids) {
    *os << " " << kv.first << ":[";
    for (const Node & k : kv.second) {
      PrintTo(k, os);
    }
    *os << "]";
  }
  *os << "}";
}

inline Node Item(uint64_t id = 0) {
  Node n;
  n.id = id;
  return n;
}

inline Node One(const GDEFSTOR_Value & v, uint64_t id = 0) {
  Node n;
  n.id = id;
  n.vals[""] = Val::From(v);
  return n;
}

/* The key id and normalization version every MAC of the suite is given. */
const uint32_t kMacKey = 7;
const uint32_t kMacVersion = 2;

inline void Emit(GDEFSTOR_Tree * t, size_t at, const Node & n) {
  for (const auto & kv : n.vals) {
    GDEFSTOR_Value v = kv.second.Lend();
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_set(t, at, kv.first.data(),
        kv.first.size(), &v));
  }
  for (const auto & kv : n.macs) {
    GDEFSTOR_BlindMac m;
    std::memset(&m, 0, sizeof m);
    m.size = sizeof m;
    m.mac = reinterpret_cast<const uint8_t *>(kv.second.data());
    m.mac_len = kv.second.size();
    m.key_id = kMacKey;
    m.version = kMacVersion;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_set_blind(t, at, kv.first.data(),
        kv.first.size(), &m));
  }
  for (const auto & kv : n.kids) {
    for (const Node & k : kv.second) {
      size_t child = 0;
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_add(t, at, kv.first.data(),
          kv.first.size(), k.id, &child));
      Emit(t, child, k);
    }
  }
}

/* A tree built from @p n; free it with gdefstor_tree_free(). */
inline GDEFSTOR_Tree * Build(const Node & n) {
  GDEFSTOR_Tree * t = nullptr;
  if (gdefstor_tree_create(nullptr, &t) != GDEFSTOR_OK) {
    return nullptr;
  }
  Emit(t, 0, n);
  return t;
}

inline Node Read(const GDEFSTOR_Tree * t, size_t at = 0) {
  Node n;
  GDEFSTOR_TreeItem it;
  std::memset(&it, 0, sizeof it);
  it.size = sizeof it;
  gdefstor_tree_item(t, at, &it);
  n.id = it.id;
  for (size_t k = 0; k < it.value_count; k++) {
    const char * f = nullptr;
    size_t fl = 0;
    GDEFSTOR_Value v;
    gdefstor_tree_value(t, at, k, &f, &fl, &v);
    n.vals[std::string(f, fl)] = Val::From(v);
  }
  for (size_t i = at + 1; i < gdefstor_tree_item_count(t); i++) {
    GDEFSTOR_TreeItem c;
    std::memset(&c, 0, sizeof c);
    c.size = sizeof c;
    gdefstor_tree_item(t, i, &c);
    if (c.parent == at) {
      n.kids[std::string(c.field, c.field_len)].push_back(Read(t, i));
    }
  }
  return n;
}

/* The order keys of the items under @p field of the item with id @p parent. */
inline std::vector<double> Keys(const GDEFSTOR_Tree * t, uint64_t parent,
    const std::string & field) {
  std::vector<double> out;
  size_t at = SIZE_MAX;
  for (size_t i = 0; i < gdefstor_tree_item_count(t); i++) {
    GDEFSTOR_TreeItem c;
    std::memset(&c, 0, sizeof c);
    c.size = sizeof c;
    gdefstor_tree_item(t, i, &c);
    if (c.id == parent) {
      at = i;
    }
  }
  for (size_t i = 0; i < gdefstor_tree_item_count(t); i++) {
    GDEFSTOR_TreeItem c;
    std::memset(&c, 0, sizeof c);
    c.size = sizeof c;
    gdefstor_tree_item(t, i, &c);
    if (c.parent == at && std::string(c.field, c.field_len) == field) {
      out.push_back(c.order);
    }
  }
  return out;
}

/* ---- the purchase order, the design's own example ------------------------ */

const char * const kPo = "site.purchase_order";

inline std::vector<Field> PurchaseOrder() {
  const unsigned M = GDEFSTOR_FIELD_MULTIPLE;
  return {
      F("number", GDEFSTOR_FIELD_TEXT,
          GDEFSTOR_FIELD_REQUIRED | GDEFSTOR_FIELD_INDEXED),
      F("placed", GDEFSTOR_FIELD_DATE),
      F("total", GDEFSTOR_FIELD_DECIMAL, 0, 2),
      F("customer", GDEFSTOR_FIELD_REFERENCE, GDEFSTOR_FIELD_INDEXED),
      F("notes", GDEFSTOR_FIELD_TEXT, M),
      G("lines", M,
          {F("sku", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_REQUIRED),
              F("qty", GDEFSTOR_FIELD_INTEGER),
              F("price", GDEFSTOR_FIELD_DECIMAL, 0, 2),
              G("taxes", M,
                  {F("name", GDEFSTOR_FIELD_TEXT),
                      F("rate", GDEFSTOR_FIELD_DECIMAL, 0, 4),
                      F("amount", GDEFSTOR_FIELD_DECIMAL, 0, 2)}),
              G("discounts", M,
                  {F("code", GDEFSTOR_FIELD_TEXT),
                      F("amount", GDEFSTOR_FIELD_DECIMAL, 0, 2),
                      G("breakdown", M,
                          {F("reason", GDEFSTOR_FIELD_TEXT),
                              F("amount", GDEFSTOR_FIELD_DECIMAL, 0, 2)})})}),
      G("shipments", M,
          {F("carrier", GDEFSTOR_FIELD_TEXT),
              G("packages", M,
                  {F("weight", GDEFSTOR_FIELD_FLOAT),
                      G("tracking", M,
                          {F("at", GDEFSTOR_FIELD_DATETIME),
                              F("status", GDEFSTOR_FIELD_TEXT)})})}),
  };
}

inline Node Line(const char * sku, int64_t qty, int64_t price_cents) {
  Node l;
  l.Set("sku", Text(sku)).Set("qty", Integer(qty)).Set("price",
      Decimal(price_cents));
  l.Add("taxes").Set("name", Text("vat")).Set("rate", Decimal(2000)).Set(
      "amount", Decimal(price_cents * qty / 5));
  Node & d = l.Add("discounts");
  d.Set("code", Text("spring")).Set("amount", Decimal(100));
  d.Add("breakdown").Set("reason", Text("loyalty")).Set("amount", Decimal(60));
  d.Add("breakdown").Set("reason", Text("season")).Set("amount", Decimal(40));
  return l;
}

inline Node PoTree(const char * number, int64_t total_cents,
    const char * customer) {
  Node po;
  po.Set("number", Text(number)).Set("placed",
      Int(GDEFSTOR_FIELD_DATE, 20378)).Set("total", Decimal(total_cents));
  po.Set("customer", Ref(customer));
  po.Add("notes", One(Text("fragile")));
  po.Add("notes", One(Text("call first")));
  po.Add("lines", Line("A-1", 2, 1250));
  po.Add("lines", Line("B-2", 1, 999));
  Node & s = po.Add("shipments");
  s.Set("carrier", Text("post"));
  Node & p = s.Add("packages");
  p.Set("weight", Real(1.5));
  p.Add("tracking").Set("at", Int(GDEFSTOR_FIELD_DATETIME,
      INT64_C(1760000000000))).Set("status", Text("sent"));
  return po;
}

/* ---- declarations -------------------------------------------------------- */

class ModelDeclare : public ModelTest {};

TEST_F(ModelDeclare, AddingAFieldWorks) {
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("title", GDEFSTOR_FIELD_TEXT)}));
  ASSERT_EQ(GDEFSTOR_OK, Create("site.note", "n1"));
  Node a;
  a.Set("title", Text("one"));
  GDEFSTOR_Tree * t = Build(a);
  ASSERT_EQ(GDEFSTOR_OK, Save("n1", 0, t));
  gdefstor_tree_free(t);
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("title", GDEFSTOR_FIELD_TEXT),
      F("rank", GDEFSTOR_FIELD_INTEGER)}));
  GDEFSTOR_Tree * cur = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "n1", 2, kLang, kLangLen, 0, &cur));
  Node b = Read(cur);
  gdefstor_tree_free(cur);
  EXPECT_EQ(Val::From(Text("one")), b.vals["title"]);
  b.Set("rank", Integer(7));
  t = Build(b);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, Save("n1", 1, t, &rev));
  gdefstor_tree_free(t);
  EXPECT_EQ(2u, rev);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "n1", 2, kLang, kLangLen, 0, &cur));
  EXPECT_EQ(Val::From(Integer(7)), Read(cur).vals["rank"]);
  gdefstor_tree_free(cur);
  /* Revision 1 was written before the field existed and still reads so. */
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "n1", 2, kLang, kLangLen, 1, &cur));
  EXPECT_EQ(0u, Read(cur).vals.count("rank"));
  gdefstor_tree_free(cur);
}

TEST_F(ModelDeclare, RetiredFieldDataStaysReadable) {
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("title", GDEFSTOR_FIELD_TEXT),
      F("old", GDEFSTOR_FIELD_TEXT), G("parts", GDEFSTOR_FIELD_MULTIPLE,
          {F("p", GDEFSTOR_FIELD_INTEGER)})}));
  ASSERT_EQ(GDEFSTOR_OK, Create("site.note", "n1"));
  Node a;
  a.Set("title", Text("one")).Set("old", Text("keep me"));
  a.Add("parts").Set("p", Integer(1));
  GDEFSTOR_Tree * t = Build(a);
  ASSERT_EQ(GDEFSTOR_OK, Save("n1", 0, t));
  gdefstor_tree_free(t);
  /* Retire "old" and "parts" by leaving them out. */
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("title", GDEFSTOR_FIELD_TEXT)}));
  GDEFSTOR_Type * type = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(store, kTenant, 2, "site.note", 9,
      &type));
  size_t at = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_find(type, "old", 3, &at));
  GDEFSTOR_FieldInfo info;
  std::memset(&info, 0, sizeof info);
  info.size = sizeof info;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_field(type, at, &info));
  EXPECT_TRUE(info.retired);
  gdefstor_type_free(type);
  /* A save that does not mention them changes the rest and keeps them. */
  Node b;
  b.Set("title", Text("two"));
  t = Build(b);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, Save("n1", 1, t, &rev));
  gdefstor_tree_free(t);
  ASSERT_EQ(2u, rev);
  GDEFSTOR_Tree * cur = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "n1", 2, kLang, kLangLen, 0, &cur));
  Node now = Read(cur);
  gdefstor_tree_free(cur);
  EXPECT_EQ(Val::From(Text("two")), now.vals["title"]);
  EXPECT_EQ(Val::From(Text("keep me")), now.vals["old"]);
  ASSERT_EQ(1u, now.kids["parts"].size());
  EXPECT_EQ(Val::From(Integer(1)), now.kids["parts"][0].vals["p"]);
  /* Writing a retired field is refused, and restating it is not a change. */
  Node c = now;
  c.Set("old", Text("changed"));
  t = Build(c);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Save("n1", 2, t));
  gdefstor_tree_free(t);
  t = Build(now);
  ASSERT_EQ(GDEFSTOR_OK, Save("n1", 2, t, &rev));
  gdefstor_tree_free(t);
  EXPECT_EQ(0u, rev);
  /* Declaring it again brings it back, data and all. */
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("title", GDEFSTOR_FIELD_TEXT),
      F("old", GDEFSTOR_FIELD_TEXT), G("parts", GDEFSTOR_FIELD_MULTIPLE,
          {F("p", GDEFSTOR_FIELD_INTEGER)})}));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "n1", 2, kLang, kLangLen, 0, &cur));
  EXPECT_EQ(Val::From(Text("keep me")), Read(cur).vals["old"]);
  gdefstor_tree_free(cur);
}

TEST_F(ModelDeclare, ChangingATypeIsRefused) {
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("rank", GDEFSTOR_FIELD_INTEGER),
      F("price", GDEFSTOR_FIELD_DECIMAL, 0, 2), F("tags", GDEFSTOR_FIELD_TEXT,
          GDEFSTOR_FIELD_MULTIPLE)}));
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, Declare("site.note",
      {F("rank", GDEFSTOR_FIELD_TEXT), F("price", GDEFSTOR_FIELD_DECIMAL, 0, 2),
          F("tags", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_MULTIPLE)}));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, Declare("site.note",
      {F("rank", GDEFSTOR_FIELD_INTEGER), F("price", GDEFSTOR_FIELD_DECIMAL, 0, 3),
          F("tags", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_MULTIPLE)}));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, Declare("site.note",
      {F("rank", GDEFSTOR_FIELD_INTEGER), F("price", GDEFSTOR_FIELD_DECIMAL, 0, 2),
          F("tags", GDEFSTOR_FIELD_TEXT)}));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, Declare("site.note",
      {F("rank", GDEFSTOR_FIELD_INTEGER, GDEFSTOR_FIELD_PERSONAL),
          F("price", GDEFSTOR_FIELD_DECIMAL, 0, 2),
          F("tags", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_MULTIPLE)}));
  /* A retired field keeps its name: it cannot come back as another type. */
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("price",
      GDEFSTOR_FIELD_DECIMAL, 0, 2), F("tags", GDEFSTOR_FIELD_TEXT,
          GDEFSTOR_FIELD_MULTIPLE)}));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, Declare("site.note",
      {F("rank", GDEFSTOR_FIELD_FLOAT), F("price", GDEFSTOR_FIELD_DECIMAL, 0, 2)}));
  /* Required and indexed may change. */
  EXPECT_EQ(GDEFSTOR_OK, Declare("site.note", {F("rank", GDEFSTOR_FIELD_INTEGER,
      GDEFSTOR_FIELD_INDEXED), F("price", GDEFSTOR_FIELD_DECIMAL, 0, 2),
          F("tags", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_MULTIPLE)}));
  (void)before;
}

TEST_F(ModelDeclare, ATypeKnowsItsName) {
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("title", GDEFSTOR_FIELD_TEXT)}));
  GDEFSTOR_Type * type = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(store, kTenant, 2, "site.note", 9,
      &type));
  const char * name = nullptr;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_name(type, &name, &len));
  EXPECT_EQ(std::string("site.note"), std::string(name, len));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_type_name(type, nullptr, &len));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_type_name(nullptr, &name, &len));
  gdefstor_type_free(type);
}

/* AD-15: an index is built after the declaring write commits, and the field
 * reads "indexing" until it is. With nothing in the way, the build is done by
 * the time the declaring write's commit returns. */
TEST_F(ModelDeclare, AnIndexedFieldReadsReadyOnceItsDeclarationCommits) {
  const unsigned IX = GDEFSTOR_FIELD_INDEXED;
  auto state = [&](const char * type, const char * path, int * out) {
    return gdefstor_type_index_state(store, kTenant, 2, type, std::strlen(type),
        path, std::strlen(path), out);
  };
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note",
      {F("title", GDEFSTOR_FIELD_TEXT, IX), F("body", GDEFSTOR_FIELD_TEXT),
          G("lines", GDEFSTOR_FIELD_MULTIPLE,
              {F("sku", GDEFSTOR_FIELD_TEXT, IX),
                  F("qty", GDEFSTOR_FIELD_INTEGER)}),
          F("tags", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_MULTIPLE | IX)}));
  int s = -1;
  ASSERT_EQ(GDEFSTOR_OK, state("site.note", "title", &s));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, s);
  ASSERT_EQ(GDEFSTOR_OK, state("site.note", "lines.sku", &s));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, s);
  ASSERT_EQ(GDEFSTOR_OK, state("site.note", "tags", &s));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, s);
  ASSERT_EQ(GDEFSTOR_OK, state("site.note", "body", &s));
  EXPECT_EQ(GDEFSTOR_INDEX_NONE, s);
  ASSERT_EQ(GDEFSTOR_OK, state("site.note", "lines", &s));
  EXPECT_EQ(GDEFSTOR_INDEX_NONE, s);
  /* INDEXED may change: a field that becomes indexed is built in turn. */
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note",
      {F("title", GDEFSTOR_FIELD_TEXT, IX), F("body", GDEFSTOR_FIELD_TEXT, IX),
          G("lines", GDEFSTOR_FIELD_MULTIPLE,
              {F("sku", GDEFSTOR_FIELD_TEXT, IX),
                  F("qty", GDEFSTOR_FIELD_INTEGER)}),
          F("tags", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_MULTIPLE | IX)}));
  ASSERT_EQ(GDEFSTOR_OK, state("site.note", "body", &s));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, s);
  s = -1;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, state("site.note", "nope", &s));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, state("site.other", "title", &s));
  EXPECT_EQ(-1, s);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, state("site.note", "title", nullptr));
}

TEST_F(ModelDeclare, AnEqualSpecWritesNothing) {
  ASSERT_EQ(GDEFSTOR_OK, Declare(kPo, PurchaseOrder()));
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  ASSERT_EQ(GDEFSTOR_OK, Declare(kPo, PurchaseOrder()));
  EXPECT_EQ(before, Dump(GDEFSTOR_MODEL_TRUTH));
}

TEST_F(ModelDeclare, NamesAndShapesAreChecked) {
  const int T = GDEFSTOR_FIELD_TEXT;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("nodot", {F("a", T)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("a.b.c", {F("a", T)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {F("bad name", T)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {F("", T)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {F("a.b", T)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {F("Title", T), F("title", T)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {F("a", 0)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {F("a", 10)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {F("a", T, 0x100)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {F("a", GDEFSTOR_FIELD_DECIMAL, 0, 19)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {F("a", T, 0, 2)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {G("g", 0, {})}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {G("g", GDEFSTOR_FIELD_PERSONAL, {F("a", T)})}));
  /* A personal field's subject is a sibling single reference that is not
   * personal itself. */
  Field p = F("note", T, GDEFSTOR_FIELD_PERSONAL);
  p.subject = "donor";
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {p}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {p, F("donor", T)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.x", {p, F("donor",
      GDEFSTOR_FIELD_REFERENCE, GDEFSTOR_FIELD_MULTIPLE)}));
  EXPECT_EQ(GDEFSTOR_OK, Declare("site.x", {p, F("donor", GDEFSTOR_FIELD_REFERENCE)}));
  Field q = F("plain", T);
  q.subject = "donor";
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.y", {q, F("donor",
      GDEFSTOR_FIELD_REFERENCE)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.y", {F("a", T,
      GDEFSTOR_FIELD_PERSONAL | GDEFSTOR_FIELD_INDEXED)}));
  /* A new field may not differ from a stored one by case alone. */
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.z", {F("title", T)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Declare("site.z", {F("Title", T)}));
}

TEST_F(ModelDeclare, LimitsAreRefusedNotCrashed) {
  /* Nine groups deep is one too many. */
  Field leaf = F("x", GDEFSTOR_FIELD_TEXT);
  Field g = leaf;
  for (int d = 0; d < GDEFSTOR_MODEL_MAX_DEPTH; d++) {
    g = G("g", 0, {g});
  }
  EXPECT_EQ(GDEFSTOR_OK, Declare("site.deep", {g}));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, Declare("site.deeper", {G("g", 0, {g})}));
  std::vector<Field> wide;
  for (int i = 0; i <= GDEFSTOR_MODEL_MAX_GROUP_FIELDS; i++) {
    wide.push_back(F(("f" + std::to_string(i)).c_str(), GDEFSTOR_FIELD_TEXT));
  }
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, Declare("site.wide", wide));
  wide.pop_back();
  EXPECT_EQ(GDEFSTOR_OK, Declare("site.wide", wide));
  /* Retired fields count: a 65th sibling beside 64 stored ones is refused. */
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, Declare("site.wide", {F("other",
      GDEFSTOR_FIELD_TEXT)}));
}

/* ---- entities and states ------------------------------------------------- */

class ModelEntity : public ModelTest {};

TEST_F(ModelEntity, CreateRecordsCreatorAndTime) {
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("title", GDEFSTOR_FIELD_TEXT)}));
  ASSERT_EQ(GDEFSTOR_OK, Create("site.note", "n1", "alice", 1234));
  GDEFSTOR_Entity * e = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_get(store, kTenant, 2, "n1", 2, &e));
  GDEFSTOR_EntityInfo info;
  std::memset(&info, 0, sizeof info);
  info.size = sizeof info;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_info(e, &info));
  EXPECT_EQ("site.note", std::string(info.type, info.type_len));
  EXPECT_EQ("alice", std::string(info.creator, info.creator_len));
  EXPECT_EQ(1234, info.created_ms);
  EXPECT_EQ(0u, info.revision);
  EXPECT_EQ(0, info.published);
  EXPECT_NE(0u, info.row);
  gdefstor_entity_free(e);
  /* No revision yet: no tree. */
  GDEFSTOR_Tree * t = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_tree(store, kTenant, 2, "n1",
      2, kLang, kLangLen, 0, &t));
  /* Undeclared types and malformed ids are refused. */
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Create("site.none", "n2"));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Create("site.note", "n 2"));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Create("site.note", ""));
}

TEST_F(ModelEntity, PublicIdIsUniqueAcrossTypes) {
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.a", {F("x", GDEFSTOR_FIELD_TEXT)}));
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.b", {F("x", GDEFSTOR_FIELD_TEXT)}));
  ASSERT_EQ(GDEFSTOR_OK, Create("site.a", "id1"));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Create("site.a", "id1"));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Create("site.b", "id1"));
  /* In one write too. */
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, InWrite([&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = gdefstor_entity_create(w, "site.b", 6, "id2", 3, "", 0,
        1, nullptr);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    return gdefstor_entity_create(w, "site.a", 6, "id2", 3, "", 0, 1, nullptr);
  }));
  /* Rows grow with creation order. */
  ASSERT_EQ(GDEFSTOR_OK, Create("site.b", "id3"));
  GDEFSTOR_Entity * a = nullptr;
  GDEFSTOR_Entity * b = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_get(store, kTenant, 2, "id1", 3, &a));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_get(store, kTenant, 2, "id3", 3, &b));
  GDEFSTOR_EntityInfo ia;
  GDEFSTOR_EntityInfo ib;
  std::memset(&ia, 0, sizeof ia);
  std::memset(&ib, 0, sizeof ib);
  ia.size = sizeof ia;
  ib.size = sizeof ib;
  gdefstor_entity_info(a, &ia);
  gdefstor_entity_info(b, &ib);
  EXPECT_LT(ia.row, ib.row);
  gdefstor_entity_free(a);
  gdefstor_entity_free(b);
  GDEFSTOR_Entity * none = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_get(store, kTenant, 2, "id2",
      3, &none));
}

TEST_F(ModelEntity, StateChangesAreAppended) {
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.note", {F("title", GDEFSTOR_FIELD_TEXT)}));
  ASSERT_EQ(GDEFSTOR_OK, Create("site.note", "n1"));
  int changed = -1;
  auto set = [&](int pub, int del, const char * who, int64_t when) {
    return InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_set_state(w, "n1", 2, pub, del, who,
          std::strlen(who), when, &changed);
    });
  };
  ASSERT_EQ(GDEFSTOR_OK, set(1, 0, "ed", 10));
  EXPECT_EQ(1, changed);
  ASSERT_EQ(GDEFSTOR_OK, set(1, 0, "ed", 11));
  EXPECT_EQ(0, changed); /* the same state appends nothing */
  ASSERT_EQ(GDEFSTOR_OK, set(1, 1, "mod", 12));
  EXPECT_EQ(1, changed);
  GDEFSTOR_History * h = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_history(store, kTenant, 2, "n1", 2, nullptr, 0, &h));
  ASSERT_EQ(2u, gdefstor_history_state_count(h));
  GDEFSTOR_StateInfo s;
  std::memset(&s, 0, sizeof s);
  s.size = sizeof s;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_history_state(h, 0, &s));
  EXPECT_EQ(1, s.published);
  EXPECT_EQ(0, s.deleted);
  EXPECT_EQ("ed", std::string(s.actor, s.actor_len));
  EXPECT_EQ(10, s.when_ms);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_history_state(h, 1, &s));
  EXPECT_EQ(1, s.deleted);
  EXPECT_EQ(12, s.when_ms);
  gdefstor_history_free(h);
  GDEFSTOR_Entity * e = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_get(store, kTenant, 2, "n1", 2, &e));
  GDEFSTOR_EntityInfo info;
  std::memset(&info, 0, sizeof info);
  info.size = sizeof info;
  gdefstor_entity_info(e, &info);
  EXPECT_EQ(1, info.published);
  EXPECT_EQ(1, info.deleted);
  gdefstor_entity_free(e);
}

/* ---- saves --------------------------------------------------------------- */

class ModelSave : public ModelTest {
 protected:
  /* Declare the purchase order, create po1 and save the fixture as revision 1. */
  void SetUpPo() {
    ASSERT_EQ(GDEFSTOR_OK, Declare(kPo, PurchaseOrder()));
    ASSERT_EQ(GDEFSTOR_OK, Create(kPo, "po1"));
    GDEFSTOR_Tree * t = Build(PoTree("PO-1", 3499, "cust1"));
    uint64_t rev = 0;
    ASSERT_EQ(GDEFSTOR_OK, Save("po1", 0, t, &rev));
    gdefstor_tree_free(t);
    ASSERT_EQ(1u, rev);
  }

  Node Current(const std::string & pid) {
    GDEFSTOR_Tree * t = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, pid.data(),
        pid.size(), kLang, kLangLen, 0, &t));
    Node n = t == nullptr ? Node() : Read(t);
    gdefstor_tree_free(t);
    return n;
  }
};

/* Strip ids, so a tree read back compares with the tree that was written. */
inline Node NoIds(Node n) {
  n.id = 0;
  for (auto & kv : n.kids) {
    for (Node & k : kv.second) {
      k = NoIds(k);
    }
  }
  return n;
}

TEST_F(ModelSave, FirstSaveIsRevisionOne) {
  SetUpPo();
  Node now = Current("po1");
  EXPECT_EQ(NoIds(PoTree("PO-1", 3499, "cust1")), NoIds(now));
  EXPECT_EQ(1u, now.id);
  EXPECT_EQ(1u, Head("po1"));
  GDEFSTOR_History * h = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_history(store, kTenant, 2, "po1", 3, nullptr, 0, &h));
  ASSERT_EQ(1u, gdefstor_history_revision_count(h));
  GDEFSTOR_RevisionInfo r;
  std::memset(&r, 0, sizeof r);
  r.size = sizeof r;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_history_revision(h, 0, &r));
  EXPECT_EQ(1u, r.revision);
  EXPECT_EQ("u1", std::string(r.who, r.who_len));
  EXPECT_EQ(2000, r.when_ms);
  EXPECT_NE(0u, r.commit_seq);
  gdefstor_history_free(h);
}

TEST_F(ModelSave, SameTreeWritesNothing) {
  SetUpPo();
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  Node now = Current("po1");
  GDEFSTOR_Tree * t = Build(now);
  uint64_t rev = 99;
  ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t, &rev));
  gdefstor_tree_free(t);
  EXPECT_EQ(0u, rev);
  EXPECT_EQ(1u, Head("po1"));
  EXPECT_EQ(before, Dump(GDEFSTOR_MODEL_TRUTH));
}

TEST_F(ModelSave, UnchangedItemIsNotRewritten) {
  SetUpPo();
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  Node now = Current("po1");
  now.kids["lines"][1].Set("qty", Integer(5));
  GDEFSTOR_Tree * t = Build(now);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t, &rev));
  gdefstor_tree_free(t);
  ASSERT_EQ(2u, rev);
  std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
  /* One line changed: one item version, one revision; nothing else. */
  EXPECT_EQ(1u, NewRows(before, after, 'I'));
  EXPECT_EQ(1u, NewRows(before, after, 'R'));
  EXPECT_EQ("", AppendOnly(before, after));
  GDEFSTOR_Diff * d = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_diff(store, kTenant, 2, "po1", 3, nullptr, 0, 1,
      2, &d));
  ASSERT_EQ(1u, gdefstor_diff_count(d));
  GDEFSTOR_DiffItem di;
  std::memset(&di, 0, sizeof di);
  di.size = sizeof di;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_diff_item(d, 0, &di));
  EXPECT_EQ(GDEFSTOR_DIFF_CHANGED, di.kind);
  EXPECT_EQ(GDEFSTOR_DIFF_VALUES, di.what);
  EXPECT_EQ(now.kids["lines"][1].id, di.item);
  EXPECT_EQ("lines", std::string(di.field, di.field_len));
  gdefstor_diff_free(d);
}

TEST_F(ModelSave, StaleBaseIsAConflictAndWritesNothing) {
  SetUpPo();
  Node now = Current("po1");
  now.Set("total", Decimal(1));
  GDEFSTOR_Tree * t = Build(now);
  ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t));
  gdefstor_tree_free(t);
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  std::string cur_before = Dump(GDEFSTOR_MODEL_CURRENT);
  now.Set("total", Decimal(2));
  t = Build(now);
  /* Built on revision 1; the head is 2. */
  EXPECT_EQ(GDEFSTOR_ERR_CONFLICT, Save("po1", 1, t));
  EXPECT_EQ(GDEFSTOR_ERR_CONFLICT, Save("po1", 0, t));
  EXPECT_EQ(GDEFSTOR_ERR_CONFLICT, Save("po1", 3, t));
  EXPECT_EQ(before, Dump(GDEFSTOR_MODEL_TRUTH));
  EXPECT_EQ(cur_before, Dump(GDEFSTOR_MODEL_CURRENT));
  uint64_t rev = 0;
  EXPECT_EQ(GDEFSTOR_OK, Save("po1", 2, t, &rev));
  EXPECT_EQ(3u, rev);
  gdefstor_tree_free(t);
}

TEST_F(ModelSave, NestedAppendWritesOneItemVersion) {
  SetUpPo();
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  Node now = Current("po1");
  /* A tracking event, three groups deep: shipments, packages, tracking. */
  now.kids["shipments"][0].kids["packages"][0].Add("tracking").Set("at",
      Int(GDEFSTOR_FIELD_DATETIME, INT64_C(1760000360000))).Set("status",
      Text("delivered"));
  GDEFSTOR_Tree * t = Build(now);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t, &rev));
  gdefstor_tree_free(t);
  ASSERT_EQ(2u, rev);
  std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(1u, NewRows(before, after, 'I'));
  EXPECT_EQ(1u, NewRows(before, after, 'R'));
  EXPECT_EQ("", AppendOnly(before, after));
  Node got = Current("po1");
  ASSERT_EQ(2u, got.kids["shipments"][0].kids["packages"][0].kids["tracking"].size());
  EXPECT_EQ(Val::From(Text("delivered")), got.kids["shipments"][0]
      .kids["packages"][0].kids["tracking"][1].vals["status"]);
}

TEST_F(ModelSave, NewItemsGetNewIdsAndRemovedOnesAreClosed) {
  SetUpPo();
  Node now = Current("po1");
  uint64_t gone = now.kids["lines"][0].id;
  std::set<uint64_t> seen;
  std::function<void(const Node &)> walk = [&](const Node & n) {
    seen.insert(n.id);
    for (const auto & kv : n.kids) {
      for (const Node & k : kv.second) {
        walk(k);
      }
    }
  };
  walk(now);
  now.kids["lines"].erase(now.kids["lines"].begin());
  now.kids["lines"].push_back(Line("C-3", 4, 100));
  GDEFSTOR_Tree * t = Build(now);
  ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t));
  gdefstor_tree_free(t);
  Node got = Current("po1");
  ASSERT_EQ(2u, got.kids["lines"].size());
  EXPECT_EQ(Val::From(Text("C-3")), got.kids["lines"][1].vals["sku"]);
  EXPECT_EQ(0u, seen.count(got.kids["lines"][1].id));
  EXPECT_EQ(0u, seen.count(got.kids["lines"][1].kids["taxes"][0].id));
  /* The removed line and everything under it are closed, not deleted. */
  GDEFSTOR_Tree * old = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "po1", 3, kLang, kLangLen, 1, &old));
  EXPECT_EQ(gone, Read(old).kids["lines"][0].id);
  gdefstor_tree_free(old);
  /* An id the entity does not have now is refused. */
  Node bad = got;
  bad.kids["lines"][0].id = gone;
  t = Build(bad);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Save("po1", 2, t));
  gdefstor_tree_free(t);
  bad = got;
  bad.kids["lines"][1].id = bad.kids["lines"][0].id;
  t = Build(bad);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Save("po1", 2, t));
  gdefstor_tree_free(t);
  /* An id of an item of another field is refused. */
  bad = got;
  bad.kids["notes"][0].id = bad.kids["lines"][0].id;
  t = Build(bad);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Save("po1", 2, t));
  gdefstor_tree_free(t);
}

TEST_F(ModelSave, ValuesAreValidated) {
  SetUpPo();
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  Node good = Current("po1");
  auto refused = [&](const Node & n, GDEFSTOR_Result want) {
    GDEFSTOR_Tree * t = Build(n);
    EXPECT_EQ(want, Save("po1", 1, t));
    gdefstor_tree_free(t);
  };
  Node n = good;
  n.Set("total", Integer(5)); /* an integer for a decimal */
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.Set("total", Decimal(GDEFSTOR_MODEL_MAX_DECIMAL + 1));
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.Set("placed", Int(GDEFSTOR_FIELD_DATE, 2932897)); /* after 9999-12-31 */
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.kids["shipments"][0].kids["packages"][0].Set("weight", Real(NAN));
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.kids["shipments"][0].kids["packages"][0].Set("weight", Real(INFINITY));
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  {
    GDEFSTOR_Value bad = Text("x");
    bad.bytes = reinterpret_cast<const uint8_t *>("a\0b");
    bad.len = 3;
    n.Set("number", bad); /* NUL */
  }
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  {
    GDEFSTOR_Value bad = Text("\xC0\xAF"); /* overlong */
    n.Set("number", bad);
  }
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.Set("customer", Ref("not an id"));
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.vals.erase("number"); /* required */
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.kids["lines"][0].vals.erase("sku"); /* required, one group down */
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.Set("nosuch", Text("x"));
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.Add("number", Item()); /* an item under a scalar */
  refused(n, GDEFSTOR_ERR_INVALID);
  n = good;
  n.kids["notes"].push_back(Item()); /* an item of a multiple with no value */
  refused(n, GDEFSTOR_ERR_INVALID);
  EXPECT_EQ(before, Dump(GDEFSTOR_MODEL_TRUTH));
  /* A single group holds at most one item. */
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.addr", {G("home", 0,
      {F("city", GDEFSTOR_FIELD_TEXT)})}));
  ASSERT_EQ(GDEFSTOR_OK, Create("site.addr", "a1"));
  Node a;
  a.Add("home").Set("city", Text("x"));
  a.Add("home").Set("city", Text("y"));
  GDEFSTOR_Tree * t = Build(a);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Save("a1", 0, t));
  gdefstor_tree_free(t);
}

TEST_F(ModelSave, PersonalValuesNameTheirSubject) {
  Field note = F("note", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_PERSONAL);
  note.subject = "donor";
  Field own = F("phone", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_PERSONAL);
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.gift", {F("donor",
      GDEFSTOR_FIELD_REFERENCE), note, own, F("amount",
          GDEFSTOR_FIELD_DECIMAL, 0, 2)}));
  ASSERT_EQ(GDEFSTOR_OK, Create("site.gift", "g1"));
  Node n;
  n.Set("donor", Ref("p7")).Set("amount", Decimal(500));
  n.Set("note", Personal(GDEFSTOR_FIELD_TEXT, "\x01\x02sealed", "p7"));
  n.Set("phone", Personal(GDEFSTOR_FIELD_TEXT, "\x03sealed", "p9"));
  GDEFSTOR_Tree * t = Build(n);
  ASSERT_EQ(GDEFSTOR_OK, Save("g1", 0, t));
  gdefstor_tree_free(t);
  Node got = Current("g1");
  EXPECT_EQ("p7", got.vals["note"].s);
  EXPECT_EQ("\x01\x02sealed", got.vals["note"].b);
  EXPECT_EQ("p9", got.vals["phone"].s);
  /* The stored ciphertext passed through unchanged is no change. */
  t = Build(got);
  uint64_t rev = 7;
  ASSERT_EQ(GDEFSTOR_OK, Save("g1", 1, t, &rev));
  gdefstor_tree_free(t);
  EXPECT_EQ(0u, rev);
  /* A subject that is not the named reference's value, none at all, or one on
   * a field that is not personal, is refused. */
  Node bad = got;
  bad.Set("note", Personal(GDEFSTOR_FIELD_TEXT, "x", "p8"));
  t = Build(bad);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Save("g1", 1, t));
  gdefstor_tree_free(t);
  bad = got;
  bad.Set("phone", Text("plain"));
  t = Build(bad);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Save("g1", 1, t));
  gdefstor_tree_free(t);
  bad = got;
  bad.Set("amount", Personal(GDEFSTOR_FIELD_DECIMAL, "x", "p7"));
  t = Build(bad);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Save("g1", 1, t));
  gdefstor_tree_free(t);
}

TEST_F(ModelSave, LimitsAreRefusedNotCrashed) {
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.big", {F("tags", GDEFSTOR_FIELD_INTEGER,
      GDEFSTOR_FIELD_MULTIPLE), F("body", GDEFSTOR_FIELD_TEXT)}));
  ASSERT_EQ(GDEFSTOR_OK, Create("site.big", "b1"));
  GDEFSTOR_Tree * t = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_create(nullptr, &t));
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  size_t i;
  for (i = 1; i < GDEFSTOR_MODEL_MAX_ITEMS && rc == GDEFSTOR_OK; i++) {
    size_t at = 0;
    rc = gdefstor_tree_add(t, 0, "tags", 4, 0, &at);
  }
  ASSERT_EQ(GDEFSTOR_OK, rc);
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, gdefstor_tree_add(t, 0, "tags", 4, 0, nullptr));
  gdefstor_tree_free(t);
  std::string big(GDEFSTOR_MODEL_MAX_VALUE_BYTES + 1, 'a');
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_create(nullptr, &t));
  GDEFSTOR_Value v = Text(big.c_str());
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, gdefstor_tree_set(t, 0, "body", 4, &v));
  v.len = GDEFSTOR_MODEL_MAX_VALUE_BYTES;
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_tree_set(t, 0, "body", 4, &v));
  EXPECT_EQ(GDEFSTOR_OK, Save("b1", 0, t));
  gdefstor_tree_free(t);
  std::string why(GDEFSTOR_MODEL_MAX_WHY_BYTES + 1, 'w');
  GDEFSTOR_RevisionMeta m = Meta("u1", 1, why.c_str());
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_create(nullptr, &t));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_entity_save(w, "b1", 2, 1, &m, kLang, kLangLen, t, nullptr);
  }));
  gdefstor_tree_free(t);
}

/* ---- history ------------------------------------------------------------- */

class ModelHistory : public ModelSave {
 protected:
  /* Revisions 1 to 3 of po1, and the tree of each. */
  std::vector<Node> trees;

  void SetUpThree() {
    SetUpPo();
    trees.push_back(Node());
    trees.push_back(Current("po1"));
    Node two = trees[1];
    two.Set("total", Decimal(5000));
    two.kids["lines"][0].kids["taxes"][0].Set("amount", Decimal(1));
    two.kids["notes"].erase(two.kids["notes"].begin());
    GDEFSTOR_Tree * t = Build(two);
    ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t));
    gdefstor_tree_free(t);
    trees.push_back(Current("po1"));
    Node three = trees[2];
    three.kids["lines"].push_back(Line("Z-9", 9, 9));
    three.kids["shipments"].clear();
    t = Build(three);
    ASSERT_EQ(GDEFSTOR_OK, Save("po1", 2, t));
    gdefstor_tree_free(t);
    trees.push_back(Current("po1"));
  }

  Node AsOf(uint64_t rev) {
    GDEFSTOR_Tree * t = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "po1", 3,
        kLang, kLangLen, rev, &t));
    Node n = t == nullptr ? Node() : Read(t);
    gdefstor_tree_free(t);
    return n;
  }
};

TEST_F(ModelHistory, ValidToIsWrittenOnce) {
  SetUpPo();
  std::string prev = Dump(GDEFSTOR_MODEL_TRUTH);
  uint64_t head = 1;
  /* Change the same item again and again, remove it, and revert: each item
   * version's valid_to may go from open to a number once, never again. */
  for (int round = 0; round < 6; round++) {
    Node now = Current("po1");
    if (round == 3) {
      now.kids["lines"].erase(now.kids["lines"].begin());
    } else {
      now.kids["lines"][0].Set("qty", Integer(100 + round));
    }
    GDEFSTOR_Tree * t = Build(now);
    uint64_t rev = 0;
    ASSERT_EQ(GDEFSTOR_OK, Save("po1", head, t, &rev));
    gdefstor_tree_free(t);
    head = rev;
    std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
    EXPECT_EQ("", AppendOnly(prev, after)) << "round " << round;
    prev = after;
  }
  for (uint64_t to : {uint64_t(1), uint64_t(3), uint64_t(1)}) {
    uint64_t rev = 0;
    ASSERT_EQ(GDEFSTOR_OK, Revert("po1", head, to, &rev));
    ASSERT_EQ(head + 1, rev);
    head = rev;
    std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
    EXPECT_EQ("", AppendOnly(prev, after)) << "revert to " << to;
    prev = after;
  }
}

TEST_F(ModelHistory, AsOfReadsEveryPastRevision) {
  SetUpThree();
  for (uint64_t r = 1; r <= 3; r++) {
    EXPECT_EQ(trees[r], AsOf(r)) << "revision " << r;
  }
  EXPECT_EQ(trees[3], Current("po1"));
  GDEFSTOR_Tree * t = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_tree(store, kTenant, 2, "po1",
      3, kLang, kLangLen, 4, &t));
}

TEST_F(ModelHistory, RevertAddsRowsAndLeavesHistory) {
  SetUpThree();
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, Revert("po1", 3, 1, &rev));
  ASSERT_EQ(4u, rev);
  std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ("", AppendOnly(before, after));
  EXPECT_GT(NewRows(before, after, 'I'), 0u);
  EXPECT_EQ(1u, NewRows(before, after, 'R'));
  /* Revision 4 is revision 1 again, ids and order included. */
  EXPECT_EQ(trees[1], AsOf(4));
  EXPECT_EQ(trees[1], Current("po1"));
  /* Revisions 2 and 3 read exactly as they did. */
  EXPECT_EQ(trees[2], AsOf(2));
  EXPECT_EQ(trees[3], AsOf(3));
  /* Every new version starts at revision 4; nothing earlier was reopened. */
  std::set<std::string> old_keys;
  for (const std::string & b : Lines(before, 'I')) {
    std::vector<std::string> v = Words(b);
    old_keys.insert(v[1] + " " + v[2] + " " + v[3] + " " + v[4]);
  }
  for (const std::string & l : Lines(after, 'I')) {
    std::vector<std::string> w = Words(l);
    if (old_keys.count(w[1] + " " + w[2] + " " + w[3] + " " + w[4]) == 0) {
      EXPECT_EQ("4", w[4]) << l;
      EXPECT_EQ("0", w[5]) << l;
    }
  }
  /* Reverting to the tree it already has writes nothing. */
  ASSERT_EQ(GDEFSTOR_OK, Revert("po1", 4, 1, &rev));
  EXPECT_EQ(0u, rev);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Revert("po1", 4, 9));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Revert("po1", 4, 0));
  EXPECT_EQ(GDEFSTOR_ERR_CONFLICT, Revert("po1", 3, 1));
}

TEST_F(ModelHistory, DiffListsAddedChangedRemoved) {
  SetUpThree();
  GDEFSTOR_Diff * d = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_diff(store, kTenant, 2, "po1", 3, nullptr, 0, 1,
      2, &d));
  std::map<uint64_t, GDEFSTOR_DiffItem> got;
  for (size_t i = 0; i < gdefstor_diff_count(d); i++) {
    GDEFSTOR_DiffItem x;
    std::memset(&x, 0, sizeof x);
    x.size = sizeof x;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_diff_item(d, i, &x));
    got[x.item] = x;
  }
  /* The root (total), the first line's tax, and the first note removed. */
  ASSERT_EQ(3u, got.size());
  EXPECT_EQ(GDEFSTOR_DIFF_CHANGED, got[1].kind);
  EXPECT_EQ(GDEFSTOR_DIFF_VALUES, got[1].what);
  uint64_t tax = trees[1].kids["lines"][0].kids["taxes"][0].id;
  EXPECT_EQ(GDEFSTOR_DIFF_CHANGED, got[tax].kind);
  EXPECT_EQ("lines.taxes", std::string(got[tax].field, got[tax].field_len));
  uint64_t note = trees[1].kids["notes"][0].id;
  EXPECT_EQ(GDEFSTOR_DIFF_REMOVED, got[note].kind);
  gdefstor_diff_free(d);
  /* From nothing, everything is added. */
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_diff(store, kTenant, 2, "po1", 3, nullptr, 0, 0,
      1, &d));
  size_t all = 0;
  std::function<void(const Node &)> count = [&](const Node & n) {
    all += 1;
    for (const auto & kv : n.kids) {
      for (const Node & k : kv.second) {
        count(k);
      }
    }
  };
  count(trees[1]);
  EXPECT_EQ(all, gdefstor_diff_count(d));
  gdefstor_diff_free(d);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_diff(store, kTenant, 2, "po1", 3, nullptr, 0, 2,
      2, &d));
  EXPECT_EQ(0u, gdefstor_diff_count(d));
  gdefstor_diff_free(d);
}

/* ---- order --------------------------------------------------------------- */

class ModelOrder : public ModelSave {
 protected:
  void SetUpList(int n) {
    ASSERT_EQ(GDEFSTOR_OK, Declare("site.list", {F("items", GDEFSTOR_FIELD_TEXT,
        GDEFSTOR_FIELD_MULTIPLE)}));
    ASSERT_EQ(GDEFSTOR_OK, Create("site.list", "l1"));
    Node root;
    for (int i = 0; i < n; i++) {
      std::string s = "v" + std::to_string(i);
      root.Add("items", One(Text(s.c_str())));
    }
    std::vector<std::string> keep;
    GDEFSTOR_Tree * t = Build(root);
    ASSERT_EQ(GDEFSTOR_OK, Save("l1", 0, t));
    gdefstor_tree_free(t);
  }

  std::map<uint64_t, double> IdKeys() {
    std::map<uint64_t, double> out;
    GDEFSTOR_Tree * t = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "l1", 2, kLang, kLangLen, 0,
        &t));
    for (size_t i = 1; t != nullptr && i < gdefstor_tree_item_count(t); i++) {
      GDEFSTOR_TreeItem c;
      std::memset(&c, 0, sizeof c);
      c.size = sizeof c;
      gdefstor_tree_item(t, i, &c);
      out[c.id] = c.order;
    }
    gdefstor_tree_free(t);
    return out;
  }

  std::vector<double> CurrentKeys() {
    GDEFSTOR_Tree * t = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "l1", 2, kLang, kLangLen, 0,
        &t));
    std::vector<double> k = t == nullptr ? std::vector<double>()
                                         : Keys(t, 1, "items");
    gdefstor_tree_free(t);
    return k;
  }
};

TEST_F(ModelOrder, MovingAnItemChangesOnlyItsKey) {
  SetUpList(5);
  std::vector<double> before_keys = CurrentKeys();
  Node now = Current("l1");
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  /* Move the last item to between the first and the second. */
  std::vector<Node> & items = now.kids["items"];
  Node last = items.back();
  items.pop_back();
  items.insert(items.begin() + 1, last);
  GDEFSTOR_Tree * t = Build(now);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, Save("l1", 1, t, &rev));
  gdefstor_tree_free(t);
  ASSERT_EQ(2u, rev);
  std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(1u, NewRows(before, after, 'I'));
  EXPECT_EQ(now, Current("l1"));
  std::vector<double> k = CurrentKeys();
  ASSERT_EQ(5u, k.size());
  /* The moved item took the midpoint of its new neighbours. */
  EXPECT_EQ((before_keys[0] + before_keys[1]) / 2.0, k[1]);
  EXPECT_EQ(before_keys[0], k[0]);
  EXPECT_EQ(before_keys[1], k[2]);
  /* Before the first: smaller than the first. */
  now = Current("l1");
  last = now.kids["items"].back();
  now.kids["items"].pop_back();
  now.kids["items"].insert(now.kids["items"].begin(), last);
  t = Build(now);
  ASSERT_EQ(GDEFSTOR_OK, Save("l1", 2, t, &rev));
  gdefstor_tree_free(t);
  k = CurrentKeys();
  EXPECT_LT(k[0], k[1]);
  EXPECT_EQ(now, Current("l1"));
}

TEST_F(ModelOrder, RenumberingHappensInOneRevision) {
  SetUpList(3);
  uint64_t head = 1;
  bool renumbered = false;
  /* Insert again and again just after the first item: the gap halves each
   * time until it is too small, and then the run is renumbered, whole, in
   * the revision that needed it. */
  for (int round = 0; round < 40 && !renumbered; round++) {
    Node now = Current("l1");
    std::string s = "n" + std::to_string(round);
    now.kids["items"].insert(now.kids["items"].begin() + 1, One(Text(s.c_str())));
    std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
    std::map<uint64_t, double> was = IdKeys();
    GDEFSTOR_Tree * t = Build(now);
    uint64_t rev = 0;
    ASSERT_EQ(GDEFSTOR_OK, Save("l1", head, t, &rev));
    gdefstor_tree_free(t);
    head = rev;
    std::string after = Dump(GDEFSTOR_MODEL_TRUTH);
    std::map<uint64_t, double> is = IdKeys();
    size_t moved = 0;
    for (const auto & kv : is) {
      if (was.count(kv.first) == 0 || was[kv.first] != kv.second) {
        moved += 1;
      }
    }
    std::vector<double> k = CurrentKeys();
    ASSERT_EQ(now.kids["items"].size(), k.size());
    for (size_t i = 1; i < k.size(); i++) {
      /* No revision ever holds siblings closer than the epsilon. */
      ASSERT_GE(k[i] - k[i - 1], GDEFSTOR_MODEL_ORDER_EPSILON)
          << "round " << round;
    }
    size_t rows = NewRows(before, after, 'I');
    /* Exactly the items whose key changed got a new version, all in this
     * revision. */
    EXPECT_EQ(moved, rows) << "round " << round;
    if (rows > 1) {
      /* The renumbering: the siblings at 1, 2, 3 ..., in this one revision. */
      for (size_t i = 0; i < k.size(); i++) {
        EXPECT_EQ(static_cast<double>(i + 1), k[i]);
      }
      renumbered = true;
    } else {
      EXPECT_EQ(1u, rows);
    }
    EXPECT_EQ(now.kids["items"].size(), Current("l1").kids["items"].size());
  }
  EXPECT_TRUE(renumbered);
}

/* ---- drafts -------------------------------------------------------------- */

class ModelDraft : public ModelSave {
 protected:
  std::string Json(const Node & n) {
    GDEFSTOR_Type * type = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_type_get(store, kTenant, 2, kPo,
        std::strlen(kPo), &type));
    GDEFSTOR_Tree * t = Build(n);
    uint8_t * json = nullptr;
    size_t len = 0;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_tree_to_json(type, t, &json, &len));
    std::string s(reinterpret_cast<const char *>(json), len);
    gdefstor_bytes_free(nullptr, json);
    gdefstor_tree_free(t);
    gdefstor_type_free(type);
    return s;
  }

  GDEFSTOR_Result Put(const char * pid, uint64_t base, const std::string & json,
      const char * type = nullptr) {
    return InWrite([&](GDEFSTOR_Write * w) {
      return DraftPut(w, type, type == nullptr ? 0 : std::strlen(type),
          pid, std::strlen(pid), base,
          reinterpret_cast<const uint8_t *>(json.data()), json.size());
    });
  }

  GDEFSTOR_Result Apply(const char * pid, uint64_t * out = nullptr) {
    GDEFSTOR_RevisionMeta m = Meta("editor", 7000, "apply draft");
    uint64_t got = 0;
    GDEFSTOR_Result rc = InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_draft_apply(w, pid, std::strlen(pid), &m, &got);
    });
    if (out != nullptr) {
      *out = got;
    }
    return rc;
  }

  bool HasDraft(const char * pid) {
    GDEFSTOR_Draft * d = nullptr;
    GDEFSTOR_Result rc = gdefstor_draft_get(store, kTenant, 2, pid,
        std::strlen(pid), &d);
    gdefstor_draft_free(d);
    return rc == GDEFSTOR_OK;
  }
};

TEST_F(ModelDraft, JsonRoundTripsTheTree) {
  SetUpPo();
  Node now = Current("po1");
  now.kids["shipments"][0].kids["packages"][0].Set("weight", Real(0.1 + 0.2));
  GDEFSTOR_Type * type = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(store, kTenant, 2, kPo,
      std::strlen(kPo), &type));
  std::string json = Json(now);
  SCOPED_TRACE(json);
  GDEFSTOR_Tree * back = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_from_json(type,
      reinterpret_cast<const uint8_t *>(json.data()), json.size(), nullptr,
      &back));
  /* A built tree's root carries no id (a save ignores it), so neither does
   * its JSON. */
  Node want = now;
  want.id = 0;
  EXPECT_EQ(want, Read(back));
  gdefstor_tree_free(back);
  /* Other formats and fields the type does not have are refused. */
  std::string other = "{\"format\":\"defiant-tree/9\",\"root\":{}}";
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, gdefstor_tree_from_json(type,
      reinterpret_cast<const uint8_t *>(other.data()), other.size(), nullptr,
      &back));
  std::string unknown = "{\"format\":\"defiant-tree/1\",\"root\":{\"nope\":1}}";
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_tree_from_json(type,
      reinterpret_cast<const uint8_t *>(unknown.data()), unknown.size(),
      nullptr, &back));
  gdefstor_type_free(type);
}

TEST_F(ModelDraft, AppliedOnAMovedHeadIsRefused) {
  SetUpPo();
  Node draft = Current("po1");
  draft.Set("total", Decimal(42));
  ASSERT_EQ(GDEFSTOR_OK, Put("po1", 1, Json(draft)));
  /* Someone saves first. */
  Node other = Current("po1");
  other.Set("total", Decimal(43));
  GDEFSTOR_Tree * t = Build(other);
  ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t));
  gdefstor_tree_free(t);
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(GDEFSTOR_ERR_CONFLICT, Apply("po1"));
  EXPECT_EQ(before, Dump(GDEFSTOR_MODEL_TRUTH));
  EXPECT_TRUE(HasDraft("po1"));
  EXPECT_EQ(Val::From(Decimal(43)), Current("po1").vals["total"]);
  /* Rebased on the head, it applies, and is gone. */
  ASSERT_EQ(GDEFSTOR_OK, Put("po1", 2, Json(draft)));
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, Apply("po1", &rev));
  EXPECT_EQ(3u, rev);
  EXPECT_EQ(Val::From(Decimal(42)), Current("po1").vals["total"]);
  EXPECT_FALSE(HasDraft("po1"));
}

TEST_F(ModelDraft, DeletingADraftLeavesRevisionsUntouched) {
  SetUpPo();
  std::string truth_before = Dump(GDEFSTOR_MODEL_TRUTH);
  std::string current_before = Dump(GDEFSTOR_MODEL_CURRENT);
  Node draft = Current("po1");
  draft.Set("total", Decimal(1));
  ASSERT_EQ(GDEFSTOR_OK, Put("po1", 1, Json(draft)));
  EXPECT_EQ(current_before, Dump(GDEFSTOR_MODEL_CURRENT));
  EXPECT_EQ(1u, Lines(Dump(GDEFSTOR_MODEL_TRUTH), 'D').size());
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_draft_delete(w, "po1", 3);
  }));
  EXPECT_EQ(truth_before, Dump(GDEFSTOR_MODEL_TRUTH));
  EXPECT_EQ(current_before, Dump(GDEFSTOR_MODEL_CURRENT));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_draft_delete(w, "po1", 3);
  }));
}

TEST_F(ModelDraft, ADraftReservesItsPublicId) {
  ASSERT_EQ(GDEFSTOR_OK, Declare(kPo, PurchaseOrder()));
  ASSERT_EQ(GDEFSTOR_OK, Put("new1", 0, Json(PoTree("PO-9", 1, "c")), kPo));
  /* No entity row until it is applied, and the id is taken. */
  GDEFSTOR_Entity * e = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_get(store, kTenant, 2, "new1",
      4, &e));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Create(kPo, "new1"));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Put("new2", 1, Json(PoTree("PO-9", 1, "c")),
      kPo));
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, Apply("new1", &rev));
  EXPECT_EQ(1u, rev);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_get(store, kTenant, 2, "new1", 4, &e));
  GDEFSTOR_EntityInfo info;
  std::memset(&info, 0, sizeof info);
  info.size = sizeof info;
  gdefstor_entity_info(e, &info);
  EXPECT_EQ("editor", std::string(info.creator, info.creator_len));
  EXPECT_EQ(7000, info.created_ms);
  EXPECT_EQ(1u, info.revision);
  gdefstor_entity_free(e);
  EXPECT_EQ(NoIds(PoTree("PO-9", 1, "c")), NoIds(Current("new1")));
  EXPECT_FALSE(HasDraft("new1"));
}

TEST_F(ModelDraft, ADraftIsValidatedWhenApplied) {
  ASSERT_EQ(GDEFSTOR_OK, Declare(kPo, PurchaseOrder()));
  Node bad = PoTree("PO-1", 1, "c");
  bad.vals.erase("number");
  ASSERT_EQ(GDEFSTOR_OK, Put("n1", 0, Json(bad), kPo));
  std::string before = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Apply("n1"));
  /* Nothing was created. */
  EXPECT_EQ(before, Dump(GDEFSTOR_MODEL_TRUTH));
  /* Not JSON, or not a tree, is refused at put. */
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Put("n2", 0, "{", kPo));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Put("n2", 0, "[]", kPo));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Put("n2", 0, "{\"root\":{}}", kPo));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Put("n2", 0, Json(bad), "site.none"));
  /* A draft that became invalid when the type changed is refused at apply. */
  ASSERT_EQ(GDEFSTOR_OK, Put("n3", 0, Json(PoTree("PO-3", 1, "c")), kPo));
  std::vector<Field> stricter = PurchaseOrder();
  stricter.push_back(F("approver", GDEFSTOR_FIELD_REFERENCE,
      GDEFSTOR_FIELD_REQUIRED));
  ASSERT_EQ(GDEFSTOR_OK, Declare(kPo, stricter));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Apply("n3"));
  EXPECT_TRUE(HasDraft("n3"));
}

/* ---- queries ------------------------------------------------------------- */

/* A query tree and everything it points to. */
struct Q {
  std::deque<GDEFSTOR_QueryNode> nodes;
  std::deque<std::vector<const GDEFSTOR_QueryNode *>> kids;
  std::deque<std::vector<GDEFSTOR_Value>> vals;
  std::deque<std::string> strs;
  std::deque<GDEFSTOR_QueryText> texts;

  /* The right side of a blind-equality node: one MAC. */
  GDEFSTOR_QueryOperand Mac(const std::string & mac) {
    strs.push_back(mac);
    GDEFSTOR_QueryText t;
    t.bytes = reinterpret_cast<const uint8_t *>(strs.back().data());
    t.len = strs.back().size();
    texts.push_back(t);
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_CONST;
    o.texts = &texts.back();
    o.text_count = 1;
    return o;
  }

  GDEFSTOR_QueryOperand Field(const std::string & path, bool outer = false) {
    strs.push_back(path);
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_FIELD;
    o.field = strs.back().data();
    o.field_len = strs.back().size();
    o.flags = outer ? GDEFSTOR_QUERY_OUTER : 0;
    return o;
  }
  GDEFSTOR_QueryOperand Values(std::vector<GDEFSTOR_Value> v) {
    vals.push_back(std::move(v));
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_VALUES;
    o.values = vals.back().empty() ? nullptr : vals.back().data();
    o.value_count = vals.back().size();
    return o;
  }
  GDEFSTOR_QueryOperand Meta(int m, bool outer = false) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_META;
    o.meta = m;
    o.flags = outer ? GDEFSTOR_QUERY_OUTER : 0;
    return o;
  }
  GDEFSTOR_QueryOperand Entity(bool outer = false) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_ENTITY;
    o.flags = outer ? GDEFSTOR_QUERY_OUTER : 0;
    return o;
  }
  const GDEFSTOR_QueryNode * Cmp(int op, GDEFSTOR_QueryOperand l,
      GDEFSTOR_QueryOperand r) {
    nodes.emplace_back();
    GDEFSTOR_QueryNode & n = nodes.back();
    std::memset(&n, 0, sizeof n);
    n.op = op;
    n.left = l;
    n.right = r;
    return &n;
  }
  const GDEFSTOR_QueryNode * Op(int op,
      std::vector<const GDEFSTOR_QueryNode *> children,
      const char * exists = nullptr) {
    kids.push_back(std::move(children));
    nodes.emplace_back();
    GDEFSTOR_QueryNode & n = nodes.back();
    std::memset(&n, 0, sizeof n);
    n.op = op;
    n.children = kids.back().empty() ? nullptr : kids.back().data();
    n.child_count = kids.back().size();
    if (exists != nullptr) {
      n.exists_type = exists;
      n.exists_type_len = std::strlen(exists);
    }
    return &n;
  }
};

struct Answer {
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  std::vector<std::string> ids;
  uint64_t total = 0;
  bool operator==(const Answer & o) const {
    return rc == o.rc && ids == o.ids && total == o.total;
  }
};

inline std::ostream & operator<<(std::ostream & os, const Answer & a) {
  os << "rc=" << a.rc << " total=" << a.total << " [";
  for (const std::string & s : a.ids) {
    os << s << " ";
  }
  return os << "]";
}

inline Answer Ask(GDEFSTOR_Store * store, const char * type,
    const GDEFSTOR_QueryNode * where,
    std::vector<GDEFSTOR_QuerySort> sorts = {}, uint64_t offset = 0,
    uint64_t limit = 0, const char * lang = kLang) {
  GDEFSTOR_Query q;
  std::memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.language = lang;
  q.language_len = std::strlen(lang);
  q.where = where;
  q.sorts = sorts.empty() ? nullptr : sorts.data();
  q.sort_count = sorts.size();
  q.offset = offset;
  q.limit = limit;
  Answer a;
  GDEFSTOR_QueryResult * r = nullptr;
  a.rc = gdefstor_entity_query(store, kTenant, 2, type, std::strlen(type), &q,
      &r);
  if (a.rc != GDEFSTOR_OK) {
    return a;
  }
  a.total = gdefstor_query_result_total(r);
  for (size_t i = 0; i < gdefstor_query_result_rows(r); i++) {
    const char * e = nullptr;
    size_t n = 0;
    gdefstor_query_result_entity(r, i, &e, &n);
    a.ids.push_back(std::string(e, n));
  }
  gdefstor_query_result_free(r);
  return a;
}

inline GDEFSTOR_QuerySort By(GDEFSTOR_QueryOperand key, bool desc = false) {
  GDEFSTOR_QuerySort s;
  std::memset(&s, 0, sizeof s);
  s.key = key;
  s.descending = desc ? 1 : 0;
  return s;
}

inline Answer Ids(std::vector<std::string> ids, uint64_t total) {
  Answer a;
  a.ids = std::move(ids);
  a.total = total;
  return a;
}

class ModelQuery : public ModelSave {
 protected:
  void SetUp() override {
    ModelSave::SetUp();
    ASSERT_EQ(GDEFSTOR_OK, Declare(kPo, PurchaseOrder()));
    ASSERT_EQ(GDEFSTOR_OK, Declare("site.customer", {F("name",
        GDEFSTOR_FIELD_TEXT), F("vip", GDEFSTOR_FIELD_BOOLEAN)}));
    Customer("c1", "Ann", 1);
    Customer("c2", "Bob", 0);
    Customer("c3", "Cy", 1);
    /* po1: two lines (A-1, B-2), sent. */
    Po("po1", PoTree("PO-1", 3499, "c1"));
    /* po2: one line C-3 x5, delivered. */
    Node two = PoTree("PO-2", 10000, "c2");
    two.kids["lines"].clear();
    two.kids["lines"].push_back(Line("C-3", 5, 2000));
    two.kids["shipments"][0].kids["packages"][0].kids["tracking"][0].Set(
        "status", Text("delivered"));
    Po("po2", two);
    /* po3: no lines, no shipments, c1, published. */
    Node three;
    three.Set("number", Text("PO-3")).Set("total", Decimal(1000)).Set(
        "customer", Ref("c1")).Set("placed", Int(GDEFSTOR_FIELD_DATE, 20000));
    Po("po3", three);
    ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_set_state(w, "po3", 3, 1, 0, "ed", 2, 9, nullptr);
    }));
    /* po4: created, never saved. */
    ASSERT_EQ(GDEFSTOR_OK, Create(kPo, "po4", "", 400));
  }

  void Customer(const char * id, const char * name, int vip) {
    ASSERT_EQ(GDEFSTOR_OK, Create("site.customer", id));
    Node n;
    n.Set("name", Text(name)).Set("vip", Int(GDEFSTOR_FIELD_BOOLEAN, vip));
    GDEFSTOR_Tree * t = Build(n);
    ASSERT_EQ(GDEFSTOR_OK, Save(id, 0, t));
    gdefstor_tree_free(t);
  }

  void Po(const char * id, const Node & n) {
    ASSERT_EQ(GDEFSTOR_OK, Create(kPo, id));
    GDEFSTOR_Tree * t = Build(n);
    ASSERT_EQ(GDEFSTOR_OK, Save(id, 0, t));
    gdefstor_tree_free(t);
  }
};

TEST_F(ModelQuery, EqualityInAndRanges) {
  Q q;
  EXPECT_EQ(Ids({"po2"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("number"), q.Values({Text("PO-2")}))));
  EXPECT_EQ(Ids({"po1", "po3"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_IN,
      q.Field("customer"), q.Values({Ref("c1"), Ref("c9")}))));
  EXPECT_EQ(Ids({"po1", "po2"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_GT,
      q.Field("total"), q.Values({Decimal(3000)}))));
  EXPECT_EQ(Ids({"po1", "po3"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_LE,
      q.Field("total"), q.Values({Decimal(3499)}))));
  /* Dates compare as dates: 20000 is before 20378. */
  EXPECT_EQ(Ids({"po3"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_LT,
      q.Field("placed"), q.Values({Int(GDEFSTOR_FIELD_DATE, 20378)}))));
  /* An absent value matches no comparison, and != holds for it. */
  EXPECT_EQ(Ids({"po4"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_NE,
      q.Field("number"), q.Values({Text("PO-1"), Text("PO-2"), Text("PO-3")}))));
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_IN,
      q.Field("number"), q.Values({}))));
  EXPECT_EQ(Ids({"po1", "po2", "po3", "po4"}, 4), Ask(store, kPo, nullptr));
  /* Floats compare as numbers, an integer field as an integer. */
  EXPECT_EQ(Ids({"po1", "po2"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_GE,
      q.Field("shipments.packages.weight"), q.Values({Real(1.5)}))));
  EXPECT_EQ(Ids({"po2"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_GT,
      q.Field("lines.qty"), q.Values({Integer(2)}))));
}

TEST_F(ModelQuery, MultiplesMatchSomeItem) {
  Q q;
  EXPECT_EQ(Ids({"po1"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("lines.sku"), q.Values({Text("B-2")}))));
  EXPECT_EQ(Ids({"po2"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("shipments.packages.tracking.status"),
      q.Values({Text("delivered")}))));
  EXPECT_EQ(Ids({"po1", "po2"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("lines.discounts.breakdown.reason"),
      q.Values({Text("season")}))));
  EXPECT_EQ(Ids({"po1", "po2"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("notes"), q.Values({Text("fragile")}))));
  /* "Some line is A-1" and "some line has qty 1" hold on different lines of
   * po1: each comparison is decided on its own. */
  EXPECT_EQ(Ids({"po1"}, 1), Ask(store, kPo, q.Op(GDEFSTOR_QUERY_AND,
      {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("lines.sku"), q.Values({Text("A-1")})),
          q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("lines.qty"),
              q.Values({Integer(1)}))})));
  /* != is "no item equals": po1 has an A-1 line. */
  EXPECT_EQ(Ids({"po2", "po3", "po4"}, 3), Ask(store, kPo, q.Cmp(
      GDEFSTOR_QUERY_NE, q.Field("lines.sku"), q.Values({Text("A-1")}))));
  EXPECT_EQ(Ids({"po2", "po3", "po4"}, 3), Ask(store, kPo, q.Op(
      GDEFSTOR_QUERY_NOT, {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("lines.sku"),
          q.Values({Text("A-1")}))})));
}

TEST_F(ModelQuery, SortPageAndCount) {
  Q q;
  const GDEFSTOR_QueryNode * all = q.Op(GDEFSTOR_QUERY_TRUE, {});
  EXPECT_EQ(Ids({"po2", "po1", "po3", "po4"}, 4), Ask(store, kPo, all,
      {By(q.Field("total"), true)}));
  /* No value sorts first, ascending. */
  EXPECT_EQ(Ids({"po4", "po3", "po1", "po2"}, 4), Ask(store, kPo, all,
      {By(q.Field("total"))}));
  /* A page is a slice of one order; the total is every match. */
  EXPECT_EQ(Ids({"po1", "po3"}, 4), Ask(store, kPo, all,
      {By(q.Field("total"), true)}, 1, 2));
  EXPECT_EQ(Ids({}, 4), Ask(store, kPo, all, {By(q.Field("total"))}, 9, 2));
  /* po1 to po3 were created at 500 and po4 at 400: the ties go by row. */
  EXPECT_EQ(Ids({"po1", "po2", "po3", "po4"}, 4), Ask(store, kPo, all,
      {By(q.Meta(GDEFSTOR_META_CREATED), true)}));
  EXPECT_EQ(Ids({"po1", "po2", "po3", "po4"}, 4), Ask(store, kPo, all,
      {By(q.Entity())}));
  uint64_t n = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(store, kTenant, 2, kPo,
      std::strlen(kPo), kLang, kLangLen, q.Cmp(GDEFSTOR_QUERY_GT, q.Field("total"),
          q.Values({Decimal(0)})), &n));
  EXPECT_EQ(3u, n);
  /* A limit above the page cap is clamped to it. */
  std::vector<std::string> many;
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    for (int i = 0; i < GDEFSTOR_QUERY_LIMIT_MAX + 5; i++) {
      std::string id = "c-" + std::to_string(i);
      GDEFSTOR_Result rc = gdefstor_entity_create(w, "site.customer", 13,
          id.data(), id.size(), "", 0, 1, nullptr);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    return GDEFSTOR_OK;
  }));
  Answer big = Ask(store, "site.customer", nullptr, {}, 0,
      GDEFSTOR_QUERY_LIMIT_MAX + 100);
  EXPECT_EQ(static_cast<size_t>(GDEFSTOR_QUERY_LIMIT_MAX), big.ids.size());
  EXPECT_EQ(static_cast<uint64_t>(GDEFSTOR_QUERY_LIMIT_MAX + 8), big.total);
}

TEST_F(ModelQuery, TiebreakIsTheRow) {
  Q q;
  const GDEFSTOR_QueryNode * all = q.Op(GDEFSTOR_QUERY_TRUE, {});
  /* po1 and po3 tie on customer c1; ties go by row, creation order, under
   * either direction. */
  EXPECT_EQ(Ids({"po4", "po1", "po3", "po2"}, 4), Ask(store, kPo, all,
      {By(q.Field("customer"))}));
  EXPECT_EQ(Ids({"po2", "po1", "po3", "po4"}, 4), Ask(store, kPo, all,
      {By(q.Field("customer"), true)}));
  EXPECT_EQ(Ids({"po1", "po2", "po3", "po4"}, 4), Ask(store, kPo, all));
  EXPECT_EQ(Ids({"po1", "po2", "po4", "po3"}, 4), Ask(store, kPo, all,
      {By(q.Meta(GDEFSTOR_META_PUBLISHED))}));
}

TEST_F(ModelQuery, TotalCountsMatchesNotScanned) {
  Q q;
  const GDEFSTOR_QueryNode * w = q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("number"),
      q.Values({Text("PO-2")}));
  EXPECT_EQ(Ids({"po2"}, 1), Ask(store, kPo, w, {}, 0, 1));
  EXPECT_EQ(Ids({}, 1), Ask(store, kPo, w, {}, 1, 1));
  uint64_t n = 99;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(store, kTenant, 2, kPo,
      std::strlen(kPo), kLang, kLangLen, w, &n));
  EXPECT_EQ(1u, n);
}

TEST_F(ModelQuery, AndUsesEveryClause) {
  Q q;
  EXPECT_EQ(Ids({"po1"}, 1), Ask(store, kPo, q.Op(GDEFSTOR_QUERY_AND,
      {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("customer"), q.Values({Ref("c1")})),
          q.Cmp(GDEFSTOR_QUERY_GT, q.Field("total"), q.Values({Decimal(2000)}))})));
  EXPECT_EQ(Ids({"po1", "po2", "po3"}, 3), Ask(store, kPo, q.Op(GDEFSTOR_QUERY_OR,
      {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("customer"), q.Values({Ref("c1")})),
          q.Cmp(GDEFSTOR_QUERY_GT, q.Field("total"), q.Values({Decimal(5000)}))})));
  EXPECT_EQ(Ids({"po1", "po2", "po3", "po4"}, 4), Ask(store, kPo,
      q.Op(GDEFSTOR_QUERY_AND, {})));
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, q.Op(GDEFSTOR_QUERY_OR, {})));
}

TEST_F(ModelQuery, MetaFactsAreQueried) {
  Q q;
  EXPECT_EQ(Ids({"po3"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Meta(GDEFSTOR_META_PUBLISHED), q.Values({Int(GDEFSTOR_FIELD_BOOLEAN, 1)}))));
  EXPECT_EQ(Ids({"po4"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Meta(GDEFSTOR_META_REVISION), q.Values({Integer(0)}))));
  /* po4 was created by the system: it has no creator. */
  EXPECT_EQ(Ids({"po1", "po2", "po3"}, 3), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Meta(GDEFSTOR_META_CREATOR), q.Values({Ref("u1")}))));
  EXPECT_EQ(Ids({"po4"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_LT,
      q.Meta(GDEFSTOR_META_CREATED), q.Values({Int(GDEFSTOR_FIELD_DATETIME, 450)}))));
  EXPECT_EQ(Ids({"po2"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Entity(), q.Values({Ref("po2")}))));
}

TEST_F(ModelQuery, ExistsIsARelationSemiJoin) {
  Q q;
  /* Orders whose customer is a VIP. */
  EXPECT_EQ(Ids({"po1", "po3"}, 2), Ask(store, kPo, q.Op(GDEFSTOR_QUERY_EXISTS,
      {q.Op(GDEFSTOR_QUERY_AND, {q.Cmp(GDEFSTOR_QUERY_EQ, q.Entity(),
          q.Field("customer", true)), q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("vip"),
              q.Values({Int(GDEFSTOR_FIELD_BOOLEAN, 1)}))})},
      "site.customer")));
  /* Customers with an order over 50.00, the other way round. */
  EXPECT_EQ(Ids({"c2"}, 1), Ask(store, "site.customer", q.Op(
      GDEFSTOR_QUERY_EXISTS, {q.Op(GDEFSTOR_QUERY_AND,
          {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("customer"), q.Entity(true)),
              q.Cmp(GDEFSTOR_QUERY_GT, q.Field("total"),
                  q.Values({Decimal(5000)}))})}, kPo)));
  /* Customers with no order at all. */
  EXPECT_EQ(Ids({"c3"}, 1), Ask(store, "site.customer", q.Op(GDEFSTOR_QUERY_NOT,
      {q.Op(GDEFSTOR_QUERY_EXISTS, {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("customer"),
          q.Entity(true))}, kPo)})));
  /* Two EXISTS in one tree, the second only reached when the first fails. */
  EXPECT_EQ(Ids({"c2", "c3"}, 2), Ask(store, "site.customer", q.Op(
      GDEFSTOR_QUERY_OR, {q.Op(GDEFSTOR_QUERY_EXISTS, {q.Op(GDEFSTOR_QUERY_AND,
          {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("customer"), q.Entity(true)),
              q.Cmp(GDEFSTOR_QUERY_GT, q.Field("total"),
                  q.Values({Decimal(5000)}))})}, kPo),
          q.Op(GDEFSTOR_QUERY_NOT, {q.Op(GDEFSTOR_QUERY_EXISTS,
              {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("customer"), q.Entity(true))},
              kPo)})})));
}

TEST_F(ModelQuery, RefusesWhatItCannotAnswer) {
  Q q;
  auto refused = [&](const GDEFSTOR_QueryNode * w, GDEFSTOR_Result want,
      std::vector<GDEFSTOR_QuerySort> sorts = {}) {
    EXPECT_EQ(want, Ask(store, kPo, w, sorts).rc);
  };
  GDEFSTOR_Result I = GDEFSTOR_ERR_INVALID;
  refused(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("nosuch"), q.Values({Text("x")})), I);
  refused(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("lines"), q.Values({Text("x")})), I);
  refused(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("number"), q.Values({Integer(1)})), I);
  refused(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("number"),
      q.Values({Text("a"), Integer(1)})), I);
  refused(q.Cmp(GDEFSTOR_QUERY_LT, q.Field("number"), q.Values({Text("x")})), I);
  refused(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("customer"), q.Values({Text("c1")})), I);
  refused(q.Cmp(GDEFSTOR_QUERY_IN, q.Field("number"), q.Field("number")), I);
  refused(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("number", true), q.Values({Text("x")})), I);
  refused(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("total"),
      q.Values({Decimal(1)})), GDEFSTOR_OK);
  {
    /* An operand of no kind the form has. */
    GDEFSTOR_QueryOperand odd;
    std::memset(&odd, 0, sizeof odd);
    odd.kind = 99;
    refused(q.Cmp(GDEFSTOR_QUERY_EQ, odd, q.Values({Text("x")})), I);
  }
  std::vector<GDEFSTOR_Value> lots(GDEFSTOR_QUERY_MAX_TEXTS + 1, Text("x"));
  refused(q.Cmp(GDEFSTOR_QUERY_IN, q.Field("number"), q.Values(lots)),
      GDEFSTOR_ERR_LIMIT);
  /* Nested EXISTS, an undeclared type. */
  refused(q.Op(GDEFSTOR_QUERY_EXISTS, {q.Op(GDEFSTOR_QUERY_EXISTS,
      {q.Op(GDEFSTOR_QUERY_TRUE, {})}, kPo)}, kPo), I);
  refused(q.Op(GDEFSTOR_QUERY_EXISTS, {q.Op(GDEFSTOR_QUERY_TRUE, {})},
      "site.none"), I);
  EXPECT_EQ(I, Ask(store, "site.none", nullptr).rc);
  /* Sorts: one value per entity, so a single root field, a fact or the id. */
  refused(nullptr, I, {By(q.Field("notes"))});
  refused(nullptr, I, {By(q.Field("lines.sku"))});
  refused(nullptr, I, {By(q.Values({Text("x")}))});
  /* A deep tree is a limit, not a crash. */
  const GDEFSTOR_QueryNode * deep = q.Op(GDEFSTOR_QUERY_TRUE, {});
  for (int i = 0; i < GDEFSTOR_QUERY_MAX_DEPTH + 1; i++) {
    deep = q.Op(GDEFSTOR_QUERY_NOT, {deep});
  }
  refused(deep, GDEFSTOR_ERR_LIMIT);
  /* Personal and retired fields are not queried. */
  Field note = F("note", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_PERSONAL);
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.p", {note, F("gone", GDEFSTOR_FIELD_TEXT)}));
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.p", {note}));
  EXPECT_EQ(I, Ask(store, "site.p", q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("note"),
      q.Values({Text("x")}))).rc);
  EXPECT_EQ(I, Ask(store, "site.p", q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("gone"),
      q.Values({Text("x")}))).rc);
  /* A query that binds no language. */
  GDEFSTOR_Query bad;
  std::memset(&bad, 0, sizeof bad);
  bad.size = sizeof bad;
  GDEFSTOR_QueryResult * r = nullptr;
  EXPECT_EQ(I, gdefstor_entity_query(store, kTenant, 2, kPo, std::strlen(kPo),
      &bad, &r));
}

/* ---- the forms a caller of text values reads ----------------------------- */

inline GDEFSTOR_QueryOperand Flagged(GDEFSTOR_QueryOperand o, int flags) {
  o.flags |= flags;
  return o;
}

TEST_F(ModelQuery, EmptyIfNoneReadsNoValueAsTheEmptyText) {
  Q q;
  const int E = GDEFSTOR_QUERY_EMPTY_IF_NONE;
  /* po4 was never saved: it has no number. Without the flag no comparison of
   * it holds; with it the number is the single empty text. */
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("number"), q.Values({Text("")}))));
  EXPECT_EQ(Ids({"po4"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(q.Field("number"), E), q.Values({Text("")}))));
  EXPECT_EQ(Ids({"po1", "po2", "po3"}, 3), Ask(store, kPo, q.Cmp(
      GDEFSTOR_QUERY_NE, Flagged(q.Field("number"), E), q.Values({Text("")}))));
  EXPECT_EQ(Ids({"po4"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_IN,
      Flagged(q.Field("number"), E), q.Values({Text("x"), Text("")}))));
  /* A multiple with no item, and a reference with no value. */
  EXPECT_EQ(Ids({"po3", "po4"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(q.Field("notes"), E), q.Values({Text("")}))));
  EXPECT_EQ(Ids({"po4"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(q.Field("customer"), E), q.Values({Ref("")}))));
  /* Two flagged fields with no value are equal: both are the empty text. */
  EXPECT_EQ(Ids({"po3", "po4"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(q.Field("notes"), E), Flagged(q.Field("lines.sku"), E))));
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("notes"), q.Field("lines.sku"))));
  /* Only on a text or reference field. */
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(q.Field("total"), E), q.Values({Decimal(1)}))).rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("number"), Flagged(q.Values({Text("")}), E))).rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(q.Entity(), E), q.Values({Ref("po1")}))).rc);
  /* As a sort key no value is the empty text, tied with it, and ties go by
   * row: t2 was made before t1. */
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.tag", {F("title", GDEFSTOR_FIELD_TEXT)}));
  for (const char * id : {"t2", "t1", "t3"}) {
    ASSERT_EQ(GDEFSTOR_OK, Create("site.tag", id));
  }
  Node empty;
  empty.Set("title", Text(""));
  Node a;
  a.Set("title", Text("a"));
  GDEFSTOR_Tree * te = Build(empty);
  GDEFSTOR_Tree * ta = Build(a);
  ASSERT_EQ(GDEFSTOR_OK, Save("t2", 0, te));
  ASSERT_EQ(GDEFSTOR_OK, Save("t3", 0, ta));
  gdefstor_tree_free(te);
  gdefstor_tree_free(ta);
  const GDEFSTOR_QueryNode * all = q.Op(GDEFSTOR_QUERY_TRUE, {});
  EXPECT_EQ(Ids({"t1", "t2", "t3"}, 3), Ask(store, "site.tag", all,
      {By(q.Field("title"))}));
  EXPECT_EQ(Ids({"t2", "t1", "t3"}, 3), Ask(store, "site.tag", all,
      {By(Flagged(q.Field("title"), E))}));
  EXPECT_EQ(Ids({"t3", "t2", "t1"}, 3), Ask(store, "site.tag", all,
      {By(Flagged(q.Field("title"), E), true)}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, all,
      {By(Flagged(q.Field("total"), E))}).rc);
}

TEST_F(ModelQuery, AsTextComparesAReferenceWithText) {
  Q q;
  const int A = GDEFSTOR_QUERY_AS_TEXT;
  EXPECT_EQ(Ids({"po1", "po3"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(q.Field("customer"), A), q.Values({Text("c1")}))));
  EXPECT_EQ(Ids({"po2"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_IN,
      Flagged(q.Entity(), A), q.Values({Text("po2"), Text("zz")}))));
  EXPECT_EQ(Ids({"po1", "po2", "po3"}, 3), Ask(store, kPo, q.Cmp(
      GDEFSTOR_QUERY_EQ, Flagged(q.Meta(GDEFSTOR_META_CREATOR), A),
      q.Values({Text("u1")}))));
  /* Against a text field: no order's number is its customer's id. */
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("number"), Flagged(q.Field("customer"), A))));
  /* Inside an EXISTS, against the outer entity's text field. */
  EXPECT_EQ(Ids({"c1"}, 1), Ask(store, "site.customer", q.Op(
      GDEFSTOR_QUERY_EXISTS, {q.Op(GDEFSTOR_QUERY_AND,
          {q.Cmp(GDEFSTOR_QUERY_EQ, Flagged(q.Field("customer"), A),
              Flagged(q.Entity(true), A)),
              q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("number"),
                  q.Values({Text("PO-3")}))})}, kPo)));
  /* Without the flag a reference is not text; with it, only a reference. */
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("customer"), q.Values({Text("c1")}))).rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(q.Field("number"), A), q.Values({Text("PO-1")}))).rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(q.Meta(GDEFSTOR_META_REVISION), A), q.Values({Text("1")}))).rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("number"), Flagged(q.Values({Ref("c1")}), A))).rc);
}

TEST_F(ModelQuery, FirstSortsAMultipleByItsFirstItem) {
  Q q;
  const int FIRST = GDEFSTOR_QUERY_FIRST;
  ASSERT_EQ(GDEFSTOR_OK, Declare("site.tagged",
      {F("tags", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_MULTIPLE)}));
  auto make = [&](const char * id, std::vector<const char *> tags) {
    ASSERT_EQ(GDEFSTOR_OK, Create("site.tagged", id));
    if (tags.empty()) {
      return;
    }
    Node n;
    for (const char * t : tags) {
      n.Add("tags", One(Text(t)));
    }
    GDEFSTOR_Tree * t = Build(n);
    ASSERT_EQ(GDEFSTOR_OK, Save(id, 0, t));
    gdefstor_tree_free(t);
  };
  /* e1's first tag is b, though it also has a. */
  make("e1", {"b", "a"});
  make("e2", {"a", "z"});
  make("e3", {});
  make("e4", {"c"});
  const GDEFSTOR_QueryNode * all = q.Op(GDEFSTOR_QUERY_TRUE, {});
  EXPECT_EQ(Ids({"e3", "e2", "e1", "e4"}, 4), Ask(store, "site.tagged", all,
      {By(Flagged(q.Field("tags"), FIRST))}));
  EXPECT_EQ(Ids({"e4", "e1", "e2", "e3"}, 4), Ask(store, "site.tagged", all,
      {By(Flagged(q.Field("tags"), FIRST), true)}));
  EXPECT_EQ(Ids({"e3", "e2", "e1", "e4"}, 4), Ask(store, "site.tagged", all,
      {By(Flagged(q.Field("tags"), FIRST | GDEFSTOR_QUERY_EMPTY_IF_NONE))}));
  /* Moving a above b makes it e1's first, tied with e2's and after it by
   * row. */
  GDEFSTOR_Tree * cur = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "e1", 2,
      kLang, kLangLen, 0, &cur));
  GDEFSTOR_TreeItem b;
  GDEFSTOR_TreeItem a;
  std::memset(&b, 0, sizeof b);
  std::memset(&a, 0, sizeof a);
  b.size = sizeof b;
  a.size = sizeof a;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_item(cur, 1, &b));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_item(cur, 2, &a));
  gdefstor_tree_free(cur);
  Node moved;
  moved.Add("tags", One(Text("a"), a.id));
  moved.Add("tags", One(Text("b"), b.id));
  GDEFSTOR_Tree * t = Build(moved);
  ASSERT_EQ(GDEFSTOR_OK, Save("e1", 1, t));
  gdefstor_tree_free(t);
  EXPECT_EQ(Ids({"e3", "e1", "e2", "e4"}, 4), Ask(store, "site.tagged", all,
      {By(Flagged(q.Field("tags"), FIRST))}));
  /* FIRST is for a root multiple; a plain sort of one is still refused. */
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, "site.tagged", all,
      {By(q.Field("tags"))}).rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, all,
      {By(Flagged(q.Field("number"), FIRST))}).rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, all,
      {By(Flagged(q.Field("lines.sku"), FIRST))}).rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, all,
      {By(Flagged(q.Entity(), FIRST))}).rc);
}

TEST_F(ModelQuery, RevisionWhoIsTheNewestRevisionsAuthor) {
  Q q;
  const GDEFSTOR_QueryOperand who = q.Meta(GDEFSTOR_META_REVISION_WHO);
  /* Every saved order's newest revision is u1's; po4 has none. */
  EXPECT_EQ(Ids({"po1", "po2", "po3"}, 3), Ask(store, kPo, q.Cmp(
      GDEFSTOR_QUERY_EQ, who, q.Values({Ref("u1")}))));
  Node two = PoTree("PO-2b", 10000, "c2");
  GDEFSTOR_Tree * t = Build(two);
  ASSERT_EQ(GDEFSTOR_OK, Save("po2", 1, t, nullptr, "u2"));
  gdefstor_tree_free(t);
  /* The system's save has no author. */
  Node three;
  three.Set("number", Text("PO-3b"));
  t = Build(three);
  ASSERT_EQ(GDEFSTOR_OK, Save("po3", 1, t, nullptr, ""));
  gdefstor_tree_free(t);
  EXPECT_EQ(Ids({"po2"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ, who,
      q.Values({Ref("u2")}))));
  EXPECT_EQ(Ids({"po1"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ, who,
      q.Values({Ref("u1")}))));
  EXPECT_EQ(Ids({"po2", "po3", "po4"}, 3), Ask(store, kPo, q.Cmp(
      GDEFSTOR_QUERY_NE, who, q.Values({Ref("u1")}))));
  EXPECT_EQ(Ids({"po1", "po2"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      who, who)));
  EXPECT_EQ(Ids({"po2"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      Flagged(who, GDEFSTOR_QUERY_AS_TEXT), q.Values({Text("u2")}))));
  /* The creator stays the creator. */
  EXPECT_EQ(Ids({"po1", "po2", "po3"}, 3), Ask(store, kPo, q.Cmp(
      GDEFSTOR_QUERY_EQ, q.Meta(GDEFSTOR_META_CREATOR), q.Values({Ref("u1")}))));
  /* Inside an EXISTS: customers whose order u2 wrote last. */
  EXPECT_EQ(Ids({"c2"}, 1), Ask(store, "site.customer", q.Op(
      GDEFSTOR_QUERY_EXISTS, {q.Op(GDEFSTOR_QUERY_AND,
          {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("customer"), q.Entity(true)),
              q.Cmp(GDEFSTOR_QUERY_EQ, q.Meta(GDEFSTOR_META_REVISION_WHO),
                  q.Values({Ref("u2")}))})}, kPo)));
  /* As a sort key: no author first, then by bytes. */
  EXPECT_EQ(Ids({"po3", "po4", "po1", "po2"}, 4), Ask(store, kPo,
      q.Op(GDEFSTOR_QUERY_TRUE, {}), {By(who)}));
}

TEST_F(ModelQuery, AnExistsReadsTheLanguageItNames) {
  Q q;
  /* c1 has a French name too. */
  Node fr;
  fr.Set("name", Text("Anne")).Set("vip", Int(GDEFSTOR_FIELD_BOOLEAN, 1));
  GDEFSTOR_Tree * t = Build(fr);
  GDEFSTOR_RevisionMeta m = Meta("u1", 2500);
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_entity_save(w, "c1", 2, 1, &m, "fr", 2, t, nullptr);
  }));
  gdefstor_tree_free(t);
  auto named = [&](const char * name, const char * lang) {
    GDEFSTOR_QueryNode * x = const_cast<GDEFSTOR_QueryNode *>(q.Op(
        GDEFSTOR_QUERY_EXISTS, {q.Op(GDEFSTOR_QUERY_AND,
            {q.Cmp(GDEFSTOR_QUERY_EQ, q.Entity(), q.Field("customer", true)),
                q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("name"),
                    q.Values({Text(name)}))})}, "site.customer"));
    if (lang != nullptr) {
      x->exists_language = lang;
      x->exists_language_len = std::strlen(lang);
    }
    return static_cast<const GDEFSTOR_QueryNode *>(x);
  };
  /* The orders are read in English, their customer's name in French. */
  EXPECT_EQ(Ids({"po1", "po3"}, 2), Ask(store, kPo, named("Anne", "fr")));
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, named("Anne", nullptr)));
  EXPECT_EQ(Ids({"po1", "po3"}, 2), Ask(store, kPo, named("Ann", nullptr)));
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, named("Ann", "fr")));
  /* A language nobody has a tree in matches nothing, and a tag that is not
   * one is refused. */
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, named("Anne", "de")));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Ask(store, kPo, named("Anne", "e n")).rc);
  /* The outer entity's operands read the query's language: in French the
   * orders have no tree, so no customer. */
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, named("Anne", "fr"), {}, 0, 0, "fr"));
}

/* ---- derived data -------------------------------------------------------- */

class ModelDerived : public ModelQuery {};

TEST_F(ModelDerived, RebuildGivesTheSameCurrentState) {
  /* Some history first: changes, a removal, a move, a revert, states. */
  Node one = Current("po1");
  one.kids["lines"][0].Set("qty", Integer(9));
  std::swap(one.kids["lines"][0], one.kids["lines"][1]);
  one.kids["notes"].pop_back();
  GDEFSTOR_Tree * t = Build(one);
  ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t));
  gdefstor_tree_free(t);
  ASSERT_EQ(GDEFSTOR_OK, Revert("po1", 2, 1));
  Node two = Current("po2");
  two.kids["shipments"][0].kids["packages"][0].Add("tracking").Set("status",
      Text("lost"));
  t = Build(two);
  ASSERT_EQ(GDEFSTOR_OK, Save("po2", 1, t));
  gdefstor_tree_free(t);
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_entity_set_state(w, "po2", 3, 1, 1, "ed", 2, 9, nullptr);
  }));
  Q q;
  std::vector<std::function<Answer()>> asks = {
      [&] { return Ask(store, kPo, nullptr, {By(q.Field("total"), true)}); },
      [&] { return Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
                q.Field("shipments.packages.tracking.status"),
                q.Values({Text("lost")}))); },
      [&] { return Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
                q.Meta(GDEFSTOR_META_DELETED),
                q.Values({Int(GDEFSTOR_FIELD_BOOLEAN, 1)}))); },
      [&] { return Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_GT, q.Field("lines.qty"),
                q.Values({Integer(3)}))); },
  };
  std::vector<Answer> before;
  for (auto & a : asks) {
    before.push_back(a());
  }
  std::string current = Dump(GDEFSTOR_MODEL_CURRENT);
  std::string truth = Dump(GDEFSTOR_MODEL_TRUTH);
  Node po1 = Current("po1");
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_model_rebuild(w);
  }));
  EXPECT_EQ(current, Dump(GDEFSTOR_MODEL_CURRENT));
  EXPECT_EQ(truth, Dump(GDEFSTOR_MODEL_TRUTH));
  EXPECT_EQ(po1, Current("po1"));
  for (size_t i = 0; i < asks.size(); i++) {
    EXPECT_EQ(before[i], asks[i]()) << "query " << i;
  }
  EXPECT_EQ(Ids({"po2"}, 1), before[1]);
}

/* ---- the blind index and subjects (AD-14, AD-19) ------------------------- */

/* Stands in for an HMAC-SHA-256 under a tenant key: 32 bytes that depend on
 * the plaintext alone. Storage compares them for equality and nothing else. */
inline std::string MacOf(const std::string & plain) {
  std::string m(GDEFSTOR_BLIND_MAC_BYTES, '\0');
  uint64_t h = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < m.size(); i++) {
    for (char c : plain) {
      h ^= static_cast<unsigned char>(c);
      h *= UINT64_C(1099511628211);
    }
    h ^= i + 1;
    h *= UINT64_C(1099511628211);
    m[i] = static_cast<char>(h >> 56);
  }
  return m;
}

inline std::string HexOf(const std::string & b) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : b) {
    out += hex[c >> 4];
    out += hex[c & 0xF];
  }
  return out;
}

const char * const kMember = "site.member";
const char * const kGift = "site.gift";

/* A personal name of the entity's own subject, a plain tag, and a multiple
 * group of contacts each holding a multiple personal field: the design's
 * "multiples and groups" for blind equality. */
inline std::vector<Field> MemberType() {
  return {F("name", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_PERSONAL),
      F("tag", GDEFSTOR_FIELD_TEXT),
      G("contacts", GDEFSTOR_FIELD_MULTIPLE,
          {F("label", GDEFSTOR_FIELD_TEXT),
              F("emails", GDEFSTOR_FIELD_TEXT,
                  GDEFSTOR_FIELD_PERSONAL | GDEFSTOR_FIELD_MULTIPLE)})};
}

/* A personal note whose subject is the donor reference. */
inline std::vector<Field> GiftType() {
  Field note = F("note", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_PERSONAL);
  note.subject = "donor";
  return {F("donor", GDEFSTOR_FIELD_REFERENCE), note,
      F("amount", GDEFSTOR_FIELD_DECIMAL, 0, 2)};
}

class ModelBlind : public ModelSave {
 protected:
  int nonce = 0;

  void SetUp() override {
    ModelSave::SetUp();
    ASSERT_EQ(GDEFSTOR_OK, Declare(kMember, MemberType()));
    ASSERT_EQ(GDEFSTOR_OK, Declare(kGift, GiftType()));
  }

  /* @p plain, freshly encrypted (a new ciphertext every time, as AEAD with a
   * fresh nonce gives), with its MAC. */
  void Seal(Node & n, const std::string & field, const std::string & plain,
      const std::string & subject) {
    std::string c = "sealed:" + plain + ":" + std::to_string(++nonce);
    n.Set(field, Personal(GDEFSTOR_FIELD_TEXT, c.c_str(), subject.c_str()));
    n.Mac(field, MacOf(plain));
  }

  Node Email(const std::string & plain, const std::string & subject) {
    Node e;
    Seal(e, "", plain, subject);
    return e;
  }

  /* m<i>: name, tag, and contacts {label: [emails]}. */
  Node MemberTree(const std::string & name, const std::string & subject,
      const std::string & tag,
      const std::vector<std::pair<std::string, std::vector<std::string>>> &
          contacts = {}) {
    Node n;
    if (!name.empty()) {
      Seal(n, "name", name, subject);
    }
    n.Set("tag", Text(tag.c_str()));
    for (const auto & c : contacts) {
      Node & k = n.Add("contacts");
      k.Set("label", Text(c.first.c_str()));
      for (const std::string & e : c.second) {
        k.Add("emails", Email(e, subject));
      }
    }
    return n;
  }

  Node GiftTree(const std::string & donor, const std::string & note) {
    Node n;
    n.Set("donor", Ref(donor.c_str())).Set("amount", Decimal(100));
    Seal(n, "note", note, donor);
    return n;
  }

  void Put(const char * type, const std::string & pid, const Node & n,
      uint64_t base = 0) {
    if (base == 0) {
      ASSERT_EQ(GDEFSTOR_OK, Create(type, pid));
    }
    GDEFSTOR_Tree * t = Build(n);
    uint64_t rev = 0;
    ASSERT_EQ(GDEFSTOR_OK, Save(pid, base, t, &rev));
    gdefstor_tree_free(t);
    ASSERT_EQ(base + 1, rev);
  }

  Answer Blind(const char * type, const char * field, const std::string & plain) {
    Q q;
    return Ask(store, type, q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field(field),
        q.Mac(MacOf(plain))));
  }

  /* "pid from-to subject" per value version, in the port's order. */
  std::vector<std::string> History(const char * type, const char * field,
      const std::string & plain) {
    std::vector<std::string> out;
    GDEFSTOR_BlindHits * h = nullptr;
    std::string mac = MacOf(plain);
    GDEFSTOR_Result rc = gdefstor_blind_history(store, kTenant, 2, type,
        std::strlen(type), field, std::strlen(field),
        reinterpret_cast<const uint8_t *>(mac.data()), mac.size(), &h);
    EXPECT_EQ(GDEFSTOR_OK, rc);
    for (size_t i = 0; i < gdefstor_blind_hits_count(h); i++) {
      GDEFSTOR_BlindHit x;
      std::memset(&x, 0, sizeof x);
      x.size = sizeof x;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_blind_hit(h, i, &x));
      EXPECT_EQ(kMacKey, x.key_id);
      EXPECT_EQ(kMacVersion, x.version);
      out.push_back(std::string(x.public_id, x.public_id_len) + " " +
          std::to_string(x.valid_from) + "-" + std::to_string(x.valid_to) +
          " " + std::string(reinterpret_cast<const char *>(x.subject),
                    x.subject_len));
    }
    gdefstor_blind_hits_free(h);
    return out;
  }

  /* "type pid field from-to" per value version, in the port's order. */
  std::vector<std::string> Subject(const std::string & subject) {
    std::vector<std::string> out;
    GDEFSTOR_SubjectHits * h = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_subject_lookup(store, kTenant, 2,
        subject.data(), subject.size(), &h));
    for (size_t i = 0; i < gdefstor_subject_hits_count(h); i++) {
      GDEFSTOR_SubjectHit x;
      std::memset(&x, 0, sizeof x);
      x.size = sizeof x;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_subject_hit(h, i, &x));
      out.push_back(std::string(x.type, x.type_len) + " " +
          std::string(x.public_id, x.public_id_len) + " " +
          std::string(x.field, x.field_len) + " " +
          std::to_string(x.valid_from) + "-" + std::to_string(x.valid_to));
    }
    gdefstor_subject_hits_free(h);
    return out;
  }

  GDEFSTOR_Result DeleteSubject(const std::string & subject) {
    return InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_blind_delete_subject(w, subject.data(), subject.size());
    });
  }

  std::vector<std::string> BlindRows() {
    return Lines(Dump(GDEFSTOR_MODEL_TRUTH), 'B');
  }

  /* Every line of both dumps but the blind rows. */
  std::string NotBlind() {
    std::string out;
    for (int which : {GDEFSTOR_MODEL_TRUTH, GDEFSTOR_MODEL_CURRENT}) {
      std::istringstream in(Dump(which));
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty() && line[0] != 'B') {
          out += line + "\n";
        }
      }
    }
    return out;
  }
};

TEST_F(ModelBlind, ASaveWritesOneBlindRowPerPersonalValueVersion) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x", {{"home", {"a@x", "b@x"}}}));
  std::string truth = Dump(GDEFSTOR_MODEL_TRUTH);
  std::vector<std::string> blind = Lines(truth, 'B');
  /* The name and the two emails: three values, three rows. */
  ASSERT_EQ(3u, blind.size()) << truth;
  for (const std::string & b : blind) {
    /* B row item from field subject mac key version language */
    std::vector<std::string> w = Words(b);
    ASSERT_EQ(10u, w.size()) << b;
    EXPECT_EQ(HexOf(kLang), w[9]) << b;
    EXPECT_EQ("1", w[3]) << b;
    EXPECT_EQ(HexOf("s1"), w[5]) << b;
    EXPECT_EQ(std::to_string(kMacKey), w[7]) << b;
    EXPECT_EQ(std::to_string(kMacVersion), w[8]) << b;
    bool indexed = false;
    for (const std::string & l : Lines(truth, 'I')) {
      std::vector<std::string> v = Words(l);
      indexed |= v[1] == w[1] && v[3] == w[2] && v[4] == w[3];
    }
    EXPECT_TRUE(indexed) << "a blind row without the version it indexes: " << b;
  }
  std::set<std::string> macs;
  for (const std::string & b : blind) {
    macs.insert(Words(b)[6]);
  }
  EXPECT_EQ((std::set<std::string>{HexOf(MacOf("ann")), HexOf(MacOf("a@x")),
                HexOf(MacOf("b@x"))}),
      macs);
}

TEST_F(ModelBlind, NoMacIsInARevisionGroupOrCurrentRow) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x", {{"home", {"a@x"}}}));
  Put(kGift, "g1", GiftTree("s1", "thanks"));
  Node now = Current("m1");
  Seal(now, "name", "cy", "s1");
  GDEFSTOR_Tree * t = Build(now);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t));
  gdefstor_tree_free(t);
  std::string rest = NotBlind();
  for (const char * p : {"ann", "a@x", "thanks", "cy"}) {
    std::string mac = MacOf(p);
    EXPECT_EQ(std::string::npos, rest.find(HexOf(mac)))
        << "the MAC of " << p << " is outside the blind table:\n" << rest;
    /* Nor in what a reader is handed. */
    for (const char * pid : {"m1", "g1"}) {
      Node n = Current(pid);
      std::function<void(const Node &)> look = [&](const Node & x) {
        for (const auto & kv : x.vals) {
          EXPECT_EQ(std::string::npos, kv.second.b.find(mac)) << pid;
        }
        for (const auto & kv : x.kids) {
          for (const Node & k : kv.second) {
            look(k);
          }
        }
      };
      look(n);
    }
  }
  EXPECT_EQ(4u, BlindRows().size());
}

TEST_F(ModelBlind, AFailedSaveWritesNoBlindRow) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x"));
  std::vector<std::string> before = BlindRows();
  std::string rest = NotBlind();
  /* A stale base, a write that fails after its save, and a MAC on a field
   * that is not personal each write neither the value nor its blind row. */
  Node next = Current("m1");
  Seal(next, "name", "bob", "s1");
  GDEFSTOR_Tree * t = Build(next);
  EXPECT_EQ(GDEFSTOR_ERR_CONFLICT, Save("m1", 0, t));
  GDEFSTOR_RevisionMeta m = Meta();
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, InWrite([&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = gdefstor_entity_save(w, "m1", 2, 1, &m, kLang, kLangLen, t, nullptr);
    EXPECT_EQ(GDEFSTOR_OK, rc);
    /* Then something in the same write is refused, and it aborts. */
    return gdefstor_entity_create(w, kMember, std::strlen(kMember), "m1", 2,
        "", 0, 1, nullptr);
  }));
  gdefstor_tree_free(t);
  Node plain = Current("m1");
  plain.Set("tag", Text("y")).Mac("tag", MacOf("y"));
  t = Build(plain);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, Save("m1", 1, t));
  gdefstor_tree_free(t);
  EXPECT_EQ(before, BlindRows());
  EXPECT_EQ(rest, NotBlind());
  EXPECT_EQ(1u, Head("m1"));
}

TEST_F(ModelBlind, AnUnchangedValueWritesNoBlindRow) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x", {{"home", {"a@x", "b@x"}}}));
  std::string truth1 = Dump(GDEFSTOR_MODEL_TRUTH);
  /* The same tree, read back, MACs or none: no revision, no row. */
  Node same = Current("m1");
  GDEFSTOR_Tree * t = Build(same);
  uint64_t rev = 9;
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t, &rev));
  gdefstor_tree_free(t);
  EXPECT_EQ(0u, rev);
  same.Mac("name", MacOf("ann"));
  same.kids["contacts"][0].kids["emails"][0].Mac("", MacOf("a@x"));
  t = Build(same);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t, &rev));
  gdefstor_tree_free(t);
  EXPECT_EQ(0u, rev);
  EXPECT_EQ(truth1, Dump(GDEFSTOR_MODEL_TRUTH));
  /* The tag changes, so the root item gets a version; its name is the same
   * value and keeps its blind row in that version. The emails are items of
   * their own and do not change: no version, no row. */
  Node tag = Current("m1");
  tag.Set("tag", Text("y"));
  t = Build(tag);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t, &rev));
  gdefstor_tree_free(t);
  ASSERT_EQ(2u, rev);
  std::string truth2 = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(1u, NewRows(truth1, truth2, 'B')) << truth2;
  for (const std::string & b : Lines(truth2, 'B')) {
    std::vector<std::string> w = Words(b);
    if (w[3] == "2") {
      EXPECT_EQ(HexOf(MacOf("ann")), w[6]) << b;
      EXPECT_EQ(std::to_string(tag.id), w[2]) << b;
    }
  }
  /* A contact's label: its item changes, the emails under it do not. */
  Node label = Current("m1");
  label.kids["contacts"][0].Set("label", Text("work"));
  t = Build(label);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 2, t, &rev));
  gdefstor_tree_free(t);
  ASSERT_EQ(3u, rev);
  EXPECT_EQ(0u, NewRows(truth2, Dump(GDEFSTOR_MODEL_TRUTH), 'B'));
  EXPECT_EQ(Ids({"m1"}, 1), Blind(kMember, "contacts.emails", "b@x"));
}

TEST_F(ModelBlind, EqualityMatchesOnlyTheCurrentVersion) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x"));
  Put(kMember, "m2", MemberTree("bob", "s2", "x"));
  EXPECT_EQ(Ids({"m1"}, 1), Blind(kMember, "name", "ann"));
  EXPECT_EQ(Ids({"m2"}, 1), Blind(kMember, "name", "bob"));
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "name", "cy"));
  Node next = Current("m1");
  Seal(next, "name", "cy", "s1");
  GDEFSTOR_Tree * t = Build(next);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t));
  gdefstor_tree_free(t);
  /* The older version's MAC is history, not a match. */
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "name", "ann"));
  EXPECT_EQ(Ids({"m1"}, 1), Blind(kMember, "name", "cy"));
  uint64_t n = 9;
  Q q;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(store, kTenant, 2, kMember,
      std::strlen(kMember), kLang, kLangLen, q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("name"),
          q.Mac(MacOf("ann"))), &n));
  EXPECT_EQ(0u, n);
  /* The history lookup finds it, with the revisions that held it. */
  EXPECT_EQ(std::vector<std::string>({"m1 1-2 s1"}),
      History(kMember, "name", "ann"));
  EXPECT_EQ(std::vector<std::string>({"m1 2-0 s1"}),
      History(kMember, "name", "cy"));
  EXPECT_EQ(std::vector<std::string>({"m2 1-0 s2"}),
      History(kMember, "name", "bob"));
  EXPECT_TRUE(History(kMember, "name", "nobody").empty());
}

TEST_F(ModelBlind, MultiplesInANestedGroupMatchSomeItem) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x",
      {{"home", {"a@x", "b@x"}}, {"work", {"c@x"}}}));
  Put(kMember, "m2", MemberTree("bob", "s2", "x", {{"home", {"d@x"}}}));
  EXPECT_EQ(Ids({"m1"}, 1), Blind(kMember, "contacts.emails", "b@x"));
  EXPECT_EQ(Ids({"m1"}, 1), Blind(kMember, "contacts.emails", "c@x"));
  EXPECT_EQ(Ids({"m2"}, 1), Blind(kMember, "contacts.emails", "d@x"));
  /* A MAC of another field is not this field's. */
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "contacts.emails", "ann"));
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "name", "a@x"));
  Node next = Current("m1");
  auto & emails = next.kids["contacts"][0].kids["emails"];
  emails.erase(emails.begin() + 1);
  GDEFSTOR_Tree * t = Build(next);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t));
  gdefstor_tree_free(t);
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "contacts.emails", "b@x"));
  EXPECT_EQ(Ids({"m1"}, 1), Blind(kMember, "contacts.emails", "a@x"));
  EXPECT_EQ(std::vector<std::string>({"m1 1-2 s1"}),
      History(kMember, "contacts.emails", "b@x"));
  EXPECT_EQ(std::vector<std::string>({"m1 1-0 s1"}),
      History(kMember, "contacts.emails", "a@x"));
}

TEST_F(ModelBlind, EqualityComposesWithEveryOtherClause) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x"));
  Put(kMember, "m2", MemberTree("bob", "s2", "y"));
  Put(kMember, "m3", MemberTree("ann", "s3", "y"));
  Put(kGift, "g1", GiftTree("m1", "thanks"));
  Put(kGift, "g2", GiftTree("m2", "thanks"));
  Q q;
  auto ann = [&]() {
    return q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("name"), q.Mac(MacOf("ann")));
  };
  EXPECT_EQ(Ids({"m1", "m3"}, 2), Ask(store, kMember, ann()));
  EXPECT_EQ(Ids({"m3"}, 1), Ask(store, kMember, q.Op(GDEFSTOR_QUERY_AND,
      {ann(), q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("tag"), q.Values({Text("y")}))})));
  EXPECT_EQ(Ids({"m2"}, 1), Ask(store, kMember, q.Op(GDEFSTOR_QUERY_NOT,
      {ann()})));
  EXPECT_EQ(Ids({"m1", "m2", "m3"}, 3), Ask(store, kMember, q.Op(
      GDEFSTOR_QUERY_OR, {ann(), q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("name"),
          q.Mac(MacOf("bob")))})));
  /* Sorted and paged like any other filter. */
  EXPECT_EQ(Ids({"m3"}, 2), Ask(store, kMember, ann(),
      {By(q.Field("tag"), true)}, 0, 1));
  /* Inside a relation: gifts whose donor is a member named ann. */
  EXPECT_EQ(Ids({"g1"}, 1), Ask(store, kGift, q.Op(GDEFSTOR_QUERY_EXISTS,
      {q.Op(GDEFSTOR_QUERY_AND, {ann(), q.Cmp(GDEFSTOR_QUERY_EQ, q.Entity(),
          q.Field("donor", true))})}, kMember)));
  /* A personal field whose subject is a reference matches the same way. */
  EXPECT_EQ(Ids({"g1", "g2"}, 2), Ask(store, kGift, q.Cmp(GDEFSTOR_QUERY_BLIND,
      q.Field("note"), q.Mac(MacOf("thanks")))));
}

TEST_F(ModelBlind, PersonalFieldsAreMatchedOnlyBlind) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x", {{"home", {"a@x"}}}));
  const GDEFSTOR_Result I = GDEFSTOR_ERR_INVALID;
  Q q;
  std::string mac = MacOf("ann");
  /* Ciphertext is never compared, ranged or sorted. */
  EXPECT_EQ(I, Ask(store, kMember, q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("name"),
      q.Values({Text("ann")}))).rc);
  EXPECT_EQ(I, Ask(store, kMember, q.Cmp(GDEFSTOR_QUERY_LT, q.Field("name"),
      q.Values({Text("ann")}))).rc);
  EXPECT_EQ(I, Ask(store, kMember, nullptr, {By(q.Field("name"))}).rc);
  /* Blind equality takes a personal scalar and exactly one whole MAC. */
  EXPECT_EQ(I, Ask(store, kMember, q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("tag"),
      q.Mac(mac))).rc);
  EXPECT_EQ(I, Ask(store, kMember, q.Cmp(GDEFSTOR_QUERY_BLIND,
      q.Field("contacts"), q.Mac(mac))).rc);
  EXPECT_EQ(I, Ask(store, kMember, q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("name"),
      q.Mac(mac.substr(1)))).rc);
  EXPECT_EQ(I, Ask(store, kMember, q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("name"),
      q.Values({Text("ann")}))).rc);
  EXPECT_EQ(I, Ask(store, kMember, q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("nope"),
      q.Mac(mac))).rc);
  EXPECT_EQ(I, Ask(store, kGift, q.Op(GDEFSTOR_QUERY_EXISTS,
      {q.Cmp(GDEFSTOR_QUERY_BLIND, q.Field("note", true), q.Mac(mac))},
      kMember)).rc);
  /* A MAC goes on a value that is set, and a whole one. */
  GDEFSTOR_Tree * t = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_create(nullptr, &t));
  GDEFSTOR_BlindMac m;
  std::memset(&m, 0, sizeof m);
  m.size = sizeof m;
  m.mac = reinterpret_cast<const uint8_t *>(mac.data());
  m.mac_len = mac.size();
  EXPECT_EQ(I, gdefstor_tree_set_blind(t, 0, "name", 4, &m));
  GDEFSTOR_Value v = Personal(GDEFSTOR_FIELD_TEXT, "c", "s1");
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_set(t, 0, "name", 4, &v));
  m.mac_len = mac.size() - 1;
  EXPECT_EQ(I, gdefstor_tree_set_blind(t, 0, "name", 4, &m));
  m.mac_len = mac.size();
  m.size = sizeof m - 1;
  EXPECT_EQ(I, gdefstor_tree_set_blind(t, 0, "name", 4, &m));
  m.size = sizeof m;
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_tree_set_blind(t, 0, "name", 4, &m));
  gdefstor_tree_free(t);
  /* The history lookup takes a personal field and a whole MAC. */
  GDEFSTOR_BlindHits * h = nullptr;
  const uint8_t * mb = reinterpret_cast<const uint8_t *>(mac.data());
  EXPECT_EQ(I, gdefstor_blind_history(store, kTenant, 2, kMember, 11, "tag", 3,
      mb, mac.size(), &h));
  EXPECT_EQ(I, gdefstor_blind_history(store, kTenant, 2, kMember, 11, "name", 4,
      mb, mac.size() - 1, &h));
  EXPECT_EQ(I, gdefstor_blind_history(store, kTenant, 2, "site.none", 9,
      "name", 4, mb, mac.size(), &h));
  EXPECT_EQ(nullptr, h);
  GDEFSTOR_SubjectHits * sh = nullptr;
  EXPECT_EQ(I, gdefstor_subject_lookup(store, kTenant, 2, "not one", 7, &sh));
  EXPECT_EQ(I, gdefstor_subject_lookup(store, kTenant, 2, "", 0, &sh));
  EXPECT_EQ(nullptr, sh);
  EXPECT_EQ(I, DeleteSubject("not one"));
}

TEST_F(ModelBlind, SubjectLookupFindsEveryValueAcrossTypesAndHistory) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x", {{"home", {"a@x"}}}));
  Node next = Current("m1");
  Seal(next, "name", "ann b", "s1");
  next.kids["contacts"][0].Add("emails", Email("z@x", "s9"));
  GDEFSTOR_Tree * t = Build(next);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t));
  gdefstor_tree_free(t);
  Put(kGift, "g1", GiftTree("s1", "thanks"));
  /* Values written with no MAC name their subject all the same. */
  Node bare;
  bare.Set("name", Personal(GDEFSTOR_FIELD_TEXT, "sealed", "s1"));
  bare.Set("tag", Text("x"));
  Put(kMember, "m2", bare);
  EXPECT_EQ(std::vector<std::string>({"site.gift g1 note 1-0",
                "site.member m1 name 1-2", "site.member m1 name 2-0",
                "site.member m1 contacts.emails 1-0", "site.member m2 name 1-0"}),
      Subject("s1"));
  EXPECT_EQ(std::vector<std::string>({"site.member m1 contacts.emails 2-0"}),
      Subject("s9"));
  EXPECT_TRUE(Subject("s404").empty());
}

TEST_F(ModelBlind, DeletionRemovesASubjectsBlindRowsInEveryType) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x", {{"home", {"a@x"}}}));
  Put(kMember, "m2", MemberTree("bob", "s2", "x", {{"home", {"b@x"}}}));
  Put(kGift, "g1", GiftTree("s1", "thanks"));
  Put(kGift, "g2", GiftTree("s2", "cheers"));
  ASSERT_EQ(Ids({"m1"}, 1), Blind(kMember, "name", "ann"));
  ASSERT_EQ(Ids({"g1"}, 1), Blind(kGift, "note", "thanks"));
  std::string rest = NotBlind();
  std::vector<std::string> subject = Subject("s1");
  ASSERT_EQ(3u, subject.size());
  ASSERT_EQ(GDEFSTOR_OK, DeleteSubject("s1"));
  /* Neither blind equality nor history finds the subject, in either type. */
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "name", "ann"));
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "contacts.emails", "a@x"));
  EXPECT_EQ(Ids({}, 0), Blind(kGift, "note", "thanks"));
  EXPECT_TRUE(History(kMember, "name", "ann").empty());
  EXPECT_TRUE(History(kMember, "contacts.emails", "a@x").empty());
  EXPECT_TRUE(History(kGift, "note", "thanks").empty());
  for (const std::string & b : BlindRows()) {
    EXPECT_NE(HexOf("s1"), Words(b)[5]) << b;
  }
  /* Another subject's rows stay. */
  EXPECT_EQ(Ids({"m2"}, 1), Blind(kMember, "name", "bob"));
  EXPECT_EQ(Ids({"g2"}, 1), Blind(kGift, "note", "cheers"));
  EXPECT_EQ(3u, BlindRows().size());
  /* The ciphertext rows and the subject lookup are as they were. */
  EXPECT_EQ(rest, NotBlind());
  EXPECT_EQ(subject, Subject("s1"));
  /* Twice is a no-op, and so is a subject with no row. */
  std::string truth = Dump(GDEFSTOR_MODEL_TRUTH);
  EXPECT_EQ(GDEFSTOR_OK, DeleteSubject("s1"));
  EXPECT_EQ(GDEFSTOR_OK, DeleteSubject("s404"));
  EXPECT_EQ(truth, Dump(GDEFSTOR_MODEL_TRUTH));
  /* A value the subject keeps after erasure carries no row forward. */
  Node tag = Current("m1");
  tag.Set("tag", Text("y"));
  GDEFSTOR_Tree * t = Build(tag);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t));
  gdefstor_tree_free(t);
  EXPECT_EQ(3u, BlindRows().size());
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "name", "ann"));
}

TEST_F(ModelBlind, RebuildLeavesTheBlindTableAlone) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x", {{"home", {"a@x", "b@x"}}}));
  Node next = Current("m1");
  Seal(next, "name", "cy", "s1");
  GDEFSTOR_Tree * t = Build(next);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t));
  gdefstor_tree_free(t);
  Put(kGift, "g1", GiftTree("s2", "thanks"));
  std::vector<std::string> rows = BlindRows();
  std::vector<Answer> asks = {Blind(kMember, "name", "cy"),
      Blind(kMember, "name", "ann"), Blind(kMember, "contacts.emails", "b@x"),
      Blind(kGift, "note", "thanks")};
  std::vector<std::vector<std::string>> history = {
      History(kMember, "name", "ann"), History(kMember, "name", "cy")};
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    return gdefstor_model_rebuild(w);
  }));
  EXPECT_EQ(rows, BlindRows());
  EXPECT_EQ(asks[0], Blind(kMember, "name", "cy"));
  EXPECT_EQ(asks[1], Blind(kMember, "name", "ann"));
  EXPECT_EQ(asks[2], Blind(kMember, "contacts.emails", "b@x"));
  EXPECT_EQ(asks[3], Blind(kGift, "note", "thanks"));
  EXPECT_EQ(history[0], History(kMember, "name", "ann"));
  EXPECT_EQ(history[1], History(kMember, "name", "cy"));
  EXPECT_EQ(Ids({"m1"}, 1), asks[0]);
}

TEST_F(ModelBlind, ARevertBringsBackTheBlindRowsOfTheValuesItRestores) {
  Put(kMember, "m1", MemberTree("ann", "s1", "x", {{"home", {"a@x"}}}));
  Node next = Current("m1");
  Seal(next, "name", "cy", "s1");
  next.kids["contacts"].clear();
  GDEFSTOR_Tree * t = Build(next);
  ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t));
  gdefstor_tree_free(t);
  ASSERT_EQ(Ids({}, 0), Blind(kMember, "name", "ann"));
  ASSERT_EQ(GDEFSTOR_OK, Revert("m1", 2, 1));
  EXPECT_EQ(Ids({"m1"}, 1), Blind(kMember, "name", "ann"));
  EXPECT_EQ(Ids({"m1"}, 1), Blind(kMember, "contacts.emails", "a@x"));
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "name", "cy"));
  EXPECT_EQ(std::vector<std::string>({"m1 1-2 s1", "m1 3-0 s1"}),
      History(kMember, "name", "ann"));
}

/* Story S6c: the blind index rebuilt from decryptable values. The suite's
 * "ciphertext" is "sealed:<plain>:<n>", so the plaintext of a version, and so
 * its MAC, is read back from the version itself, as a caller holding the key
 * would read it. */
class ModelBlindPut : public ModelBlind {
 protected:
  struct Version {
    std::string type;
    std::string pid;
    std::string field;
    uint64_t item = 0;
    uint64_t from = 0;
    std::string lang;
  };

  std::vector<Version> Versions(const std::string & subject) {
    std::vector<Version> out;
    GDEFSTOR_SubjectHits * h = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_subject_lookup(store, kTenant, 2,
        subject.data(), subject.size(), &h));
    for (size_t i = 0; i < gdefstor_subject_hits_count(h); i++) {
      GDEFSTOR_SubjectHit x;
      std::memset(&x, 0, sizeof x);
      x.size = sizeof x;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_subject_hit(h, i, &x));
      Version v;
      v.type.assign(x.type, x.type_len);
      v.pid.assign(x.public_id, x.public_id_len);
      v.field.assign(x.field, x.field_len);
      v.item = x.item;
      v.from = x.valid_from;
      v.lang.assign(x.language, x.language_len);
      out.push_back(v);
    }
    gdefstor_subject_hits_free(h);
    return out;
  }

  /* The plaintext the version holds, from its ciphertext. */
  std::string PlainOf(const Version & v) {
    GDEFSTOR_Tree * tree = nullptr;
    std::string plain;
    std::string leaf = v.field.substr(v.field.rfind('.') == std::string::npos
                                          ? 0
                                          : v.field.rfind('.') + 1);
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, v.pid.data(),
        v.pid.size(), v.lang.data(), v.lang.size(), v.from, &tree));
    for (size_t i = 0; tree != nullptr && i < gdefstor_tree_item_count(tree);
         i++) {
      GDEFSTOR_TreeItem it;
      std::memset(&it, 0, sizeof it);
      it.size = sizeof it;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_tree_item(tree, i, &it));
      if (it.id != v.item) {
        continue;
      }
      GDEFSTOR_Value val;
      std::memset(&val, 0, sizeof val);
      bool own = std::string(it.field, it.field_len) == leaf;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_tree_get(tree, i, own ? "" : leaf.data(),
          own ? 0 : leaf.size(), &val));
      std::string c(reinterpret_cast<const char *>(val.bytes), val.len);
      if (c.compare(0, 7, "sealed:") == 0 && c.rfind(':') > 6) {
        plain = c.substr(7, c.rfind(':') - 7);
      }
    }
    gdefstor_tree_free(tree);
    return plain;
  }

  GDEFSTOR_Result PutMac(GDEFSTOR_Write * w, const Version & v,
      const std::string & mac) {
    GDEFSTOR_BlindMac m;
    std::memset(&m, 0, sizeof m);
    m.size = sizeof m;
    m.mac = reinterpret_cast<const uint8_t *>(mac.data());
    m.mac_len = mac.size();
    m.key_id = kMacKey;
    m.version = kMacVersion;
    return gdefstor_blind_put(w, v.type.data(), v.type.size(), v.pid.data(),
        v.pid.size(), v.field.data(), v.field.size(), v.item, v.from,
        v.lang.data(), v.lang.size(), &m);
  }

  /* Put back the row of every version of @p subjects. */
  GDEFSTOR_Result Rebuild(const std::vector<std::string> & subjects) {
    std::vector<Version> all;
    for (const std::string & s : subjects) {
      for (const Version & v : Versions(s)) {
        all.push_back(v);
      }
    }
    return InWrite([&](GDEFSTOR_Write * w) {
      for (const Version & v : all) {
        std::string plain = PlainOf(v);
        if (plain.empty()) {
          return GDEFSTOR_ERR_INTERNAL;
        }
        GDEFSTOR_Result rc = PutMac(w, v, MacOf(plain));
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
      }
      return GDEFSTOR_OK;
    });
  }

  /* Members with a changed name and a nested email, and a gift. */
  void History3() {
    Put(kMember, "m1", MemberTree("ann", "s1", "x", {{"home", {"a@x"}}}));
    Node next = Current("m1");
    Seal(next, "name", "cy", "s1");
    GDEFSTOR_Tree * t = Build(next);
    ASSERT_EQ(GDEFSTOR_OK, Save("m1", 1, t));
    gdefstor_tree_free(t);
    Put(kMember, "m2", MemberTree("bob", "s2", "y", {{"work", {"b@x"}}}));
    Put(kGift, "g1", GiftTree("s1", "thanks"));
  }
};

/* Every row deleted, then put back from the versions the subject lookup names
 * and the plaintexts they hold: the blind table, blind equality and the
 * history lookup are as they were, and putting the rows again changes
 * nothing. */
TEST_F(ModelBlindPut, PuttingTheRowsBackRebuildsTheBlindIndex) {
  History3();
  if (HasFatalFailure()) {
    return;
  }
  std::vector<std::string> rows = BlindRows();
  /* ann and a@x, cy (a@x carried with no row of its own), bob and b@x,
   * thanks. */
  ASSERT_EQ(6u, rows.size());
  std::vector<Answer> asks = {Blind(kMember, "name", "cy"),
      Blind(kMember, "name", "ann"), Blind(kMember, "contacts.emails", "a@x"),
      Blind(kMember, "name", "bob"), Blind(kGift, "note", "thanks")};
  std::vector<std::vector<std::string>> history = {
      History(kMember, "name", "ann"), History(kMember, "name", "cy"),
      History(kMember, "contacts.emails", "b@x")};
  ASSERT_EQ(Ids({"m1"}, 1), asks[0]);
  ASSERT_EQ(GDEFSTOR_OK, DeleteSubject("s1"));
  ASSERT_EQ(GDEFSTOR_OK, DeleteSubject("s2"));
  ASSERT_TRUE(BlindRows().empty());
  ASSERT_EQ(GDEFSTOR_OK, Rebuild({"s1", "s2"}));
  EXPECT_EQ(rows, BlindRows());
  EXPECT_EQ(asks[0], Blind(kMember, "name", "cy"));
  EXPECT_EQ(asks[1], Blind(kMember, "name", "ann"));
  EXPECT_EQ(asks[2], Blind(kMember, "contacts.emails", "a@x"));
  EXPECT_EQ(asks[3], Blind(kMember, "name", "bob"));
  EXPECT_EQ(asks[4], Blind(kGift, "note", "thanks"));
  EXPECT_EQ(history[0], History(kMember, "name", "ann"));
  EXPECT_EQ(history[1], History(kMember, "name", "cy"));
  EXPECT_EQ(history[2], History(kMember, "contacts.emails", "b@x"));
  /* Again: each version still has one row. */
  ASSERT_EQ(GDEFSTOR_OK, Rebuild({"s1", "s2"}));
  EXPECT_EQ(rows, BlindRows());
  /* A row put back is deleted with its subject like any other. */
  ASSERT_EQ(GDEFSTOR_OK, DeleteSubject("s1"));
  EXPECT_EQ(Ids({}, 0), Blind(kMember, "name", "cy"));
  EXPECT_EQ(Ids({"m2"}, 1), Blind(kMember, "name", "bob"));
}

/* A row is put back only for a version that holds a value of a personal
 * field: anything else is refused, and the write is as it was. */
TEST_F(ModelBlindPut, APutForNoSuchVersionIsRefused) {
  History3();
  if (HasFatalFailure()) {
    return;
  }
  std::vector<Version> v = Versions("s1");
  ASSERT_FALSE(v.empty());
  Version name;
  for (const Version & x : v) {
    if (x.pid == "m1" && x.field == "name" && x.from == 2) {
      name = x;
    }
  }
  ASSERT_EQ(2u, name.from);
  std::string truth = Dump(GDEFSTOR_MODEL_TRUTH);
  std::string mac = MacOf("cy");
  const GDEFSTOR_Result I = GDEFSTOR_ERR_INVALID;
  EXPECT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * w) {
    Version x = name;
    /* The item's current version began in revision 2: none began in 3. */
    x.from = 3;
    EXPECT_EQ(I, PutMac(w, x, mac));
    x = name;
    x.item += 1000;
    EXPECT_EQ(I, PutMac(w, x, mac));
    x = name;
    x.field = "tag";
    EXPECT_EQ(I, PutMac(w, x, mac));
    x = name;
    x.field = "contacts";
    EXPECT_EQ(I, PutMac(w, x, mac));
    x = name;
    x.pid = "g1";
    EXPECT_EQ(I, PutMac(w, x, mac));
    x = name;
    x.pid = "m404";
    EXPECT_EQ(I, PutMac(w, x, mac));
    x = name;
    x.lang = "fr";
    EXPECT_EQ(I, PutMac(w, x, mac));
    EXPECT_EQ(I, PutMac(w, name, mac.substr(1)));
    return GDEFSTOR_OK;
  }));
  EXPECT_EQ(truth, Dump(GDEFSTOR_MODEL_TRUTH));
}

/* The sweep: seeded histories of members, their names and nested emails
 * saved, changed, carried by other changes, removed, and their subjects
 * erased, checked against a model kept apart from the library: blind
 * equality (on the SQL backends the compiled query is also checked against
 * the reference scan), the history lookup and the subject lookup. */
class ModelBlindSweep : public ModelBlind {
 protected:
  struct MValue {
    std::string plain;
    std::string subject;
    std::string cipher;
    uint64_t id = 0;
  };
  struct MContact {
    std::string label;
    std::vector<MValue> emails;
    uint64_t id = 0;
  };
  struct MMember {
    std::string pid;
    bool has_name = false;
    MValue name;
    std::string tag;
    std::vector<MContact> contacts;
    uint64_t head = 0;
    /* Per revision: field path -> (plain, subject) of each value held. */
    std::vector<std::vector<std::pair<std::string,
        std::pair<std::string, std::string>>>> held;
  };

  std::mt19937 rng;
  std::vector<MMember> members;
  std::set<std::string> erased;
  int ops = 0;
  int matched = 0;
  int carried = 0;

  size_t Pick(size_t n) {
    return std::uniform_int_distribution<size_t>(0, n - 1)(rng);
  }
  std::string Plain() { return "p" + std::to_string(Pick(7)); }
  std::string Live() {
    static const char * const all[] = {"s1", "s2", "s3", "s4", "s5", "s6",
        "s7", "s8"};
    for (int i = 0; i < 32; i++) {
      std::string s = all[Pick(8)];
      if (erased.count(s) == 0) {
        return s;
      }
    }
    return "s0";
  }

  /* A changed value: a new plaintext's fresh ciphertext. */
  MValue Fresh() {
    MValue v;
    v.plain = Plain();
    v.subject = Live();
    v.cipher = "sealed:" + v.plain + ":" + std::to_string(++nonce);
    return v;
  }

  void Value(Node & n, const std::string & field, const MValue & v, bool mac) {
    n.Set(field, Personal(GDEFSTOR_FIELD_TEXT, v.cipher.c_str(),
        v.subject.c_str()));
    /* A caller gives the MAC of a value it wrote, and may give it again for
     * one it passes through; never for an erased subject's. */
    if (mac && erased.count(v.subject) == 0) {
      n.Mac(field, MacOf(v.plain));
    }
  }

  Node TreeOf(const MMember & m, const std::set<const void *> & changed) {
    Node n;
    n.id = 0;
    if (m.has_name) {
      Value(n, "name", m.name, changed.count(&m.name) != 0 || Pick(2) == 0);
    }
    n.Set("tag", Text(m.tag.c_str()));
    for (const MContact & c : m.contacts) {
      Node & k = n.Add("contacts", Item(c.id));
      k.Set("label", Text(c.label.c_str()));
      for (const MValue & e : c.emails) {
        Node & x = k.Add("emails", Item(e.id));
        Value(x, "", e, changed.count(&e) != 0 || Pick(2) == 0);
      }
    }
    return n;
  }

  void Record(MMember & m) {
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> h;
    if (m.has_name) {
      h.push_back({"name", {m.name.plain, m.name.subject}});
    }
    for (const MContact & c : m.contacts) {
      for (const MValue & e : c.emails) {
        h.push_back({"contacts.emails", {e.plain, e.subject}});
      }
    }
    m.held.push_back(h);
  }

  /* Read the ids the library gave back into the model, by position. */
  void Ids_(MMember & m) {
    Node cur = Current(m.pid);
    auto & cs = cur.kids["contacts"];
    ASSERT_EQ(m.contacts.size(), cs.size());
    for (size_t i = 0; i < cs.size(); i++) {
      m.contacts[i].id = cs[i].id;
      auto & es = cs[i].kids["emails"];
      ASSERT_EQ(m.contacts[i].emails.size(), es.size());
      for (size_t k = 0; k < es.size(); k++) {
        m.contacts[i].emails[k].id = es[k].id;
      }
    }
  }

  void Step() {
    MMember & m = members[Pick(members.size())];
    std::set<const void *> changed;
    ops += 1;
    switch (Pick(9)) {
    case 0:
    case 1:
      m.has_name = true;
      m.name = Fresh();
      changed.insert(&m.name);
      break;
    case 2:
      m.has_name = false;
      break;
    case 3:
      /* The root item changes; the name, if any, is carried. */
      m.tag = "t" + std::to_string(Pick(1000));
      carried += m.has_name ? 1 : 0;
      break;
    case 4: {
      MContact c;
      c.label = "l" + std::to_string(Pick(5));
      for (size_t i = 0, n = 1 + Pick(2); i < n; i++) {
        c.emails.push_back(Fresh());
      }
      m.contacts.push_back(c);
      for (const MValue & e : m.contacts.back().emails) {
        changed.insert(&e);
      }
      break;
    }
    case 5:
      if (!m.contacts.empty()) {
        m.contacts.erase(m.contacts.begin() + static_cast<long>(
            Pick(m.contacts.size())));
      }
      break;
    case 6:
      if (!m.contacts.empty()) {
        MContact & c = m.contacts[Pick(m.contacts.size())];
        if (!c.emails.empty() && Pick(2) == 0) {
          c.emails.erase(c.emails.begin() + static_cast<long>(
              Pick(c.emails.size())));
        } else {
          c.emails.push_back(Fresh());
          changed.insert(&c.emails.back());
        }
      }
      break;
    case 7:
      if (!m.contacts.empty()) {
        MContact & c = m.contacts[Pick(m.contacts.size())];
        c.label = "l" + std::to_string(Pick(1000));
      }
      break;
    default: {
      /* Erasure's data-store step for a subject someone holds. */
      std::vector<std::string> held;
      for (const MMember & x : members) {
        if (x.has_name) {
          held.push_back(x.name.subject);
        }
      }
      if (!held.empty()) {
        std::string s = held[Pick(held.size())];
        ASSERT_EQ(GDEFSTOR_OK, DeleteSubject(s));
        erased.insert(s);
      }
      return;
    }
    }
    Node n = TreeOf(m, changed);
    GDEFSTOR_Tree * t = Build(n);
    uint64_t rev = 0;
    ASSERT_EQ(GDEFSTOR_OK, Save(m.pid, m.head, t, &rev));
    gdefstor_tree_free(t);
    if (rev == 0) {
      return; /* the tree equals the current one */
    }
    ASSERT_EQ(m.head + 1, rev);
    m.head = rev;
    Record(m);
    Ids_(m);
  }

  void Check() {
    for (const char * field : {"name", "contacts.emails"}) {
      for (int p = 0; p < 7; p++) {
        std::string plain = "p" + std::to_string(p);
        /* Blind equality: members whose current revision holds the value,
         * of a subject not erased. */
        std::vector<std::string> want;
        std::set<std::string> want_history;
        for (const MMember & m : members) {
          bool now = false;
          for (size_t r = 1; r < m.held.size(); r++) {
            for (const auto & h : m.held[r]) {
              if (h.first == field && h.second.first == plain &&
                  erased.count(h.second.second) == 0) {
                want_history.insert(m.pid + " " + std::to_string(r));
                now |= r == m.head;
              }
            }
          }
          if (now) {
            want.push_back(m.pid);
          }
        }
        matched += want.empty() ? 0 : 1;
        EXPECT_EQ(Ids(want, want.size()), Blind(kMember, field, plain))
            << field << " " << plain;
        std::set<std::string> got_history;
        for (const std::string & h : History(kMember, field, plain)) {
          std::vector<std::string> w = Words(h);
          size_t dash = w[1].find('-');
          uint64_t from = std::stoull(w[1].substr(0, dash));
          uint64_t to = std::stoull(w[1].substr(dash + 1));
          const MMember * m = nullptr;
          for (const MMember & x : members) {
            m = x.pid == w[0] ? &x : m;
          }
          ASSERT_NE(nullptr, m) << h;
          for (uint64_t r = from; r < (to == 0 ? m->head + 1 : to); r++) {
            got_history.insert(w[0] + " " + std::to_string(r));
          }
        }
        EXPECT_EQ(want_history, got_history) << field << " " << plain;
      }
    }
    for (const char * s : {"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8"}) {
      /* Every value naming the subject, erased or not. */
      std::set<std::string> want;
      for (const MMember & m : members) {
        for (size_t r = 1; r < m.held.size(); r++) {
          for (const auto & h : m.held[r]) {
            if (h.second.second == s) {
              want.insert(m.pid + " " + h.first + " " + std::to_string(r));
            }
          }
        }
      }
      std::set<std::string> got;
      for (const std::string & h : Subject(s)) {
        std::vector<std::string> w = Words(h);
        size_t dash = w[3].find('-');
        uint64_t from = std::stoull(w[3].substr(0, dash));
        uint64_t to = std::stoull(w[3].substr(dash + 1));
        const MMember * m = nullptr;
        for (const MMember & x : members) {
          m = x.pid == w[1] ? &x : m;
        }
        ASSERT_NE(nullptr, m) << h;
        for (uint64_t r = from; r < (to == 0 ? m->head + 1 : to); r++) {
          got.insert(w[1] + " " + w[2] + " " + std::to_string(r));
        }
      }
      EXPECT_EQ(want, got) << s;
    }
  }
};

TEST_F(ModelBlindSweep, SeededHistoriesAgreeWithAnIndependentModel) {
  for (uint32_t seed : {11u, 29u, 4242u}) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    if (seed != 11u) {
      TearDown();
      SetUp();
    }
    rng.seed(seed);
    members.clear();
    erased.clear();
    for (int i = 0; i < 4; i++) {
      MMember m;
      m.pid = "m" + std::to_string(i);
      m.held.emplace_back(); /* revision 0: nothing */
      m.tag = "t";
      ASSERT_EQ(GDEFSTOR_OK, Create(kMember, m.pid));
      members.push_back(m);
    }
    for (int i = 0; i < 48; i++) {
      Step();
      if (HasFatalFailure()) {
        return;
      }
      if (i % 8 == 7) {
        Check();
      }
    }
    Check();
  }
  /* A sweep that never matched, carried or erased would pass vacuously. */
  EXPECT_GT(matched, 10);
  EXPECT_GT(carried, 2);
  EXPECT_FALSE(erased.empty());
  EXPECT_GT(ops, 100);
}

/* ---- isolation and the write --------------------------------------------- */

class ModelWrite : public ModelSave {};

TEST_F(ModelWrite, AnOpenWriteIsInvisible) {
  SetUpPo();
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  Node now = Current("po1");
  now.Set("total", Decimal(7));
  GDEFSTOR_Tree * t = Build(now);
  GDEFSTOR_RevisionMeta m = Meta();
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_save(w, "po1", 3, 1, &m, kLang, kLangLen, t, nullptr));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_create(w, kPo, std::strlen(kPo), "po2",
      3, "", 0, 1, nullptr));
  /* Inside the write, the next save builds on the one just written. */
  now.Set("total", Decimal(8));
  gdefstor_tree_free(t);
  t = Build(now);
  uint64_t rev = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_save(w, "po1", 3, 2, &m, kLang, kLangLen, t, &rev));
  EXPECT_EQ(3u, rev);
  gdefstor_tree_free(t);
  /* Outside it, the last commit. */
  EXPECT_EQ(1u, Head("po1"));
  EXPECT_EQ(Val::From(Decimal(3499)), Current("po1").vals["total"]);
  GDEFSTOR_Entity * e = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_get(store, kTenant, 2, "po2",
      3, &e));
  Q q;
  EXPECT_EQ(Ids({"po1"}, 1), Ask(store, kPo, nullptr));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(w));
  EXPECT_EQ(3u, Head("po1"));
  EXPECT_EQ(Ids({"po1", "po2"}, 2), Ask(store, kPo, nullptr));
  /* Both revisions of one write share its commit sequence. */
  GDEFSTOR_History * h = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_history(store, kTenant, 2, "po1", 3, nullptr, 0, &h));
  GDEFSTOR_RevisionInfo r1;
  GDEFSTOR_RevisionInfo r2;
  GDEFSTOR_RevisionInfo r3;
  std::memset(&r1, 0, sizeof r1);
  r1.size = sizeof r1;
  r2 = r1;
  r3 = r1;
  gdefstor_history_revision(h, 0, &r1);
  gdefstor_history_revision(h, 1, &r2);
  gdefstor_history_revision(h, 2, &r3);
  EXPECT_EQ(r2.commit_seq, r3.commit_seq);
  EXPECT_LT(r1.commit_seq, r2.commit_seq);
  gdefstor_history_free(h);
}

TEST_F(ModelWrite, AbortLeavesNothing) {
  SetUpPo();
  std::string truth = Dump(GDEFSTOR_MODEL_TRUTH);
  std::string current = Dump(GDEFSTOR_MODEL_CURRENT);
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  Node now = Current("po1");
  now.kids["lines"].clear();
  GDEFSTOR_Tree * t = Build(now);
  GDEFSTOR_RevisionMeta m = Meta();
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_save(w, "po1", 3, 1, &m, kLang, kLangLen, t, nullptr));
  gdefstor_tree_free(t);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_set_state(w, "po1", 3, 1, 0, "", 0, 1,
      nullptr));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_create(w, kPo, std::strlen(kPo), "po2",
      3, "", 0, 1, nullptr));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_rebuild(w));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
  EXPECT_EQ(truth, Dump(GDEFSTOR_MODEL_TRUTH));
  EXPECT_EQ(current, Dump(GDEFSTOR_MODEL_CURRENT));
  /* A refused call leaves its write usable. */
  ASSERT_EQ(GDEFSTOR_OK, InWrite([&](GDEFSTOR_Write * x) {
    Node bad = Current("po1");
    bad.vals.erase("number");
    GDEFSTOR_Tree * b = Build(bad);
    GDEFSTOR_Result rc = gdefstor_entity_save(x, "po1", 3, 1, &m, kLang, kLangLen, b, nullptr);
    gdefstor_tree_free(b);
    if (rc != GDEFSTOR_ERR_INVALID) {
      return GDEFSTOR_ERR_INTERNAL;
    }
    Node good = Current("po1");
    good.Set("total", Decimal(1));
    b = Build(good);
    rc = gdefstor_entity_save(x, "po1", 3, 1, &m, kLang, kLangLen, b, nullptr);
    gdefstor_tree_free(b);
    return rc;
  }));
  EXPECT_EQ(2u, Head("po1"));
}

/* ---- the writer queue ------------------------------------------------------ */

/* Writes to one tenant are serializable (store.h): a second writer on a fiber
 * parks until the first commits or aborts, and then sees what it committed;
 * a caller that is not a fiber is told the tenant is busy; another tenant's
 * writer never waits. */
class ModelWriters : public ModelTest {
 public:
  struct Park {
    GDEFSTOR_Store * store = nullptr;
    GDEFSTOR_Result begin_rc = GDEFSTOR_RESULT_COUNT;
    GDEFSTOR_Result get_rc = GDEFSTOR_RESULT_COUNT;
    int found = -1;
  };

  static void ParkEntry(void * arg) {
    Park * park = static_cast<Park *>(arg);
    GDEFSTOR_Write * w = nullptr;
    park->begin_rc = gdefstor_write_begin(park->store, kTenant, 2, nullptr, &w);
    if (park->begin_rc != GDEFSTOR_OK) {
      return;
    }
    uint8_t * bytes = nullptr;
    size_t len = 0;
    park->get_rc = gdefstor_record_get(park->store, kTenant, 2, "core.q", 6,
        "k", 1, &park->found, &bytes, &len, nullptr, nullptr);
    gdefstor_bytes_free(nullptr, bytes);
    gdefstor_write_abort(w);
  }

  GDEFSTOR_Result Hold(GDEFSTOR_Write ** out) {
    GDEFSTOR_Result rc = gdefstor_write_begin(store, kTenant, 2, nullptr, out);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_record_put(*out, "core.q", 6, "k", 1,
          kStamp, kStampLen,
          reinterpret_cast<const uint8_t *>("v"), 1);
    }
    return rc;
  }

  int Found() {
    uint8_t * bytes = nullptr;
    size_t len = 0;
    int found = -1;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_record_get(store, kTenant, 2, "core.q", 6,
        "k", 1, &found, &bytes, &len, nullptr, nullptr));
    gdefstor_bytes_free(nullptr, bytes);
    return found;
  }
};

/* Where cutil has no fibers (GCU_FIBER_SUPPORTED is 0: every target but
 * x86-64 and arm64 ELF, and Windows), no writer is ever on one, so a writer
 * cannot park: the two tests of parking skip there, once the platform is seen
 * to refuse a fiber, and AWriterThatIsNotAFiberIsToldTheTenantIsBusy is what
 * holds. suite/tools/xarch's first run (story S10) met it on i686, s390x,
 * PowerPC and SPARC64. */
#if GCU_FIBER_SUPPORTED
#define GDEFSTOR_TEST_NEEDS_FIBERS() (void)0
#else
#define GDEFSTOR_TEST_NEEDS_FIBERS()                                          \
  do {                                                                         \
    GCU_Fiber * none = nullptr;                                                \
    ASSERT_EQ(GCU_FIBER_ERR_UNSUPPORTED, gcu_fiber_create(&none, ParkEntry,    \
        nullptr, GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));                      \
    GTEST_SKIP() << "cutil has no fibers on this target, so no writer parks"; \
  } while (0)
#endif

TEST_F(ModelWriters, ASecondWriterOnAFiberParksUntilTheFirstCommits) {
  GDEFSTOR_TEST_NEEDS_FIBERS();
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, Hold(&w));
  Park park;
  park.store = store;
  GCU_Fiber * fiber = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fiber, ParkEntry, &park,
      GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fiber));
  EXPECT_EQ(GDEFSTOR_RESULT_COUNT, park.begin_rc) << "the second writer ran";
  EXPECT_EQ(0, Found());
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(w));
  EXPECT_EQ(GDEFSTOR_OK, park.begin_rc);
  EXPECT_EQ(GDEFSTOR_OK, park.get_rc);
  EXPECT_EQ(1, park.found);
  gcu_fiber_destroy(fiber);
}

TEST_F(ModelWriters, AnAbortWakesTheParkedWriterToNothing) {
  GDEFSTOR_TEST_NEEDS_FIBERS();
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, Hold(&w));
  Park park;
  park.store = store;
  GCU_Fiber * fiber = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fiber, ParkEntry, &park,
      GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fiber));
  EXPECT_EQ(GDEFSTOR_RESULT_COUNT, park.begin_rc);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
  EXPECT_EQ(GDEFSTOR_OK, park.begin_rc);
  EXPECT_EQ(GDEFSTOR_OK, park.get_rc);
  EXPECT_EQ(0, park.found);
  gcu_fiber_destroy(fiber);
}

TEST_F(ModelWriters, AWriterThatIsNotAFiberIsToldTheTenantIsBusy) {
  GDEFSTOR_Write * w = nullptr;
  GDEFSTOR_Write * second = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  EXPECT_EQ(GDEFSTOR_ERR_BUSY,
      gdefstor_write_begin(store, kTenant, 2, nullptr, &second));
  GDEFSTOR_Write * other = nullptr;
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, "t2", 2, nullptr, &other));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(other));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, kTenant, 2, nullptr, &w));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
}

/* ---- the purchase order end to end --------------------------------------- */

TEST_F(ModelQuery, PurchaseOrderRoundTripsReordersRevertsAndLists) {
  Node po = Current("po1");
  EXPECT_EQ(NoIds(PoTree("PO-1", 3499, "c1")), NoIds(po));
  /* Reorder the lines, add a discount breakdown, move a tracking event to a
   * new package. */
  std::swap(po.kids["lines"][0], po.kids["lines"][1]);
  po.kids["lines"][1].kids["discounts"][0].Add("breakdown").Set("reason",
      Text("bulk")).Set("amount", Decimal(5));
  Node & ship = po.kids["shipments"][0];
  Node moved = ship.kids["packages"][0].kids["tracking"][0];
  ship.kids["packages"][0].kids["tracking"].clear();
  ship.Add("packages").Set("weight", Real(2.0)).Add("tracking", moved);
  GDEFSTOR_Tree * t = Build(po);
  ASSERT_EQ(GDEFSTOR_OK, Save("po1", 1, t));
  gdefstor_tree_free(t);
  Node two = Current("po1");
  EXPECT_EQ(NoIds(po), NoIds(two));
  EXPECT_EQ(moved.id, two.kids["shipments"][0].kids["packages"][1]
      .kids["tracking"][0].id);
  Q q;
  EXPECT_EQ(Ids({"po1"}, 1), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("lines.discounts.breakdown.reason"), q.Values({Text("bulk")}))));
  ASSERT_EQ(GDEFSTOR_OK, Revert("po1", 2, 1));
  EXPECT_EQ(Ids({}, 0), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("lines.discounts.breakdown.reason"), q.Values({Text("bulk")}))));
  GDEFSTOR_Tree * one = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_tree(store, kTenant, 2, "po1", 3, kLang, kLangLen, 1,
      &one));
  EXPECT_EQ(Read(one), Current("po1"));
  gdefstor_tree_free(one);
  EXPECT_EQ(Ids({"po1", "po2"}, 2), Ask(store, kPo, q.Cmp(GDEFSTOR_QUERY_EQ,
      q.Field("lines.taxes.name"), q.Values({Text("vat")})),
      {By(q.Field("number"))}));
}

/* ---- allocation failure ------------------------------------------------- */

/* Counts allocations, and fails the one numbered fail_at (1 is the first). */
struct OomCount {
  const GCU_Allocator * base = nullptr;
  long calls = 0;
  long live = 0;
  long fail_at = 0;
  long underflow = 0;
};

extern "C" {
static void * oom_malloc(void * ctx, size_t n) {
  OomCount * c = static_cast<OomCount *>(ctx);
  c->calls += 1;
  if (c->calls == c->fail_at) {
    return nullptr;
  }
  void * p = gcu_allocator_malloc(c->base, n);
  c->live += p != nullptr ? 1 : 0;
  return p;
}
static void * oom_calloc(void * ctx, size_t k, size_t n) {
  OomCount * c = static_cast<OomCount *>(ctx);
  c->calls += 1;
  if (c->calls == c->fail_at) {
    return nullptr;
  }
  void * p = gcu_allocator_calloc(c->base, k, n);
  c->live += p != nullptr ? 1 : 0;
  return p;
}
static void * oom_realloc(void * ctx, void * old, size_t n) {
  OomCount * c = static_cast<OomCount *>(ctx);
  c->calls += 1;
  if (c->calls == c->fail_at) {
    return nullptr;
  }
  void * p = gcu_allocator_realloc(c->base, old, n);
  if (old == nullptr && p != nullptr) {
    c->live += 1;
  }
  return p;
}
static void oom_free(void * ctx, void * p) {
  OomCount * c = static_cast<OomCount *>(ctx);
  if (p == nullptr) {
    return;
  }
  if (c->live == 0) {
    c->underflow += 1;
  }
  c->live -= 1;
  gcu_allocator_free(c->base, p);
}
}

/* One failure run's allocator: its own counter, so runs on different threads
 * never share one. port() is what a store is opened with. */
struct OomRun {
  OomCount count;
  GCU_Allocator alloc;
  explicit OomRun(long fail_at = 0) {
    count.base = gcu_allocator_default();
    count.fail_at = fail_at;
    alloc.ctx = &count;
    alloc.malloc_fn = oom_malloc;
    alloc.calloc_fn = oom_calloc;
    alloc.realloc_fn = oom_realloc;
    alloc.free_fn = oom_free;
  }
  OomRun(const OomRun &) = delete;
  OomRun & operator=(const OomRun &) = delete;
  const GDEFSTOR_Allocator * port() {
    return reinterpret_cast<const GDEFSTOR_Allocator *>(&alloc);
  }
};

/* How many threads a failure sweep runs on: GDEFSTOR_TEST_THREADS, or the
 * processors there are (at most sixteen), and at least one. Every iteration of
 * a sweep owns its allocator and its store, so the iterations are independent
 * and the sweep covers the same allocation points whatever the number is;
 * ModelOpen and ModelClose are called from several threads at once. */
inline unsigned OomThreads() {
  const char * env = std::getenv("GDEFSTOR_TEST_THREADS");
  long n = env != nullptr ? std::strtol(env, nullptr, 10) : 0;
  if (n < 1) {
    unsigned hw = std::thread::hardware_concurrency();
    n = hw < 1 ? 1 : (hw > 16 ? 16 : hw);
  }
  return static_cast<unsigned>(n);
}

/* Run @p work(worker_index) on @p count threads (on this one if count is 1)
 * and wait for all of them. */
template <class Work>
inline void OomWorkers(unsigned count, Work work) {
  if (count <= 1) {
    work(0u);
    return;
  }
  std::vector<std::thread> pool;
  for (unsigned w = 0; w < count; w++) {
    pool.emplace_back([&work, w]() { work(w); });
  }
  for (std::thread & t : pool) {
    t.join();
  }
}

/* The sweep every scenario test shares. @p scenario(alloc) opens a store with
 * the allocator, makes every kind of call once and closes it; the first error
 * stops it. It runs once with nothing failing (which has to succeed and leak
 * nothing), to count the allocations it makes, and then once for each of them
 * with that one failing: the answer has to be ERR_OOM (or OK, where the call
 * got past a failure it could do without), nothing leaks, nothing is freed
 * twice. @p min_points is how few allocations would mean the scenario no
 * longer exercises the library. Prints the number of points walked. */
template <class Scenario>
inline void OomSweepPoints(const char * what, long min_points, Scenario scenario) {
  OomRun first;
  ASSERT_EQ(GDEFSTOR_OK, scenario(first.port()));
  EXPECT_EQ(0, first.count.live);
  long budget = first.count.calls;
  ASSERT_GT(budget, min_points);
  std::printf("%s allocation failures: %ld points\n", what, budget);
  std::atomic<long> next(1);
  std::atomic<bool> stop(false);
  unsigned threads = OomThreads();
  if (static_cast<long>(threads) > budget) {
    threads = static_cast<unsigned>(budget);
  }
  OomWorkers(threads, [&](unsigned) {
    for (;;) {
      long i = next.fetch_add(1);
      if (i > budget || stop.load()) {
        return;
      }
      OomRun run(i);
      GDEFSTOR_Result rc = scenario(run.port());
      bool answered = rc == GDEFSTOR_ERR_OOM || rc == GDEFSTOR_OK;
      EXPECT_TRUE(answered)
          << "allocation " << i << ": " << gdefstor_result_string(rc);
      EXPECT_EQ(0, run.count.live) << "allocation " << i;
      EXPECT_EQ(0, run.count.underflow) << "allocation " << i;
      if (!answered || run.count.live != 0 || run.count.underflow != 0) {
        stop.store(true);
      }
    }
  });
}

/* Every kind of call once, over a store whose every allocation the test
 * controls. The first error stops it; every error must be ERR_OOM. */
inline GDEFSTOR_Result OomScenario(const GDEFSTOR_Allocator * alloc) {
  GDEFSTOR_Store * store = nullptr;
  GDEFSTOR_Result rc = ModelOpen(&store, alloc);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  auto in_write = [&](const std::function<GDEFSTOR_Result(GDEFSTOR_Write *)> & f) {
    GDEFSTOR_Write * w = nullptr;
    GDEFSTOR_Result r = gdefstor_write_begin(store, kTenant, 2, nullptr, &w);
    if (r != GDEFSTOR_OK) {
      return r;
    }
    r = f(w);
    if (r != GDEFSTOR_OK) {
      gdefstor_write_abort(w);
      return r;
    }
    return gdefstor_write_commit(w);
  };
  SpecHolder po(kPo, PurchaseOrder());
  SpecHolder cust("site.customer", {F("vip", GDEFSTOR_FIELD_BOOLEAN)});
  GDEFSTOR_RevisionMeta m = Meta();
  GDEFSTOR_Tree * t = nullptr;
  GDEFSTOR_Type * type = nullptr;
  uint8_t * bytes = nullptr;
  size_t len = 0;
  rc = in_write([&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result r = gdefstor_type_declare(w, &po.spec);
    if (r == GDEFSTOR_OK) {
      r = gdefstor_type_declare(w, &cust.spec);
    }
    if (r == GDEFSTOR_OK) {
      r = gdefstor_entity_create(w, kPo, std::strlen(kPo), "po1", 3, "u1", 2,
          1, nullptr);
    }
    if (r == GDEFSTOR_OK) {
      r = gdefstor_entity_create(w, "site.customer", 13, "c1", 2, "", 0, 1,
          nullptr);
    }
    return r;
  });
  if (rc == GDEFSTOR_OK) {
    t = Build(PoTree("PO-1", 100, "c1"));
    rc = t == nullptr ? GDEFSTOR_ERR_OOM : in_write([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_save(w, "po1", 3, 0, &m, kLang, kLangLen, t, nullptr);
    });
    gdefstor_tree_free(t);
    t = nullptr;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_entity_tree(store, kTenant, 2, "po1", 3, kLang, kLangLen, 0, &t);
  }
  if (rc == GDEFSTOR_OK) {
    Node n = Read(t);
    gdefstor_tree_free(t);
    t = nullptr;
    std::swap(n.kids["lines"][0], n.kids["lines"][1]);
    n.kids["notes"].pop_back();
    n.Set("total", Decimal(7));
    t = Build(n);
    rc = t == nullptr ? GDEFSTOR_ERR_OOM : in_write([&](GDEFSTOR_Write * w) {
      GDEFSTOR_Result r = gdefstor_entity_save(w, "po1", 3, 1, &m, kLang, kLangLen, t, nullptr);
      if (r == GDEFSTOR_OK) {
        r = gdefstor_entity_set_state(w, "po1", 3, 1, 0, "u1", 2, 2, nullptr);
      }
      return r;
    });
    gdefstor_tree_free(t);
    t = nullptr;
  }
  if (rc == GDEFSTOR_OK) {
    rc = in_write([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_revert(w, "po1", 3, 2, 1, &m, nullptr);
    });
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_type_get(store, kTenant, 2, kPo, std::strlen(kPo), &type);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_entity_tree(store, kTenant, 2, "po1", 3, kLang, kLangLen, 2, &t);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_tree_to_json(type, t, &bytes, &len);
    gdefstor_tree_free(t);
    t = nullptr;
  }
  uint8_t * fresh = nullptr;
  size_t fresh_len = 0;
  if (rc == GDEFSTOR_OK) {
    /* A new entity's draft names no ids. */
    t = Build(PoTree("PO-2", 5, "c1"));
    rc = t == nullptr ? GDEFSTOR_ERR_OOM
                      : gdefstor_tree_to_json(type, t, &fresh, &fresh_len);
    gdefstor_tree_free(t);
    t = nullptr;
  }
  if (rc == GDEFSTOR_OK) {
    rc = in_write([&](GDEFSTOR_Write * w) {
      GDEFSTOR_Result r = DraftPut(w, nullptr, 0, "po1", 3, 3, bytes,
          len);
      if (r == GDEFSTOR_OK) {
        r = DraftPut(w, kPo, std::strlen(kPo), "po2", 3, 0, fresh,
            fresh_len);
      }
      if (r == GDEFSTOR_OK) {
        r = gdefstor_draft_apply(w, "po1", 3, &m, nullptr);
      }
      if (r == GDEFSTOR_OK) {
        r = gdefstor_draft_apply(w, "po2", 3, &m, nullptr);
      }
      return r;
    });
  }
  gdefstor_bytes_free(alloc, bytes);
  bytes = nullptr;
  gdefstor_bytes_free(nullptr, fresh);
  gdefstor_type_free(type);
  if (rc == GDEFSTOR_OK) {
    Q q;
    GDEFSTOR_QuerySort s = By(q.Field("total"), true);
    GDEFSTOR_Query query;
    std::memset(&query, 0, sizeof query);
    query.size = sizeof query;
    query.language = kLang;
    query.language_len = kLangLen;
    query.where = q.Op(GDEFSTOR_QUERY_AND, {q.Op(GDEFSTOR_QUERY_EXISTS,
        {q.Cmp(GDEFSTOR_QUERY_EQ, q.Entity(), q.Field("customer", true))},
        "site.customer"), q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("lines.sku"),
        q.Values({Text("A-1")}))});
    query.sorts = &s;
    query.sort_count = 1;
    GDEFSTOR_QueryResult * r = nullptr;
    rc = gdefstor_entity_query(store, kTenant, 2, kPo, std::strlen(kPo), &query,
        &r);
    gdefstor_query_result_free(r);
  }
  if (rc == GDEFSTOR_OK) {
    GDEFSTOR_History * h = nullptr;
    rc = gdefstor_entity_history(store, kTenant, 2, "po1", 3, nullptr, 0, &h);
    gdefstor_history_free(h);
  }
  if (rc == GDEFSTOR_OK) {
    GDEFSTOR_Diff * d = nullptr;
    rc = gdefstor_entity_diff(store, kTenant, 2, "po1", 3, nullptr, 0, 1, 3, &d);
    gdefstor_diff_free(d);
  }
  if (rc == GDEFSTOR_OK) {
    rc = in_write([&](GDEFSTOR_Write * w) { return gdefstor_model_rebuild(w); });
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_model_dump(store, kTenant, 2, GDEFSTOR_MODEL_TRUTH, &bytes,
        &len);
    gdefstor_bytes_free(alloc, bytes);
  }
  ModelClose(store);
  return rc;
}

/* Every allocation the entity port makes can fail: the call reports
 * ERR_OOM, nothing leaks, nothing is freed twice, and nothing crashes. */
TEST(ModelOom, EveryAllocationFailureIsReportedAndCleanedUp) {
  OomSweepPoints("entity", 100, OomScenario);
}

/* Every blind-index call once, over a store whose every allocation the test
 * controls: a save with MACs, a carried and a reverted value, blind equality
 * and a count, the history and subject lookups, a deletion, a dump. */
inline GDEFSTOR_Result BlindOomScenario(const GDEFSTOR_Allocator * alloc) {
  GDEFSTOR_Store * store = nullptr;
  GDEFSTOR_Result rc = ModelOpen(&store, alloc);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  auto in_write = [&](const std::function<GDEFSTOR_Result(GDEFSTOR_Write *)> & f) {
    GDEFSTOR_Write * w = nullptr;
    GDEFSTOR_Result r = gdefstor_write_begin(store, kTenant, 2, nullptr, &w);
    if (r != GDEFSTOR_OK) {
      return r;
    }
    r = f(w);
    if (r != GDEFSTOR_OK) {
      gdefstor_write_abort(w);
      return r;
    }
    return gdefstor_write_commit(w);
  };
  SpecHolder member(kMember, MemberType());
  SpecHolder gift(kGift, GiftType());
  GDEFSTOR_RevisionMeta m = Meta();
  std::string ann = MacOf("ann");
  std::string cy = MacOf("cy");
  rc = in_write([&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result r = gdefstor_type_declare(w, &member.spec);
    if (r == GDEFSTOR_OK) {
      r = gdefstor_type_declare(w, &gift.spec);
    }
    if (r == GDEFSTOR_OK) {
      r = gdefstor_entity_create(w, kMember, std::strlen(kMember), "m1", 2, "",
          0, 1, nullptr);
    }
    return r;
  });
  for (int step = 0; rc == GDEFSTOR_OK && step < 3; step++) {
    /* The name, then a new name, then only the tag (the name carried). */
    Node n;
    n.Set("name", Personal(GDEFSTOR_FIELD_TEXT, step == 0 ? "c1" : "c2", "s1"));
    if (step < 2) {
      n.Mac("name", step == 0 ? ann : cy);
    }
    n.Set("tag", Text(step == 2 ? "y" : "x"));
    Node & k = n.Add("contacts");
    k.Set("label", Text("home"));
    k.Add("emails", One(Personal(GDEFSTOR_FIELD_TEXT, "e1", "s1"))).Mac("", ann);
    if (step > 0) {
      /* Name the items the first save made, so they continue. */
      GDEFSTOR_Tree * cur = nullptr;
      rc = gdefstor_entity_tree(store, kTenant, 2, "m1", 2, kLang, kLangLen, 0, &cur);
      if (rc != GDEFSTOR_OK) {
        break;
      }
      Node was = Read(cur);
      gdefstor_tree_free(cur);
      n.id = was.id;
      k.id = was.kids["contacts"][0].id;
      k.kids["emails"][0].id = was.kids["contacts"][0].kids["emails"][0].id;
    }
    GDEFSTOR_Tree * t = Build(n);
    rc = t == nullptr ? GDEFSTOR_ERR_OOM : in_write([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_save(w, "m1", 2, static_cast<uint64_t>(step), &m,
          kLang, kLangLen, t, nullptr);
    });
    gdefstor_tree_free(t);
  }
  if (rc == GDEFSTOR_OK) {
    rc = in_write([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_revert(w, "m1", 2, 3, 1, &m, nullptr);
    });
  }
  if (rc == GDEFSTOR_OK) {
    Q q;
    GDEFSTOR_Query query;
    std::memset(&query, 0, sizeof query);
    query.size = sizeof query;
    query.language = kLang;
    query.language_len = kLangLen;
    query.where = q.Op(GDEFSTOR_QUERY_OR, {q.Cmp(GDEFSTOR_QUERY_BLIND,
        q.Field("name"), q.Mac(ann)), q.Cmp(GDEFSTOR_QUERY_BLIND,
            q.Field("contacts.emails"), q.Mac(ann))});
    GDEFSTOR_QueryResult * r = nullptr;
    rc = gdefstor_entity_query(store, kTenant, 2, kMember, std::strlen(kMember),
        &query, &r);
    gdefstor_query_result_free(r);
    uint64_t n = 0;
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_count(store, kTenant, 2, kMember,
          std::strlen(kMember), kLang, kLangLen, query.where, &n);
    }
  }
  if (rc == GDEFSTOR_OK) {
    GDEFSTOR_BlindHits * h = nullptr;
    rc = gdefstor_blind_history(store, kTenant, 2, kMember, std::strlen(kMember),
        "name", 4, reinterpret_cast<const uint8_t *>(ann.data()), ann.size(), &h);
    gdefstor_blind_hits_free(h);
  }
  if (rc == GDEFSTOR_OK) {
    GDEFSTOR_SubjectHits * h = nullptr;
    rc = gdefstor_subject_lookup(store, kTenant, 2, "s1", 2, &h);
    gdefstor_subject_hits_free(h);
  }
  if (rc == GDEFSTOR_OK) {
    rc = in_write([&](GDEFSTOR_Write * w) {
      return gdefstor_blind_delete_subject(w, "s1", 2);
    });
  }
  if (rc == GDEFSTOR_OK) {
    uint8_t * bytes = nullptr;
    size_t len = 0;
    rc = gdefstor_model_dump(store, kTenant, 2, GDEFSTOR_MODEL_TRUTH, &bytes,
        &len);
    gdefstor_bytes_free(alloc, bytes);
  }
  ModelClose(store);
  return rc;
}

TEST(ModelOom, EveryBlindIndexCallReportsAnAllocationFailure) {
  OomSweepPoints("blind-index", 50, BlindOomScenario);
}

/* ---- the sweep: seeded random histories against an independent model ---- */

/* The model knows the declaration as a tree of its own, and an entity as the
 * list of its trees by revision. It decides what each operation must do from
 * the design's rules alone: what a save keeps, what a revert restores, what
 * retired fields keep, and what a query answers. Item ids are the library's to
 * give; the model checks that every new one is fresh, then uses it. */
struct DField {
  std::string name;
  int type = GDEFSTOR_FIELD_TEXT;
  int scale = 0;
  bool multiple = false;
  bool retired = false;
  std::vector<DField> kids;
  bool group() const { return type == GDEFSTOR_FIELD_GROUP; }
};

struct MEntity {
  std::string pid;
  int64_t created = 0;
  std::vector<Node> snaps; /* [0] is empty: no revision */
  std::set<uint64_t> ids;  /* every item id it has ever had */
  int published = 0;
  int deleted = 0;
};

/* What a sweep did: a sweep that never wrote, reverted or matched anything
 * would pass, so the test requires each to have happened. */
struct SweepCounts {
  int revisions = 0;
  int unchanged = 0;
  int reverts = 0;
  int revert_unchanged = 0;
  int drafts = 0;
  int conflicts = 0;
  int declarations = 0;
  int retired = 0;
  int queries = 0;
  int queries_with_rows = 0;
  int queries_empty = 0;
  int nested_paths = 0;
  int moves = 0;
};

class Sweep {
 public:
  SweepCounts counts;

  Sweep(ModelTest * t, GDEFSTOR_Store * store, uint32_t seed)
      : t_(t), store_(store), rng_(seed), seed_(seed) {}

  std::mt19937 & Rng() { return rng_; }
  size_t Pick(size_t n) {
    return n == 0 ? 0 : std::uniform_int_distribution<size_t>(0, n - 1)(rng_);
  }
  bool Chance(double p) {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < p;
  }

  /* -- the declaration -- */

  std::vector<DField> RandomGroup(int depth) {
    std::vector<DField> out;
    size_t n = 1 + Pick(4);
    for (size_t i = 0; i < n; i++) {
      DField f;
      f.name = "f" + std::to_string(next_name_++);
      if (depth < 3 && Chance(0.3)) {
        f.type = GDEFSTOR_FIELD_GROUP;
        f.kids = RandomGroup(depth + 1);
      } else {
        static const int scalars[] = {GDEFSTOR_FIELD_TEXT,
            GDEFSTOR_FIELD_INTEGER, GDEFSTOR_FIELD_DECIMAL, GDEFSTOR_FIELD_FLOAT,
            GDEFSTOR_FIELD_BOOLEAN, GDEFSTOR_FIELD_DATE, GDEFSTOR_FIELD_DATETIME,
            GDEFSTOR_FIELD_REFERENCE};
        f.type = scalars[Pick(8)];
        f.scale = f.type == GDEFSTOR_FIELD_DECIMAL ? static_cast<int>(Pick(4)) : 0;
      }
      f.multiple = Chance(0.4);
      out.push_back(f);
    }
    return out;
  }

  static std::vector<Field> Spec(const std::vector<DField> & group) {
    std::vector<Field> out;
    for (const DField & d : group) {
      if (d.retired) {
        continue;
      }
      Field f;
      f.name = d.name;
      f.type = d.type;
      f.scale = d.scale;
      f.flags = d.multiple ? GDEFSTOR_FIELD_MULTIPLE : 0;
      f.kids = Spec(d.kids);
      out.push_back(f);
    }
    return out;
  }

  bool Declare() {
    std::vector<Field> spec = Spec(decl_);
    return t_->Declare(type_, spec) == GDEFSTOR_OK;
  }

  /* Add, retire or restore one field somewhere. */
  void ChangeDeclaration() {
    std::vector<std::vector<DField> *> groups;
    std::function<void(std::vector<DField> &)> walk =
        [&](std::vector<DField> & g) {
          groups.push_back(&g);
          for (DField & d : g) {
            if (d.group() && !d.retired) {
              walk(d.kids);
            }
          }
        };
    walk(decl_);
    std::vector<DField> & g = *groups[Pick(groups.size())];
    size_t choice = Pick(3);
    if (choice == 0 && g.size() < 8) {
      std::vector<DField> add = RandomGroup(3);
      g.push_back(add[0]);
    } else if (choice == 1) {
      std::vector<DField *> live;
      for (DField & d : g) {
        if (!d.retired) {
          live.push_back(&d);
        }
      }
      /* Keep one live field in every group: a group of nothing is refused. */
      if (live.size() > 1) {
        live[Pick(live.size())]->retired = true;
        counts.retired += 1;
      }
    } else {
      for (DField & d : g) {
        if (d.retired) {
          d.retired = false;
          break;
        }
      }
    }
    EXPECT_TRUE(Declare()) << "seed " << seed_;
    counts.declarations += 1;
  }

  /* -- values -- */

  GDEFSTOR_Value RandomValue(const DField & f) {
    static const char * const texts[] = {"", "a", "b", "abc", "\xC3\xA9", "z"};
    static const char * const refs[] = {"r1", "r2", "r3"};
    static const double reals[] = {0.0, 1.5, -2.25, 1e10, 0.1};
    switch (f.type) {
    case GDEFSTOR_FIELD_TEXT:
      return Text(texts[Pick(6)]);
    case GDEFSTOR_FIELD_REFERENCE:
      return Ref(refs[Pick(3)]);
    case GDEFSTOR_FIELD_FLOAT:
      return Real(reals[Pick(5)]);
    case GDEFSTOR_FIELD_BOOLEAN:
      return Int(f.type, static_cast<int64_t>(Pick(2)));
    case GDEFSTOR_FIELD_DATE:
      return Int(f.type, 19000 + static_cast<int64_t>(Pick(6)));
    case GDEFSTOR_FIELD_DATETIME:
      return Int(f.type, INT64_C(1700000000000) + static_cast<int64_t>(Pick(6)));
    default:
      return Int(f.type, static_cast<int64_t>(Pick(9)) - 4);
    }
  }

  /* -- trees -- */

  Node RandomItem(const std::vector<DField> & group) {
    Node n;
    for (const DField & d : group) {
      if (d.retired) {
        continue;
      }
      if (d.group()) {
        size_t k = d.multiple ? Pick(3) : Pick(2);
        for (size_t i = 0; i < k; i++) {
          n.Add(d.name, RandomItem(d.kids));
        }
      } else if (d.multiple) {
        size_t k = Pick(3);
        for (size_t i = 0; i < k; i++) {
          n.Add(d.name, One(RandomValue(d)));
        }
      } else if (Chance(0.7)) {
        n.Set(d.name, RandomValue(d));
      }
    }
    return n;
  }

  void Mutate(Node & n, const std::vector<DField> & group) {
    for (const DField & d : group) {
      if (d.retired) {
        continue;
      }
      if (!d.group() && !d.multiple) {
        if (Chance(0.3)) {
          n.Set(d.name, RandomValue(d));
        } else if (Chance(0.04)) {
          n.vals.erase(d.name);
        }
        continue;
      }
      std::vector<Node> & items = n.kids[d.name];
      if (Chance(0.25) && (d.multiple || items.empty())) {
        Node fresh = d.group() ? RandomItem(d.kids) : One(RandomValue(d));
        items.insert(items.begin() + static_cast<long>(Pick(items.size() + 1)),
            fresh);
      }
      if (!items.empty() && Chance(0.08)) {
        items.erase(items.begin() + static_cast<long>(Pick(items.size())));
      }
      if (items.size() > 1 && Chance(0.12)) {
        counts.moves += 1;
        size_t from = Pick(items.size());
        Node moved = items[from];
        items.erase(items.begin() + static_cast<long>(from));
        items.insert(items.begin() + static_cast<long>(Pick(items.size() + 1)),
            moved);
      }
      for (Node & k : items) {
        if (d.group()) {
          if (Chance(0.5)) {
            Mutate(k, d.kids);
          }
        } else if (Chance(0.2)) {
          k.vals[""] = Val::From(RandomValue(d));
        }
      }
      if (items.empty()) {
        n.kids.erase(d.name);
      }
    }
  }

  /* Leave out the retired parts at random: absent means "keep". */
  void Strip(Node & n, const std::vector<DField> & group) {
    for (const DField & d : group) {
      if (d.retired && Chance(0.5)) {
        n.vals.erase(d.name);
        n.kids.erase(d.name);
        continue;
      }
      if (d.group() && n.kids.count(d.name) != 0) {
        for (Node & k : n.kids[d.name]) {
          Strip(k, d.kids);
        }
      }
    }
  }

  /* The tree a revert to @p past must leave: the past tree, with every
   * retired part as it is now on the items that are still there. */
  static Node Reverted(const Node & past, const Node * now,
      const std::vector<DField> & group) {
    Node out;
    out.id = past.id;
    for (const DField & d : group) {
      if (d.retired) {
        if (now != nullptr) {
          if (now->vals.count(d.name) != 0) {
            out.vals[d.name] = now->vals.at(d.name);
          }
          if (now->kids.count(d.name) != 0) {
            out.kids[d.name] = now->kids.at(d.name);
          }
        }
        continue;
      }
      if (past.vals.count(d.name) != 0) {
        out.vals[d.name] = past.vals.at(d.name);
      }
      if (past.kids.count(d.name) == 0) {
        continue;
      }
      for (const Node & k : past.kids.at(d.name)) {
        if (!d.group()) {
          out.kids[d.name].push_back(k);
          continue;
        }
        const Node * match = nullptr;
        if (now != nullptr) {
          FindById(*now_root_, k.id, &match);
        }
        out.kids[d.name].push_back(Reverted(k, match, d.kids));
      }
    }
    if (past.vals.count("") != 0) {
      out.vals[""] = past.vals.at("");
    }
    return out;
  }

  static bool FindById(const Node & n, uint64_t id, const Node ** out) {
    if (n.id == id) {
      *out = &n;
      return true;
    }
    for (const auto & kv : n.kids) {
      for (const Node & k : kv.second) {
        if (FindById(k, id, out)) {
          return true;
        }
      }
    }
    return false;
  }

  /* @p got is @p want with every new item (id 0 in want) given a fresh id. */
  bool Adopts(const Node & want, const Node & got, MEntity & e,
      std::set<uint64_t> & fresh) {
    if (want.id != 0 && want.id != got.id) {
      return false;
    }
    if (want.id == 0) {
      if (e.ids.count(got.id) != 0 || fresh.count(got.id) != 0 || got.id == 0) {
        return false;
      }
      fresh.insert(got.id);
    }
    if (want.vals != got.vals) {
      return false;
    }
    std::set<std::string> fields;
    for (const auto & kv : want.kids) {
      if (!kv.second.empty()) {
        fields.insert(kv.first);
      }
    }
    for (const auto & kv : got.kids) {
      if (!kv.second.empty()) {
        fields.insert(kv.first);
      }
    }
    for (const std::string & f : fields) {
      auto a = want.kids.find(f);
      auto b = got.kids.find(f);
      if (a == want.kids.end() || b == got.kids.end() ||
          a->second.size() != b->second.size()) {
        return false;
      }
      for (size_t i = 0; i < a->second.size(); i++) {
        if (!Adopts(a->second[i], b->second[i], e, fresh)) {
          return false;
        }
      }
    }
    return true;
  }

  static void Collect(const Node & n, std::set<uint64_t> & ids) {
    ids.insert(n.id);
    for (const auto & kv : n.kids) {
      for (const Node & k : kv.second) {
        Collect(k, ids);
      }
    }
  }

  static bool Equal(const Node & a, const Node & b) {
    if (a.id != b.id || a.vals != b.vals) {
      return false;
    }
    std::set<std::string> fields;
    for (const auto & kv : a.kids) {
      if (!kv.second.empty()) {
        fields.insert(kv.first);
      }
    }
    for (const auto & kv : b.kids) {
      if (!kv.second.empty()) {
        fields.insert(kv.first);
      }
    }
    for (const std::string & f : fields) {
      auto x = a.kids.find(f);
      auto y = b.kids.find(f);
      if (x == a.kids.end() || y == b.kids.end() ||
          x->second.size() != y->second.size()) {
        return false;
      }
      for (size_t i = 0; i < x->second.size(); i++) {
        if (!Equal(x->second[i], y->second[i])) {
          return false;
        }
      }
    }
    return true;
  }

  /* The items whose values or parent differ between two trees, by id. */
  static void Flatten(const Node & n, uint64_t parent,
      std::map<uint64_t, std::pair<uint64_t, std::map<std::string, Val>>> & out) {
    out[n.id] = std::make_pair(parent, n.vals);
    for (const auto & kv : n.kids) {
      for (const Node & k : kv.second) {
        Flatten(k, n.id, out);
      }
    }
  }

  /* -- reading the library -- */

  Node CurrentOf(const MEntity & e, uint64_t rev = 0) {
    GDEFSTOR_Tree * t = nullptr;
    GDEFSTOR_Result rc = gdefstor_entity_tree(store_, kTenant, 2, e.pid.data(),
        e.pid.size(), kLang, kLangLen, rev, &t);
    EXPECT_EQ(GDEFSTOR_OK, rc) << "seed " << seed_ << " " << e.pid << "@" << rev;
    Node n = t == nullptr ? Node() : Read(t);
    gdefstor_tree_free(t);
    return n;
  }

  void CheckHistory(const MEntity & e) {
    for (size_t r = 1; r < e.snaps.size(); r++) {
      Node got = CurrentOf(e, r);
      EXPECT_TRUE(Equal(e.snaps[r], got)) << "seed " << seed_ << " " << e.pid
                                          << " as of " << r;
    }
    if (e.snaps.size() > 1) {
      EXPECT_TRUE(Equal(e.snaps.back(), CurrentOf(e))) << "seed " << seed_;
    }
  }

  /* After a write that should have made revision @p rev from @p prev_tree:
   * the rows it added are the items the diff names, and the diff's value and
   * parent changes are the model's. */
  void CheckRows(const MEntity & e, const std::string & before,
      const std::string & after, const Node & prev_tree, uint64_t rev) {
    EXPECT_EQ("", AppendOnly(before, after)) << "seed " << seed_;
    GDEFSTOR_Diff * d = nullptr;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_diff(store_, kTenant, 2,
        e.pid.data(), e.pid.size(), nullptr, 0, rev - 1, rev, &d));
    size_t written = 0;
    std::set<uint64_t> lib_changed;
    for (size_t i = 0; i < gdefstor_diff_count(d); i++) {
      GDEFSTOR_DiffItem x;
      std::memset(&x, 0, sizeof x);
      x.size = sizeof x;
      gdefstor_diff_item(d, i, &x);
      if (x.kind != GDEFSTOR_DIFF_REMOVED) {
        written += 1;
      }
      if (x.kind == GDEFSTOR_DIFF_CHANGED &&
          (x.what & (GDEFSTOR_DIFF_VALUES | GDEFSTOR_DIFF_PARENT)) != 0) {
        lib_changed.insert(x.item);
      }
    }
    gdefstor_diff_free(d);
    EXPECT_EQ(written, NewRows(before, after, 'I')) << "seed " << seed_;
    EXPECT_EQ(1u, NewRows(before, after, 'R')) << "seed " << seed_;
    std::map<uint64_t, std::pair<uint64_t, std::map<std::string, Val>>> a;
    std::map<uint64_t, std::pair<uint64_t, std::map<std::string, Val>>> b;
    if (rev > 1) {
      Flatten(prev_tree, 0, a);
    }
    Flatten(e.snaps[rev], 0, b);
    std::set<uint64_t> model_changed;
    for (const auto & kv : b) {
      auto it = a.find(kv.first);
      if (it != a.end() && it->second != kv.second) {
        model_changed.insert(kv.first);
      }
    }
    EXPECT_EQ(model_changed, lib_changed) << "seed " << seed_;
  }

  /* -- operations -- */

  void Create() {
    MEntity e;
    e.pid = "e" + std::to_string(entities_.size());
    e.created = static_cast<int64_t>(Pick(5));
    e.snaps.push_back(Node());
    ASSERT_EQ(GDEFSTOR_OK, t_->Create(type_, e.pid, "u1", e.created));
    entities_.push_back(e);
  }

  /* Save @p want (ids for kept items, 0 for new) on entity @p e. */
  void SaveTree(MEntity & e, Node want, bool via_draft) {
    uint64_t head = e.snaps.size() - 1;
    Node input = want;
    Strip(input, decl_);
    std::string before = t_->Dump(GDEFSTOR_MODEL_TRUTH);
    uint64_t rev = 0;
    GDEFSTOR_Result rc;
    if (via_draft) {
      GDEFSTOR_Type * type = nullptr;
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(store_, kTenant, 2, type_.data(),
          type_.size(), &type));
      GDEFSTOR_Tree * t = Build(input);
      uint8_t * json = nullptr;
      size_t len = 0;
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_tree_to_json(type, t, &json, &len));
      gdefstor_tree_free(t);
      gdefstor_type_free(type);
      rc = t_->InWrite([&](GDEFSTOR_Write * w) {
        return DraftPut(w, nullptr, 0, e.pid.data(), e.pid.size(),
            head, json, len);
      });
      gdefstor_bytes_free(nullptr, json);
      ASSERT_EQ(GDEFSTOR_OK, rc) << "seed " << seed_;
      GDEFSTOR_RevisionMeta m = Meta("u3", 5);
      rc = t_->InWrite([&](GDEFSTOR_Write * w) {
        return gdefstor_draft_apply(w, e.pid.data(), e.pid.size(), &m, &rev);
      });
    } else {
      GDEFSTOR_Tree * t = Build(input);
      rc = t_->Save(e.pid, head, t, &rev);
      gdefstor_tree_free(t);
    }
    ASSERT_EQ(GDEFSTOR_OK, rc) << "seed " << seed_;
    Node prev = e.snaps.back();
    if (head > 0 && Equal(want, prev)) {
      EXPECT_EQ(0u, rev) << "seed " << seed_ << ": an unchanged tree wrote";
      EXPECT_EQ(before, t_->Dump(GDEFSTOR_MODEL_TRUTH)) << "seed " << seed_;
      counts.unchanged += 1;
      return;
    }
    ASSERT_EQ(head + 1, rev) << "seed " << seed_;
    counts.revisions += 1;
    counts.drafts += via_draft ? 1 : 0;
    Node got = CurrentOf(e);
    std::set<uint64_t> fresh;
    want.id = head == 0 ? 0 : prev.id;
    ASSERT_TRUE(Adopts(want, got, e, fresh)) << "seed " << seed_ << " " << e.pid
                                             << " rev " << rev;
    e.snaps.push_back(got);
    Collect(got, e.ids);
    CheckRows(e, before, t_->Dump(GDEFSTOR_MODEL_TRUTH), prev, rev);
  }

  void Edit(MEntity & e, bool via_draft) {
    Node want = e.snaps.size() > 1 ? e.snaps.back() : RandomItem(decl_);
    if (e.snaps.size() > 1 && !Chance(0.15)) {
      Mutate(want, decl_);
    }
    SaveTree(e, want, via_draft);
  }

  void Revert(MEntity & e) {
    uint64_t head = e.snaps.size() - 1;
    if (head == 0) {
      return;
    }
    uint64_t to = 1 + Pick(head);
    now_root_ = &e.snaps.back();
    Node want = Reverted(e.snaps[to], &e.snaps.back(), decl_);
    want.id = e.snaps.back().id;
    std::string before = t_->Dump(GDEFSTOR_MODEL_TRUTH);
    uint64_t rev = 0;
    ASSERT_EQ(GDEFSTOR_OK, t_->Revert(e.pid, head, to, &rev)) << "seed " << seed_;
    if (Equal(want, e.snaps.back())) {
      EXPECT_EQ(0u, rev) << "seed " << seed_;
      counts.revert_unchanged += 1;
      return;
    }
    counts.reverts += 1;
    ASSERT_EQ(head + 1, rev) << "seed " << seed_;
    Node got = CurrentOf(e);
    EXPECT_TRUE(Equal(want, got)) << "seed " << seed_ << " revert of " << e.pid
                                  << " to " << to;
    Node prev = e.snaps.back();
    e.snaps.push_back(got);
    Collect(got, e.ids);
    CheckRows(e, before, t_->Dump(GDEFSTOR_MODEL_TRUTH), prev, rev);
  }

  void Stale(MEntity & e) {
    uint64_t head = e.snaps.size() - 1;
    std::string before = t_->Dump(GDEFSTOR_MODEL_TRUTH);
    Node want = e.snaps.size() > 1 ? e.snaps.back() : RandomItem(decl_);
    Mutate(want, decl_);
    GDEFSTOR_Tree * t = Build(want);
    EXPECT_EQ(GDEFSTOR_ERR_CONFLICT, t_->Save(e.pid, head + 1, t));
    if (head > 0) {
      EXPECT_EQ(GDEFSTOR_ERR_CONFLICT, t_->Save(e.pid, head - 1, t));
    }
    gdefstor_tree_free(t);
    EXPECT_EQ(before, t_->Dump(GDEFSTOR_MODEL_TRUTH)) << "seed " << seed_;
    counts.conflicts += 1;
  }

  void State(MEntity & e) {
    int pub = static_cast<int>(Pick(2));
    int del = Chance(0.2) ? 1 : 0;
    ASSERT_EQ(GDEFSTOR_OK, t_->InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_entity_set_state(w, e.pid.data(), e.pid.size(), pub, del,
          "u1", 2, 1, nullptr);
    }));
    e.published = pub;
    e.deleted = del;
  }

  /* -- queries, decided by the model -- */

  struct Leaf {
    std::vector<const DField *> path;
  };

  void Paths(const std::vector<DField> & group, std::vector<const DField *> & at,
      std::vector<std::vector<const DField *>> & out, bool root) {
    for (const DField & d : group) {
      if (d.retired) {
        continue;
      }
      at.push_back(&d);
      if (d.group()) {
        Paths(d.kids, at, out, false);
      } else {
        out.push_back(at);
        if (root && !d.multiple) {
          sortable_.push_back(&d);
        }
      }
      at.pop_back();
    }
  }

  static void Values(const Node & n, const std::vector<const DField *> & path,
      size_t at, std::vector<Val> & out) {
    const DField * d = path[at];
    if (at + 1 == path.size()) {
      if (d->multiple) {
        if (n.kids.count(d->name) != 0) {
          for (const Node & k : n.kids.at(d->name)) {
            out.push_back(k.vals.at(""));
          }
        }
      } else if (n.vals.count(d->name) != 0) {
        out.push_back(n.vals.at(d->name));
      }
      return;
    }
    if (n.kids.count(d->name) != 0) {
      for (const Node & k : n.kids.at(d->name)) {
        Values(k, path, at + 1, out);
      }
    }
  }

  static int Cmp(const Val & a, const Val & b) {
    if (a.type == GDEFSTOR_FIELD_TEXT || a.type == GDEFSTOR_FIELD_REFERENCE) {
      return a.b < b.b ? -1 : (b.b < a.b ? 1 : 0);
    }
    if (a.type == GDEFSTOR_FIELD_FLOAT) {
      return a.f < b.f ? -1 : (b.f < a.f ? 1 : 0);
    }
    return a.i < b.i ? -1 : (b.i < a.i ? 1 : 0);
  }

  /* A random predicate, built into @p q, with the model's own evaluator. */
  const GDEFSTOR_QueryNode * RandomWhere(Q & q, int depth,
      std::function<bool(const MEntity &)> & eval) {
    if (depth < 3 && Chance(0.35)) {
      int op = Chance(0.5) ? GDEFSTOR_QUERY_AND
                           : (Chance(0.7) ? GDEFSTOR_QUERY_OR : GDEFSTOR_QUERY_NOT);
      size_t n = op == GDEFSTOR_QUERY_NOT ? 1 : Pick(3);
      std::vector<const GDEFSTOR_QueryNode *> kids;
      std::vector<std::function<bool(const MEntity &)>> evals;
      for (size_t i = 0; i < n; i++) {
        std::function<bool(const MEntity &)> e;
        kids.push_back(RandomWhere(q, depth + 1, e));
        evals.push_back(e);
      }
      eval = [op, evals](const MEntity & e) {
        if (op == GDEFSTOR_QUERY_NOT) {
          return !evals[0](e);
        }
        for (const auto & f : evals) {
          bool r = f(e);
          if (op == GDEFSTOR_QUERY_AND && !r) {
            return false;
          }
          if (op == GDEFSTOR_QUERY_OR && r) {
            return true;
          }
        }
        return op == GDEFSTOR_QUERY_AND;
      };
      return q.Op(op, kids);
    }
    if (Chance(0.15)) {
      int want = static_cast<int>(Pick(2));
      eval = [want](const MEntity & e) { return e.published == want; };
      return q.Cmp(GDEFSTOR_QUERY_EQ, q.Meta(GDEFSTOR_META_PUBLISHED),
          q.Values({Int(GDEFSTOR_FIELD_BOOLEAN, want)}));
    }
    std::vector<std::vector<const DField *>> paths;
    std::vector<const DField *> at;
    sortable_.clear();
    Paths(decl_, at, paths, true);
    std::vector<const DField *> path = paths[Pick(paths.size())];
    const DField * leaf = path.back();
    bool ordered = leaf->type != GDEFSTOR_FIELD_TEXT &&
                   leaf->type != GDEFSTOR_FIELD_REFERENCE &&
                   leaf->type != GDEFSTOR_FIELD_BOOLEAN;
    static const int any_ops[] = {GDEFSTOR_QUERY_EQ, GDEFSTOR_QUERY_NE,
        GDEFSTOR_QUERY_IN};
    static const int all_ops[] = {GDEFSTOR_QUERY_EQ, GDEFSTOR_QUERY_NE,
        GDEFSTOR_QUERY_IN, GDEFSTOR_QUERY_LT, GDEFSTOR_QUERY_LE,
        GDEFSTOR_QUERY_GT, GDEFSTOR_QUERY_GE};
    int op = ordered ? all_ops[Pick(7)] : any_ops[Pick(3)];
    std::vector<GDEFSTOR_Value> consts;
    size_t nconst = op == GDEFSTOR_QUERY_IN ? Pick(3) : 1;
    std::vector<Val> mconst;
    for (size_t i = 0; i < nconst; i++) {
      consts.push_back(RandomValue(*leaf));
      mconst.push_back(Val::From(consts.back()));
    }
    if (path.size() > 1) {
      counts.nested_paths += 1;
    }
    std::string dotted;
    for (const DField * d : path) {
      dotted += (dotted.empty() ? "" : ".") + d->name;
    }
    eval = [path, op, mconst](const MEntity & e) {
      std::vector<Val> have;
      if (e.snaps.size() > 1) {
        Values(e.snaps.back(), path, 0, have);
      }
      bool some = false;
      for (const Val & a : have) {
        for (const Val & b : mconst) {
          int c = Cmp(a, b);
          bool r = op == GDEFSTOR_QUERY_LT   ? c < 0
                   : op == GDEFSTOR_QUERY_LE ? c <= 0
                   : op == GDEFSTOR_QUERY_GT ? c > 0
                   : op == GDEFSTOR_QUERY_GE ? c >= 0
                                             : c == 0;
          some = some || r;
        }
      }
      return op == GDEFSTOR_QUERY_NE ? !some : some;
    };
    return q.Cmp(op, q.Field(dotted), q.Values(consts));
  }

  void CheckQuery() {
    Q q;
    std::function<bool(const MEntity &)> eval = [](const MEntity &) {
      return true;
    };
    const GDEFSTOR_QueryNode * where =
        Chance(0.1) ? nullptr : RandomWhere(q, 0, eval);
    std::vector<std::vector<const DField *>> paths;
    std::vector<const DField *> at;
    sortable_.clear();
    Paths(decl_, at, paths, true);
    std::vector<GDEFSTOR_QuerySort> sorts;
    std::vector<std::pair<const DField *, bool>> keys; /* NULL: created */
    size_t nsort = Pick(3);
    for (size_t i = 0; i < nsort; i++) {
      bool desc = Chance(0.5);
      if (!sortable_.empty() && Chance(0.7)) {
        const DField * d = sortable_[Pick(sortable_.size())];
        sorts.push_back(By(q.Field(d->name), desc));
        keys.push_back(std::make_pair(d, desc));
      } else {
        sorts.push_back(By(q.Meta(GDEFSTOR_META_CREATED), desc));
        keys.push_back(std::make_pair(nullptr, desc));
      }
    }
    uint64_t offset = Pick(4);
    uint64_t limit = Pick(5);
    /* The model's answer. */
    std::vector<size_t> hits;
    for (size_t i = 0; i < entities_.size(); i++) {
      if (where == nullptr || eval(entities_[i])) {
        hits.push_back(i);
      }
    }
    auto key = [&](size_t e, size_t k, Val * out) {
      if (keys[k].first == nullptr) {
        out->type = GDEFSTOR_FIELD_DATETIME;
        out->i = entities_[e].created;
        return true;
      }
      const MEntity & m = entities_[e];
      if (m.snaps.size() < 2 || m.snaps.back().vals.count(keys[k].first->name) == 0) {
        return false;
      }
      *out = m.snaps.back().vals.at(keys[k].first->name);
      return true;
    };
    std::stable_sort(hits.begin(), hits.end(), [&](size_t a, size_t b) {
      for (size_t k = 0; k < keys.size(); k++) {
        Val x;
        Val y;
        bool hx = key(a, k, &x);
        bool hy = key(b, k, &y);
        int c = hx != hy ? (hx ? 1 : -1) : (hx ? Cmp(x, y) : 0);
        if (c != 0) {
          return keys[k].second ? c > 0 : c < 0;
        }
      }
      return a < b;
    });
    Answer want;
    want.total = hits.size();
    for (size_t i = static_cast<size_t>(offset); i < hits.size(); i++) {
      if (limit != 0 && want.ids.size() == limit) {
        break;
      }
      want.ids.push_back(entities_[hits[i]].pid);
    }
    Answer got = Ask(store_, type_.c_str(), where, sorts, offset, limit);
    EXPECT_EQ(want, got) << "seed " << seed_ << " query " << queries_;
    counts.queries += 1;
    counts.queries_with_rows += want.total > 0 ? 1 : 0;
    counts.queries_empty += want.total == 0 ? 1 : 0;
    if (where != nullptr) {
      uint64_t n = 0;
      EXPECT_EQ(GDEFSTOR_OK, gdefstor_entity_count(store_, kTenant, 2,
          type_.data(), type_.size(), kLang, kLangLen, where, &n));
      EXPECT_EQ(want.total, n) << "seed " << seed_;
    }
    queries_ += 1;
  }

  void Run(int ops) {
    decl_ = RandomGroup(0);
    ASSERT_TRUE(Declare()) << "seed " << seed_;
    Create();
    for (int op = 0; op < ops; op++) {
      if (::testing::Test::HasFatalFailure() ||
          ::testing::Test::HasNonfatalFailure()) {
        return;
      }
      size_t which = Pick(100);
      if (entities_.size() < 6 && which < 8) {
        Create();
        continue;
      }
      MEntity & e = entities_[Pick(entities_.size())];
      if (which < 50) {
        Edit(e, false);
      } else if (which < 58) {
        Edit(e, true);
      } else if (which < 70) {
        Revert(e);
      } else if (which < 76) {
        Stale(e);
      } else if (which < 82) {
        State(e);
      } else if (which < 87) {
        ChangeDeclaration();
      } else {
        CheckQuery();
        continue;
      }
      CheckHistory(e);
    }
    for (int i = 0; i < 6; i++) {
      CheckQuery();
    }
    /* Derived data rebuilds to what is live. */
    std::string current = t_->Dump(GDEFSTOR_MODEL_CURRENT);
    ASSERT_EQ(GDEFSTOR_OK, t_->InWrite([&](GDEFSTOR_Write * w) {
      return gdefstor_model_rebuild(w);
    }));
    EXPECT_EQ(current, t_->Dump(GDEFSTOR_MODEL_CURRENT)) << "seed " << seed_;
    for (int i = 0; i < 4; i++) {
      CheckQuery();
    }
    for (const MEntity & m : entities_) {
      CheckHistory(m);
    }
  }

 private:
  ModelTest * t_;
  GDEFSTOR_Store * store_;
  std::mt19937 rng_;
  uint32_t seed_;
  std::string type_ = "site.sweep";
  std::vector<DField> decl_;
  std::vector<MEntity> entities_;
  std::vector<const DField *> sortable_;
  int next_name_ = 0;
  int queries_ = 0;
  static const Node * now_root_;
};

const Node * Sweep::now_root_ = nullptr;

class ModelSweep : public ModelTest {};

TEST_F(ModelSweep, RandomHistoriesMatchAnIndependentModel) {
  SweepCounts all;
  for (uint32_t seed = 1; seed <= 12; seed++) {
    TearDown();
    SetUp();
    Sweep s(this, store, seed);
    s.Run(120);
    if (HasFailure()) {
      break;
    }
    all.revisions += s.counts.revisions;
    all.unchanged += s.counts.unchanged;
    all.reverts += s.counts.reverts;
    all.revert_unchanged += s.counts.revert_unchanged;
    all.drafts += s.counts.drafts;
    all.conflicts += s.counts.conflicts;
    all.declarations += s.counts.declarations;
    all.retired += s.counts.retired;
    all.queries += s.counts.queries;
    all.queries_with_rows += s.counts.queries_with_rows;
    all.queries_empty += s.counts.queries_empty;
    all.nested_paths += s.counts.nested_paths;
    all.moves += s.counts.moves;
  }
  std::printf("sweep: %d revisions, %d unchanged saves, %d reverts (%d "
              "unchanged), %d drafts, %d conflicts, %d declarations (%d "
              "retirements), %d moves, %d queries (%d with rows, %d empty, %d "
              "nested paths)\n",
      all.revisions, all.unchanged, all.reverts, all.revert_unchanged,
      all.drafts, all.conflicts, all.declarations, all.retired, all.moves,
      all.queries, all.queries_with_rows, all.queries_empty, all.nested_paths);
  /* Each kind of event happened, or the sweep proves nothing about it. */
  EXPECT_GE(all.revisions, 300);
  EXPECT_GE(all.unchanged, 20);
  EXPECT_GE(all.reverts, 40);
  EXPECT_GE(all.revert_unchanged, 3);
  EXPECT_GE(all.drafts, 20);
  EXPECT_GE(all.conflicts, 20);
  EXPECT_GE(all.declarations, 20);
  EXPECT_GE(all.retired, 5);
  EXPECT_GE(all.moves, 20);
  EXPECT_GE(all.queries, 100);
  EXPECT_GE(all.queries_with_rows, 30);
  EXPECT_GE(all.queries_empty, 10);
  EXPECT_GE(all.nested_paths, 20);
}

/* Records, counters and the outbox (story S6a), in this namespace. */
#include "system.inc"
#include "language.inc"
#include "snapshot.inc"

} /* namespace model_conformance */
