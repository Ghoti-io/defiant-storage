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
 * The query form (query.h) against every backend. Included by suite.inc, so
 * the memory and SQLite backends run it as the port does.
 *
 * The answers are checked against a model written here, apart from the
 * library: a plain C++ evaluation over the rows the test itself wrote. A
 * generated sweep draws worlds of rows (absent columns, empty and non-integer
 * texts, many-valued JSON fields in two languages, an unpublished-style joined
 * row, bytes that are quotes, percent signs and NULs) and random predicate
 * trees, sorts and pages, and requires the order, the page and the total to be
 * the model's.
 */

#include <deque>
#include <map>
#include <set>

namespace qt {

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 2685821657736338717ull + 88172645463325252ull) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint64_t below(uint64_t n) { return n == 0 ? 0 : next() % n; }
  bool chance(unsigned percent) { return below(100) < percent; }
};

typedef std::map<std::string, std::vector<std::string>> Fields;
typedef std::map<std::string, Fields> Doc;

struct MRow {
  std::string entity;
  std::map<std::string, std::string> cols;
  bool has_doc = false;
  Doc doc;
};

struct World {
  std::vector<MRow> rows;
  // join type -> entity -> columns
  std::map<std::string, std::map<std::string, std::map<std::string, std::string>>> joins;
};

static const char * const kType2 = "qt.rows";
static const char * const kOther = "qt.other";
static const char * const kJoin = "qt.state";

static std::vector<std::string> Alphabet() {
  return {"", "a", "b", "ab", "abc", "7", "07", "10", "-3", "0", "-0",
      "123456789012345678", "1234567890123456789", "x'y", "%", "_", "a b",
      std::string("a\0b", 3), "\xc3\xa9", "\xf0\x9f\x98\x80", "\"q\"",
      "back\\slash", "tab\t", "9", "99", "100"};
}

static std::string JsonString(Rng & rng, const std::string & s) {
  std::string out = "\"";
  size_t i = 0;
  char buf[16];
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c < 0x20) {
      snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else if (c == 0xf0 && s.compare(i, 4, "\xf0\x9f\x98\x80") == 0) {
      if (rng.chance(50)) {
        out += "\\ud83d\\ude00";
      } else {
        out += "\xf0\x9f\x98\x80";
      }
      i += 4;
      continue;
    } else if (c < 0x80 && c != '/' && rng.chance(8)) {
      snprintf(buf, sizeof buf, "\\u%04X", c);
      out += buf;
    } else if (c == '/') {
      out += rng.chance(50) ? "\\/" : "/";
    } else {
      out += (char)c;
    }
    i += 1;
  }
  return out + "\"";
}

/* The document text, with other members mixed in that the path must skip. */
static std::string DocText(Rng & rng, const Doc & doc) {
  std::string out = "{";
  bool first = true;
  if (rng.chance(30)) {
    out += "\"meta\":{\"n\":1.5e3,\"ok\":true,\"x\":[1,null,{\"a\":[\"z\"]}],\"s\":\"t\"}";
    first = false;
  }
  for (const auto & lang : doc) {
    if (!first) {
      out += rng.chance(50) ? "," : " , ";
    }
    first = false;
    out += JsonString(rng, lang.first) + ":{";
    bool f2 = true;
    for (const auto & field : lang.second) {
      if (!f2) {
        out += ",";
      }
      f2 = false;
      out += JsonString(rng, field.first) + ":[";
      for (size_t k = 0; k < field.second.size(); k++) {
        if (k > 0) {
          out += rng.chance(50) ? "," : ", ";
        }
        out += JsonString(rng, field.second[k]);
      }
      out += "]";
    }
    out += "}";
  }
  return out + "}";
}

static const char * kLangs[] = {"en", "fr"};
static const char * kFields[] = {"title", "tags", "rank"};

static World MakeWorld(Rng & rng, size_t n) {
  World w;
  std::vector<std::string> alpha = Alphabet();
  for (size_t i = 0; i < n; i++) {
    MRow r;
    r.entity = rng.chance(10) ? "x" + std::to_string(i) : std::to_string(i + 1);
    if (rng.chance(70)) {
      r.cols["n"] = alpha[rng.below(alpha.size())];
    }
    if (rng.chance(70)) {
      r.cols["t"] = alpha[rng.below(alpha.size())];
    }
    if (rng.chance(90)) {
      r.has_doc = true;
      for (const char * lang : kLangs) {
        if (!rng.chance(70)) {
          continue;
        }
        for (const char * field : kFields) {
          if (!rng.chance(70)) {
            continue;
          }
          std::vector<std::string> vals;
          size_t nv = rng.below(4);
          for (size_t k = 0; k < nv; k++) {
            vals.push_back(alpha[rng.below(alpha.size())]);
          }
          r.doc[lang][field] = vals;
        }
      }
      r.cols["lang"] = DocText(rng, r.doc);
    }
    if (rng.chance(85)) {
      std::map<std::string, std::string> s;
      s["pub"] = rng.chance(60) ? "1" : "0";
      if (rng.chance(60)) {
        s["creator"] = alpha[rng.below(6)];
      }
      w.joins[kJoin][r.entity] = s;
    }
    w.rows.push_back(r);
  }
  return w;
}

static std::vector<GDEFSTOR_Column> Columns(const std::map<std::string, std::string> & cols) {
  std::vector<GDEFSTOR_Column> out;
  for (const auto & c : cols) {
    GDEFSTOR_Column col;
    memset(&col, 0, sizeof col);
    col.name = c.first.data();
    col.name_len = c.first.size();
    col.value = (const uint8_t *)c.second.data();
    col.value_len = c.second.size();
    out.push_back(col);
  }
  return out;
}

static GDEFSTOR_Result PutRow(GDEFSTOR_Write * write, const char * type,
    const std::string & entity, const std::map<std::string, std::string> & cols) {
  std::vector<GDEFSTOR_Column> c = Columns(cols);
  return gdefstor_projection_put(write, type, strlen(type), entity.data(),
      entity.size(), c.empty() ? NULL : c.data(), c.size());
}

static void Commit(GDEFSTOR_Store * store, const World & w) {
  GDEFSTOR_Write * write = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  for (const auto & r : w.rows) {
    ASSERT_EQ(GDEFSTOR_OK, PutRow(write, kType2, r.entity, r.cols));
  }
  for (const auto & jt : w.joins) {
    for (const auto & e : jt.second) {
      ASSERT_EQ(GDEFSTOR_OK, PutRow(write, jt.first.c_str(), e.first, e.second));
    }
  }
  /* Rows of another type, which no query of kType2 may return. */
  for (size_t i = 0; i < 5; i++) {
    std::map<std::string, std::string> c;
    c["n"] = "7";
    c["t"] = "a";
    ASSERT_EQ(GDEFSTOR_OK, PutRow(write, kOther, "o" + std::to_string(i), c));
  }
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
}

/* ---- the description of a query, and its two readings -------------------- */

struct QOp {
  int kind = GDEFSTOR_QUERY_CONST;
  int flags = 0;
  std::string col;
  std::string join;
  bool json = false;
  std::string k1;
  std::string k2;
  std::vector<std::string> texts;
};

struct QNode {
  int op = GDEFSTOR_QUERY_TRUE;
  QOp l;
  QOp r;
  std::vector<QNode> kids;
};

struct QSort {
  QOp key;
  bool numeric = false;
  bool desc = false;
};

struct QDesc {
  bool has_where = false;
  QNode where;
  std::vector<QSort> sorts;
  bool entity_numeric = false;
  uint64_t offset = 0;
  uint64_t limit = 0;
  std::vector<QOp> select;
};

/* Storage that keeps every pointer the library will be handed alive. */
struct Arena {
  std::deque<std::vector<GDEFSTOR_QueryText>> texts;
  std::deque<std::string> strings;
  std::deque<GDEFSTOR_QueryNode> nodes;
  std::deque<std::vector<const GDEFSTOR_QueryNode *>> kids;
  std::deque<std::vector<GDEFSTOR_QuerySort>> sorts;
  std::deque<std::vector<GDEFSTOR_QueryOperand>> selects;
};

static const char * Keep(Arena & a, const std::string & s) {
  a.strings.push_back(s);
  return a.strings.back().data();
}

static GDEFSTOR_QueryOperand ToOperand(Arena & a, const QOp & o) {
  GDEFSTOR_QueryOperand out;
  memset(&out, 0, sizeof out);
  out.kind = o.kind;
  out.flags = o.flags;
  if (o.kind == GDEFSTOR_QUERY_COLUMN) {
    out.column = Keep(a, o.col);
    out.column_len = o.col.size();
    if (!o.join.empty()) {
      out.join_type = Keep(a, o.join);
      out.join_type_len = o.join.size();
    }
    if (o.json) {
      out.path_depth = 2;
      out.path1 = Keep(a, o.k1);
      out.path1_len = o.k1.size();
      out.path2 = Keep(a, o.k2);
      out.path2_len = o.k2.size();
    }
  } else if (o.kind == GDEFSTOR_QUERY_CONST) {
    std::vector<GDEFSTOR_QueryText> t;
    for (const auto & s : o.texts) {
      GDEFSTOR_QueryText x;
      x.bytes = (const uint8_t *)Keep(a, s);
      x.len = s.size();
      t.push_back(x);
    }
    a.texts.push_back(t);
    out.texts = a.texts.back().data();
    out.text_count = a.texts.back().size();
  }
  return out;
}

static const GDEFSTOR_QueryNode * ToNode(Arena & a, const QNode & n) {
  GDEFSTOR_QueryNode node;
  memset(&node, 0, sizeof node);
  node.op = n.op;
  node.left = ToOperand(a, n.l);
  node.right = ToOperand(a, n.r);
  if (!n.kids.empty()) {
    std::vector<const GDEFSTOR_QueryNode *> k;
    for (const auto & c : n.kids) {
      k.push_back(ToNode(a, c));
    }
    a.kids.push_back(k);
    node.children = a.kids.back().data();
    node.child_count = a.kids.back().size();
  }
  a.nodes.push_back(node);
  return &a.nodes.back();
}

static GDEFSTOR_Query ToQuery(Arena & a, const QDesc & d) {
  GDEFSTOR_Query q;
  memset(&q, 0, sizeof q);
  q.size = sizeof q;
  if (d.has_where) {
    q.where = ToNode(a, d.where);
  }
  if (!d.sorts.empty()) {
    std::vector<GDEFSTOR_QuerySort> s;
    for (const auto & x : d.sorts) {
      GDEFSTOR_QuerySort k;
      memset(&k, 0, sizeof k);
      k.key = ToOperand(a, x.key);
      k.numeric = x.numeric;
      k.descending = x.desc;
      s.push_back(k);
    }
    a.sorts.push_back(s);
    q.sorts = a.sorts.back().data();
    q.sort_count = a.sorts.back().size();
  }
  q.entity_numeric = d.entity_numeric;
  q.offset = d.offset;
  q.limit = d.limit;
  if (!d.select.empty()) {
    std::vector<GDEFSTOR_QueryOperand> s;
    for (const auto & x : d.select) {
      s.push_back(ToOperand(a, x));
    }
    a.selects.push_back(s);
    q.select = a.selects.back().data();
    q.select_count = a.selects.back().size();
  }
  return q;
}

/* ---- the model ----------------------------------------------------------- */

typedef std::vector<std::string> TextSet;

static TextSet ModelValues(const World & w, const MRow & row, const QOp & o) {
  TextSet out;
  if (o.kind == GDEFSTOR_QUERY_CONST) {
    out = o.texts;
  } else if (o.kind == GDEFSTOR_QUERY_ENTITY) {
    out.push_back(row.entity);
  } else if (o.join.empty()) {
    if (o.json) {
      if (row.has_doc) {
        auto l = row.doc.find(o.k1);
        if (l != row.doc.end()) {
          auto f = l->second.find(o.k2);
          if (f != l->second.end()) {
            out = f->second;
          }
        }
      }
    } else {
      auto c = row.cols.find(o.col);
      if (c != row.cols.end()) {
        out.push_back(c->second);
      }
    }
  } else {
    auto t = w.joins.find(o.join);
    if (t != w.joins.end()) {
      auto e = t->second.find(row.entity);
      if (e != t->second.end()) {
        auto c = e->second.find(o.col);
        if (c != e->second.end()) {
          out.push_back(c->second);
        }
      }
    }
  }
  if (out.empty() && (o.flags & GDEFSTOR_QUERY_EMPTY_IF_NONE) != 0) {
    out.push_back("");
  }
  return out;
}

static bool ModelInt(const std::string & s, long long * v) {
  size_t i = 0;
  bool neg = false;
  long long x = 0;
  if (!s.empty() && s[0] == '-') {
    neg = true;
    i = 1;
  }
  if (s.size() - i < 1 || s.size() - i > 18) {
    return false;
  }
  for (; i < s.size(); i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
    x = x * 10 + (s[i] - '0');
  }
  *v = neg ? -x : x;
  return true;
}

static bool ModelHolds(const World & w, const MRow & row, const QNode & n) {
  switch (n.op) {
    case GDEFSTOR_QUERY_TRUE:
      return true;
    case GDEFSTOR_QUERY_FALSE:
      return false;
    case GDEFSTOR_QUERY_AND:
      for (const auto & c : n.kids) {
        if (!ModelHolds(w, row, c)) {
          return false;
        }
      }
      return true;
    case GDEFSTOR_QUERY_OR:
      for (const auto & c : n.kids) {
        if (ModelHolds(w, row, c)) {
          return true;
        }
      }
      return false;
    case GDEFSTOR_QUERY_NOT:
      return !ModelHolds(w, row, n.kids[0]);
    default:
      break;
  }
  TextSet l = ModelValues(w, row, n.l);
  TextSet r = ModelValues(w, row, n.r);
  bool eq = false;
  for (const auto & a : l) {
    for (const auto & b : r) {
      if (a == b) {
        eq = true;
      }
    }
  }
  if (n.op == GDEFSTOR_QUERY_EQ || n.op == GDEFSTOR_QUERY_IN) {
    return eq;
  }
  if (n.op == GDEFSTOR_QUERY_NE) {
    return !eq;
  }
  for (const auto & a : l) {
    long long x;
    if (!ModelInt(a, &x)) {
      continue;
    }
    for (const auto & b : r) {
      long long y;
      if (!ModelInt(b, &y)) {
        continue;
      }
      if ((n.op == GDEFSTOR_QUERY_LT && x < y) ||
          (n.op == GDEFSTOR_QUERY_LE && x <= y) ||
          (n.op == GDEFSTOR_QUERY_GT && x > y) ||
          (n.op == GDEFSTOR_QUERY_GE && x >= y)) {
        return true;
      }
    }
  }
  return false;
}

/* A key: (rank, value, text) with rank 0 for none. */
struct MKey {
  int rank = 0;
  long long v = 0;
  std::string s;
};

static MKey ModelKey(const World & w, const MRow & row, const QSort & s) {
  MKey k;
  TextSet t = ModelValues(w, row, s.key);
  if (t.empty()) {
    return k;
  }
  if (s.numeric) {
    long long x;
    if (ModelInt(t[0], &x)) {
      k.rank = 1;
      k.v = x;
    }
  } else {
    k.rank = 1;
    k.s = t[0];
  }
  return k;
}

static int CmpKey(const QSort & s, const MKey & a, const MKey & b) {
  int c;
  if (a.rank != b.rank) {
    c = a.rank < b.rank ? -1 : 1;
  } else if (a.rank == 0) {
    c = 0;
  } else if (s.numeric) {
    c = a.v < b.v ? -1 : (a.v > b.v ? 1 : 0);
  } else {
    c = a.s.compare(b.s);
    c = c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  return s.desc ? -c : c;
}

struct Expect {
  uint64_t total = 0;
  std::vector<size_t> page; // indices into World::rows
};

static Expect ModelRun(const World & w, const QDesc & d) {
  Expect e;
  std::vector<size_t> hits;
  for (size_t i = 0; i < w.rows.size(); i++) {
    if (!d.has_where || ModelHolds(w, w.rows[i], d.where)) {
      hits.push_back(i);
    }
  }
  e.total = hits.size();
  std::stable_sort(hits.begin(), hits.end(), [&](size_t x, size_t y) {
    for (const auto & s : d.sorts) {
      int c = CmpKey(s, ModelKey(w, w.rows[x], s), ModelKey(w, w.rows[y], s));
      if (c != 0) {
        return c < 0;
      }
    }
    const std::string & a = w.rows[x].entity;
    const std::string & b = w.rows[y].entity;
    if (d.entity_numeric) {
      long long p = 0, q = 0;
      bool pi = ModelInt(a, &p), qi = ModelInt(b, &q);
      if (pi != qi) {
        return !pi;
      }
      if (pi && p != q) {
        return p < q;
      }
    }
    return a < b;
  });
  uint64_t limit = d.limit == 0 ? UINT64_MAX : std::min<uint64_t>(d.limit, GDEFSTOR_QUERY_LIMIT_MAX);
  for (uint64_t i = d.offset; i < hits.size() && e.page.size() < limit; i++) {
    e.page.push_back(hits[(size_t)i]);
  }
  return e;
}

/* ---- running one and comparing ------------------------------------------- */

static std::string Show(const std::string & s) {
  std::string out;
  char buf[8];
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7f) {
      out += (char)c;
    } else {
      snprintf(buf, sizeof buf, "\\x%02x", c);
      out += buf;
    }
  }
  return out;
}

static std::string Describe(const QOp & o) {
  switch (o.kind) {
    case GDEFSTOR_QUERY_CONST: {
      std::string s = "{";
      for (const auto & t : o.texts) {
        s += "'" + Show(t) + "' ";
      }
      return s + "}";
    }
    case GDEFSTOR_QUERY_ENTITY:
      return "entity";
    default:
      return (o.join.empty() ? "" : o.join + ".") + o.col +
             (o.json ? "[" + o.k1 + "][" + o.k2 + "]" : "") +
             ((o.flags & GDEFSTOR_QUERY_EMPTY_IF_NONE) ? "?" : "");
  }
}

static std::string Describe(const QNode & n) {
  static const char * names[] = {"", "true", "false", "and", "or", "not", "=", "!=", "in", "<", "<=", ">", ">="};
  std::string s = "(" + std::string(names[n.op]);
  if (n.op >= GDEFSTOR_QUERY_EQ) {
    s += " " + Describe(n.l) + " " + Describe(n.r);
  }
  for (const auto & c : n.kids) {
    s += " " + Describe(c);
  }
  return s + ")";
}

static std::string Describe(const QDesc & d) {
  std::string s = d.has_where ? Describe(d.where) : "all";
  for (const auto & x : d.sorts) {
    s += " sort " + Describe(x.key) + (x.numeric ? " num" : "") + (x.desc ? " desc" : "");
  }
  s += " ent" + std::string(d.entity_numeric ? "num" : "") + " offset " +
       std::to_string(d.offset) + " limit " + std::to_string(d.limit);
  return s;
}

static std::string RowsOf(GDEFSTOR_QueryResult * r) {
  std::string out;
  for (size_t i = 0; i < gdefstor_query_result_rows(r); i++) {
    const char * e = NULL;
    size_t n = 0;
    gdefstor_query_result_entity(r, i, &e, &n);
    out += std::string(e, n) + ",";
  }
  return out;
}

/* Runs @p d, and requires the model's order, page and total. */
static void CheckQuery(GDEFSTOR_Store * store, const World & w, const QDesc & d,
    const std::string & what) {
  Arena a;
  GDEFSTOR_Query q = ToQuery(a, d);
  GDEFSTOR_QueryResult * r = NULL;
  Expect e = ModelRun(w, d);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant,
                             strlen(kTenant), kType2, strlen(kType2), &q, &r))
      << what << " " << Describe(d);
  std::string want;
  for (size_t i : e.page) {
    want += w.rows[i].entity + ",";
  }
  EXPECT_EQ(want, RowsOf(r)) << what << " " << Describe(d);
  EXPECT_EQ(e.total, gdefstor_query_result_total(r)) << what << " " << Describe(d);
  for (size_t i = 0; i < e.page.size() && i < gdefstor_query_result_rows(r); i++) {
    for (size_t k = 0; k < d.select.size(); k++) {
      TextSet t = ModelValues(w, w.rows[e.page[i]], d.select[k]);
      int present = 0;
      const uint8_t * b = NULL;
      size_t n = 0;
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_query_result_value(r, i, k, &present, &b, &n));
      EXPECT_EQ(!t.empty(), present != 0) << what << " select " << k;
      if (!t.empty() && present) {
        EXPECT_EQ(t[0], std::string((const char *)b, n)) << what << " select " << k;
      }
    }
  }
  gdefstor_query_result_free(r);
  uint64_t counted = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_count(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), q.where, &counted))
      << what;
  EXPECT_EQ(e.total, counted) << what << " count " << Describe(d);
}

/* ---- random queries ------------------------------------------------------ */

static QOp ColumnOp(const char * col, bool empty) {
  QOp o;
  o.kind = GDEFSTOR_QUERY_COLUMN;
  o.col = col;
  o.flags = empty ? GDEFSTOR_QUERY_EMPTY_IF_NONE : 0;
  return o;
}

static QOp JsonOp(const char * lang, const char * field, bool empty) {
  QOp o = ColumnOp("lang", empty);
  o.json = true;
  o.k1 = lang;
  o.k2 = field;
  return o;
}

static QOp ConstOp(const std::vector<std::string> & t) {
  QOp o;
  o.kind = GDEFSTOR_QUERY_CONST;
  o.texts = t;
  return o;
}

static QOp RandOperand(Rng & rng, bool allow_const, bool allow_join = true) {
  std::vector<std::string> alpha = Alphabet();
  bool empty = rng.chance(40);
  unsigned pick = (unsigned)rng.below(allow_const ? 8 : 7);
  if (!allow_join && (pick == 4 || pick == 5)) {
    pick = 0;
  }
  switch (pick) {
    case 0:
      return ColumnOp("n", empty);
    case 1:
      return ColumnOp("t", empty);
    case 2:
      return JsonOp(kLangs[rng.below(2)], kFields[rng.below(3)], empty);
    case 3: {
      QOp o;
      o.kind = GDEFSTOR_QUERY_ENTITY;
      return o;
    }
    case 4: {
      QOp o = ColumnOp("pub", false);
      o.join = kJoin;
      return o;
    }
    case 5: {
      QOp o = ColumnOp("creator", empty);
      o.join = kJoin;
      return o;
    }
    case 6:
      return ColumnOp("absent", empty);
    default: {
      std::vector<std::string> t;
      size_t n = rng.below(3);
      for (size_t i = 0; i < n; i++) {
        t.push_back(alpha[rng.below(alpha.size())]);
      }
      return ConstOp(t);
    }
  }
}

static QNode RandNode(Rng & rng, int depth) {
  QNode n;
  std::vector<std::string> alpha = Alphabet();
  unsigned pick = (unsigned)rng.below(depth > 0 ? 10 : 6);
  if (pick >= 6) {
    n.op = pick == 9 ? GDEFSTOR_QUERY_NOT : (pick == 8 ? GDEFSTOR_QUERY_OR : GDEFSTOR_QUERY_AND);
    size_t k = n.op == GDEFSTOR_QUERY_NOT ? 1 : rng.below(4);
    for (size_t i = 0; i < k; i++) {
      n.kids.push_back(RandNode(rng, depth - 1));
    }
    return n;
  }
  if (pick == 0 && rng.chance(20)) {
    n.op = rng.chance(50) ? GDEFSTOR_QUERY_TRUE : GDEFSTOR_QUERY_FALSE;
    return n;
  }
  static const int cmp[] = {GDEFSTOR_QUERY_EQ, GDEFSTOR_QUERY_NE, GDEFSTOR_QUERY_IN,
      GDEFSTOR_QUERY_LT, GDEFSTOR_QUERY_LE, GDEFSTOR_QUERY_GT, GDEFSTOR_QUERY_GE};
  n.op = cmp[rng.below(7)];
  n.l = RandOperand(rng, true);
  if (n.op == GDEFSTOR_QUERY_IN) {
    std::vector<std::string> t;
    size_t k = rng.below(4);
    for (size_t i = 0; i < k; i++) {
      t.push_back(alpha[rng.below(alpha.size())]);
    }
    n.r = ConstOp(t);
  } else if (rng.chance(60)) {
    n.r = ConstOp({alpha[rng.below(alpha.size())]});
  } else {
    n.r = RandOperand(rng, true);
  }
  return n;
}

static QDesc RandQuery(Rng & rng) {
  QDesc d;
  d.has_where = rng.chance(90);
  if (d.has_where) {
    d.where = RandNode(rng, 3);
  }
  size_t ns = rng.below(3);
  for (size_t i = 0; i < ns; i++) {
    QSort s;
    s.key = RandOperand(rng, false, false);
    s.numeric = rng.chance(40);
    s.desc = rng.chance(40);
    d.sorts.push_back(s);
  }
  d.entity_numeric = rng.chance(50);
  d.offset = rng.chance(60) ? 0 : rng.below(40);
  static const uint64_t limits[] = {0, 0, 1, 5, 20, 37, 1001};
  d.limit = limits[rng.below(7)];
  d.select.push_back(ColumnOp("n", false));
  d.select.push_back(JsonOp("en", "title", false));
  QOp e;
  e.kind = GDEFSTOR_QUERY_ENTITY;
  d.select.push_back(e);
  return d;
}

class Query : public StoreFix {};

TEST_F(Query, Generated) {
  size_t queries = 0;
  for (uint64_t seed = 1; seed <= 6; seed++) {
    Rng rng(seed);
    World w = MakeWorld(rng, 60 + (size_t)seed * 15);
    Commit(store, w);
    if (HasFatalFailure()) {
      return;
    }
    for (int i = 0; i < 90; i++) {
      QDesc d = RandQuery(rng);
      CheckQuery(store, w, d, "seed " + std::to_string(seed) + " #" + std::to_string(i));
      if (HasFatalFailure() || HasNonfatalFailure()) {
        return;
      }
      queries += 1;
    }
    /* Start the next world clean. */
    GDEFSTOR_Write * write = NULL;
    ASSERT_EQ(GDEFSTOR_OK,
        gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
    for (const auto & r : w.rows) {
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_delete(write, kType2, strlen(kType2),
                                 r.entity.data(), r.entity.size()));
    }
    for (const auto & jt : w.joins) {
      for (const auto & e : jt.second) {
        ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_delete(write, jt.first.c_str(),
                                   jt.first.size(), e.first.data(), e.first.size()));
      }
    }
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  }
  EXPECT_EQ(540u, queries);
}

/* ---- named behaviours ---------------------------------------------------- */

static QNode Cmp(int op, const QOp & l, const QOp & r) {
  QNode n;
  n.op = op;
  n.l = l;
  n.r = r;
  return n;
}

static World Simple(const std::vector<std::map<std::string, std::string>> & rows) {
  World w;
  for (size_t i = 0; i < rows.size(); i++) {
    MRow r;
    r.entity = std::to_string(i + 1);
    r.cols = rows[i];
    w.rows.push_back(r);
  }
  return w;
}

TEST_F(Query, NumericComparisonOnlyOnIntegers) {
  World w = Simple({{{"rank", "7"}}, {{"rank", "abc"}}, {}, {{"rank", "07"}},
      {{"rank", "10"}}, {{"rank", "-3"}}, {{"rank", "7.5"}},
      {{"rank", "1234567890123456789"}}});
  Commit(store, w);
  QDesc d;
  d.has_where = true;
  d.where = Cmp(GDEFSTOR_QUERY_LT, ColumnOp("rank", false), ConstOp({"10"}));
  CheckQuery(store, w, d, "lt");
  Arena a;
  GDEFSTOR_Query q = ToQuery(a, d);
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  /* "7", "07" and "-3" are below 10; "abc", the absent column, "7.5" and a
   * 19-digit text are not integers and are false, not errors. */
  EXPECT_EQ("1,4,6,", RowsOf(r));
  gdefstor_query_result_free(r);
  /* `not` of a false comparison is true, as for any false. */
  QDesc n;
  n.has_where = true;
  QNode inner = Cmp(GDEFSTOR_QUERY_LT, ColumnOp("rank", false), ConstOp({"10"}));
  n.where.op = GDEFSTOR_QUERY_NOT;
  n.where.kids.push_back(inner);
  CheckQuery(store, w, n, "not lt");
  for (int op : {GDEFSTOR_QUERY_LE, GDEFSTOR_QUERY_GT, GDEFSTOR_QUERY_GE}) {
    QDesc o;
    o.has_where = true;
    o.where = Cmp(op, ColumnOp("rank", false), ConstOp({"7"}));
    CheckQuery(store, w, o, "ordered");
  }
}

TEST_F(Query, InListMatchesEveryMember) {
  World w = Simple({{{"t", "a"}}, {{"t", "b"}}, {{"t", "c"}}, {{"t", "d"}}, {}});
  Commit(store, w);
  for (size_t last = 0; last < 4; last++) {
    std::vector<std::string> members = {"a", "b", "c", "d"};
    std::vector<std::string> list = {members[last]};
    QDesc d;
    d.has_where = true;
    d.where = Cmp(GDEFSTOR_QUERY_IN, ColumnOp("t", false), ConstOp(list));
    CheckQuery(store, w, d, "single member");
  }
  /* The last member of a longer list is the only hit. */
  QDesc d;
  d.has_where = true;
  d.where = Cmp(GDEFSTOR_QUERY_IN, ColumnOp("t", false), ConstOp({"x", "y", "z", "d"}));
  CheckQuery(store, w, d, "last of four");
  Arena a;
  GDEFSTOR_Query q = ToQuery(a, d);
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  EXPECT_EQ("4,", RowsOf(r));
  gdefstor_query_result_free(r);
  /* An empty list matches nothing. */
  QDesc e;
  e.has_where = true;
  e.where = Cmp(GDEFSTOR_QUERY_IN, ColumnOp("t", false), ConstOp({}));
  CheckQuery(store, w, e, "empty list");
}

TEST_F(Query, AndOrNotCombineEveryClause) {
  World w = Simple({{{"n", "1"}, {"t", "x"}}, {{"n", "1"}, {"t", "y"}},
      {{"n", "2"}, {"t", "x"}}, {{"n", "2"}, {"t", "y"}}});
  Commit(store, w);
  QNode n1 = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("n", false), ConstOp({"1"}));
  QNode tx = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("t", false), ConstOp({"x"}));
  QNode ty = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("t", false), ConstOp({"y"}));
  QNode n2 = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("n", false), ConstOp({"2"}));
  struct { int op; std::vector<QNode> kids; const char * want; } cases[] = {
      {GDEFSTOR_QUERY_AND, {n1, tx}, "1,"},
      {GDEFSTOR_QUERY_AND, {tx, n1}, "1,"},
      {GDEFSTOR_QUERY_AND, {n1, ty, n1}, "2,"},
      {GDEFSTOR_QUERY_AND, {n1, tx, n2}, ""},
      {GDEFSTOR_QUERY_OR, {n1, tx}, "1,2,3,"},
      {GDEFSTOR_QUERY_OR, {n2, ty}, "2,3,4,"},
      {GDEFSTOR_QUERY_OR, {n1, n2, tx}, "1,2,3,4,"},
      {GDEFSTOR_QUERY_AND, {}, "1,2,3,4,"},
      {GDEFSTOR_QUERY_OR, {}, ""},
  };
  for (auto & c : cases) {
    QDesc d;
    d.has_where = true;
    d.where.op = c.op;
    d.where.kids = c.kids;
    Arena a;
    GDEFSTOR_Query q = ToQuery(a, d);
    GDEFSTOR_QueryResult * r = NULL;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                               kType2, strlen(kType2), &q, &r));
    EXPECT_EQ(c.want, RowsOf(r)) << Describe(d);
    gdefstor_query_result_free(r);
    CheckQuery(store, w, d, "combine");
  }
  QDesc d;
  d.has_where = true;
  d.where.op = GDEFSTOR_QUERY_NOT;
  d.where.kids.push_back(n1);
  CheckQuery(store, w, d, "not");
}

TEST_F(Query, PagesAreSlicesOfOneOrder) {
  std::vector<std::map<std::string, std::string>> rows;
  for (int i = 0; i < 250; i++) {
    /* Few distinct keys, so almost every comparison is a tie. */
    rows.push_back({{"k", std::to_string(i % 7)}, {"all", "yes"}});
  }
  World w = Simple(rows);
  Commit(store, w);
  QDesc whole;
  whole.has_where = true;
  whole.where = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("all", false), ConstOp({"yes"}));
  QSort s;
  s.key = ColumnOp("k", false);
  whole.sorts.push_back(s);
  whole.entity_numeric = true;
  Arena a0;
  GDEFSTOR_Query q0 = ToQuery(a0, whole);
  GDEFSTOR_QueryResult * all = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q0, &all));
  ASSERT_EQ(250u, gdefstor_query_result_rows(all));
  EXPECT_EQ(250u, gdefstor_query_result_total(all));
  std::string joined = RowsOf(all);
  gdefstor_query_result_free(all);
  std::string paged;
  for (uint64_t off = 0; off < 260; off += 20) {
    QDesc d = whole;
    d.offset = off;
    d.limit = 20;
    Arena a;
    GDEFSTOR_Query q = ToQuery(a, d);
    GDEFSTOR_QueryResult * r = NULL;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                               kType2, strlen(kType2), &q, &r));
    EXPECT_EQ(250u, gdefstor_query_result_total(r)) << "offset " << off;
    EXPECT_EQ(off < 250 ? std::min<uint64_t>(20, 250 - off) : 0, gdefstor_query_result_rows(r));
    paged += RowsOf(r);
    gdefstor_query_result_free(r);
    CheckQuery(store, w, d, "page");
  }
  EXPECT_EQ(joined, paged);
  /* Entities of equal key come in the order of their numbers, 9 before 10. */
  EXPECT_NE(std::string::npos, joined.find(",9,"));
}

TEST_F(Query, TiebreakIsTheEntityArgument) {
  World w = Simple({{{"k", "a"}}, {{"k", "a"}}, {{"k", "a"}}, {{"k", "a"}},
      {{"k", "a"}}, {{"k", "a"}}, {{"k", "a"}}, {{"k", "a"}}, {{"k", "a"}},
      {{"k", "a"}}, {{"k", "a"}}});
  Commit(store, w);
  QDesc d;
  QSort s;
  s.key = ColumnOp("k", false);
  d.sorts.push_back(s);
  Arena a;
  GDEFSTOR_Query q = ToQuery(a, d);
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  /* By bytes: "10" and "11" come before "2". */
  EXPECT_EQ("1,10,11,2,3,4,5,6,7,8,9,", RowsOf(r));
  gdefstor_query_result_free(r);
  d.entity_numeric = true;
  Arena b;
  q = ToQuery(b, d);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  EXPECT_EQ("1,2,3,4,5,6,7,8,9,10,11,", RowsOf(r));
  gdefstor_query_result_free(r);
  /* A descending key leaves the entity order ascending. */
  d.sorts[0].desc = true;
  Arena c;
  q = ToQuery(c, d);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  EXPECT_EQ("1,2,3,4,5,6,7,8,9,10,11,", RowsOf(r));
  gdefstor_query_result_free(r);
  CheckQuery(store, w, d, "desc");
}

TEST_F(Query, SortKeysOrderNoneFirstAndNumbersByValue) {
  World w = Simple({{{"k", "10"}}, {{"k", "9"}}, {}, {{"k", "abc"}}, {{"k", "-5"}},
      {{"k", ""}}});
  Commit(store, w);
  QDesc d;
  QSort s;
  s.key = ColumnOp("k", false);
  s.numeric = true;
  d.sorts.push_back(s);
  d.entity_numeric = true;
  Arena a;
  GDEFSTOR_Query q = ToQuery(a, d);
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  /* Rows with no integer (absent, "abc", "") tie ahead of the integers. */
  EXPECT_EQ("3,4,6,5,2,1,", RowsOf(r));
  gdefstor_query_result_free(r);
  d.sorts[0].numeric = false;
  Arena b;
  q = ToQuery(b, d);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  /* As bytes: none, then "", "-5", "10", "9", "abc". */
  EXPECT_EQ("3,6,5,1,2,4,", RowsOf(r));
  gdefstor_query_result_free(r);
}

TEST_F(Query, ConstantsAreMatchedLiterally) {
  std::string big(4096, 'q');
  big[2000] = '\'';
  std::vector<std::string> evil = {"'", "a'b", "'; DROP TABLE defiant_record; --",
      "%", "_", "a%", "\\", "\"", std::string("x\0y", 3), std::string("\0", 1),
      big, "x' OR '1'='1", "?", ":name", "$1"};
  std::vector<std::map<std::string, std::string>> rows;
  for (const auto & e : evil) {
    rows.push_back({{"t", e}});
  }
  rows.push_back({{"t", "plain"}});
  rows.push_back({{"t", "ab"}});
  rows.push_back({{"t", "xy"}});
  rows.push_back({{"t", ""}});
  World w = Simple(rows);
  Commit(store, w);
  for (size_t i = 0; i < evil.size(); i++) {
    QDesc d;
    d.has_where = true;
    d.where = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("t", false), ConstOp({evil[i]}));
    Arena a;
    GDEFSTOR_Query q = ToQuery(a, d);
    GDEFSTOR_QueryResult * r = NULL;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                               kType2, strlen(kType2), &q, &r))
        << i;
    /* Exactly the one row, never a LIKE-style neighbour or a wildcard. */
    EXPECT_EQ(std::to_string(i + 1) + ",", RowsOf(r)) << Show(evil[i]).substr(0, 40);
    EXPECT_EQ(1u, gdefstor_query_result_total(r));
    gdefstor_query_result_free(r);
    QDesc in = d;
    in.where = Cmp(GDEFSTOR_QUERY_IN, ColumnOp("t", false), ConstOp({evil[i], "nope"}));
    CheckQuery(store, w, in, "in");
    QDesc ne = d;
    ne.where = Cmp(GDEFSTOR_QUERY_NE, ColumnOp("t", false), ConstOp({evil[i]}));
    CheckQuery(store, w, ne, "ne");
  }
  /* The table is still there and holds every row. */
  QDesc all;
  CheckQuery(store, w, all, "all");
}

TEST_F(Query, TypeNamesAreNotPatterns) {
  /* Two types whose names differ by characters a LIKE would treat as
   * wildcards, in one tenant. */
  GDEFSTOR_Write * write = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK, PutRow(write, "a_b", "1", {{"n", "1"}}));
  ASSERT_EQ(GDEFSTOR_OK, PutRow(write, "axb", "2", {{"n", "1"}}));
  ASSERT_EQ(GDEFSTOR_OK, PutRow(write, "a%", "3", {{"n", "1"}}));
  ASSERT_EQ(GDEFSTOR_OK, PutRow(write, "a_b.sub", "4", {{"n", "1"}}));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  GDEFSTOR_Query q;
  memset(&q, 0, sizeof q);
  q.size = sizeof q;
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             "a_b", 3, &q, &r));
  EXPECT_EQ("1,", RowsOf(r));
  gdefstor_query_result_free(r);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             "a%", 2, &q, &r));
  EXPECT_EQ("3,", RowsOf(r));
  gdefstor_query_result_free(r);
}

TEST_F(Query, CountIsOnlyWhatMatches) {
  std::vector<std::map<std::string, std::string>> rows;
  for (int i = 0; i < 10; i++) {
    rows.push_back({{"see", i < 3 ? "yes" : "no"}});
  }
  World w = Simple(rows);
  Commit(store, w);
  QNode where = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("see", false), ConstOp({"yes"}));
  Arena a;
  const GDEFSTOR_QueryNode * node = ToNode(a, where);
  uint64_t n = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_count(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), node, &n));
  EXPECT_EQ(3u, n);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_count(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), NULL, &n));
  EXPECT_EQ(10u, n);
  /* The same total comes with a page of one. */
  QDesc d;
  d.has_where = true;
  d.where = where;
  d.limit = 1;
  d.offset = 1;
  Arena b;
  GDEFSTOR_Query q = ToQuery(b, d);
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  EXPECT_EQ(1u, gdefstor_query_result_rows(r));
  EXPECT_EQ(3u, gdefstor_query_result_total(r));
  gdefstor_query_result_free(r);
}

TEST_F(Query, LimitIsClamped) {
  /* One more row than the cap. */
  GDEFSTOR_Write * write = NULL;
  const size_t rows = GDEFSTOR_QUERY_LIMIT_MAX + 1;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  for (size_t i = 0; i < rows; i++) {
    ASSERT_EQ(GDEFSTOR_OK, PutRow(write, kType2, std::to_string(i + 1), {}));
  }
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  GDEFSTOR_Query q;
  memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.limit = rows + 500;
  q.entity_numeric = 1;
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  EXPECT_EQ((size_t)GDEFSTOR_QUERY_LIMIT_MAX, gdefstor_query_result_rows(r));
  EXPECT_EQ(rows, gdefstor_query_result_total(r));
  gdefstor_query_result_free(r);
  /* A limit of 0 asks for every match. */
  q.limit = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  EXPECT_EQ(rows, gdefstor_query_result_rows(r));
  gdefstor_query_result_free(r);
}

TEST_F(Query, JoinedRowIsTheSameEntityOfAnotherType) {
  World w;
  for (int i = 1; i <= 6; i++) {
    MRow r;
    r.entity = std::to_string(i);
    r.cols["n"] = "x";
    w.rows.push_back(r);
  }
  w.joins[kJoin]["1"] = {{"pub", "1"}, {"creator", "alice"}};
  w.joins[kJoin]["2"] = {{"pub", "0"}, {"creator", "alice"}};
  w.joins[kJoin]["3"] = {{"pub", "1"}};
  w.joins[kJoin]["4"] = {{"pub", "1"}, {"creator", "bob"}};
  /* 5 and 6 have no joined row at all. */
  Commit(store, w);
  QOp pub = ColumnOp("pub", false);
  pub.join = kJoin;
  QOp creator = ColumnOp("creator", false);
  creator.join = kJoin;
  QDesc d;
  d.has_where = true;
  d.where.op = GDEFSTOR_QUERY_AND;
  d.where.kids.push_back(Cmp(GDEFSTOR_QUERY_EQ, pub, ConstOp({"1"})));
  d.where.kids.push_back(Cmp(GDEFSTOR_QUERY_EQ, creator, ConstOp({"alice"})));
  CheckQuery(store, w, d, "join");
  Arena a;
  GDEFSTOR_Query q = ToQuery(a, d);
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  EXPECT_EQ("1,", RowsOf(r));
  gdefstor_query_result_free(r);
  /* Absent creator is no text, so neither = nor != to the empty text...  */
  QDesc e;
  e.has_where = true;
  e.where = Cmp(GDEFSTOR_QUERY_EQ, creator, ConstOp({""}));
  CheckQuery(store, w, e, "none is not empty");
  QDesc f;
  f.has_where = true;
  f.where = Cmp(GDEFSTOR_QUERY_NE, creator, ConstOp({"alice"}));
  CheckQuery(store, w, f, "ne with none");
  /* ...but the same column read with the empty-if-none flag is the empty text. */
  creator.flags = GDEFSTOR_QUERY_EMPTY_IF_NONE;
  QDesc g;
  g.has_where = true;
  g.where = Cmp(GDEFSTOR_QUERY_EQ, creator, ConstOp({""}));
  CheckQuery(store, w, g, "empty if none");
}

TEST_F(Query, JsonPathReadsEscapesAndSkipsTheRest) {
  GDEFSTOR_Write * write = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  /* \u escapes, a surrogate pair, a slash escape, other members of every JSON
   * kind before and after the one asked for, and a repeated key. */
  std::string doc =
      "{\"z\":[1,2,{\"en\":{\"title\":[\"wrong\"]}}],\"en\":{\"x\":null,"
      "\"title\":[\"caf\\u00e9\",\"\\ud83d\\ude00\",\"a\\/b\",\"q\\\"\\\\\","
      "\"\\u0000\"],\"n\":-1.5E+3,\"o\":{\"a\":[]}},\"fr\":{},"
      "\"en\":{\"title\":[\"second\"]}}";
  ASSERT_EQ(GDEFSTOR_OK, PutRow(write, kType2, "1", {{"lang", doc}}));
  ASSERT_EQ(GDEFSTOR_OK, PutRow(write, kType2, "2", {{"lang", "{}"}}));
  ASSERT_EQ(GDEFSTOR_OK, PutRow(write, kType2, "3", {{"lang", "{\"en\":{}}"}}));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  GDEFSTOR_QueryOperand sel;
  memset(&sel, 0, sizeof sel);
  sel.kind = GDEFSTOR_QUERY_COLUMN;
  sel.column = "lang";
  sel.column_len = 4;
  sel.path_depth = 2;
  sel.path1 = "en";
  sel.path1_len = 2;
  sel.path2 = "title";
  sel.path2_len = 5;
  GDEFSTOR_Query q;
  memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.select = &sel;
  q.select_count = 1;
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  ASSERT_EQ(3u, gdefstor_query_result_rows(r));
  int present = 0;
  const uint8_t * b = NULL;
  size_t n = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_query_result_value(r, 0, 0, &present, &b, &n));
  EXPECT_TRUE(present);
  EXPECT_EQ("caf\xc3\xa9", std::string((const char *)b, n));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_query_result_value(r, 1, 0, &present, &b, &n));
  EXPECT_FALSE(present);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_query_result_value(r, 2, 0, &present, &b, &n));
  EXPECT_FALSE(present);
  gdefstor_query_result_free(r);
  /* Any value of the array can be matched, including the escaped ones. */
  for (const char * want : {"\xf0\x9f\x98\x80", "a/b", "q\"\\"}) {
    QOp o = JsonOp("en", "title", false);
    QNode node = Cmp(GDEFSTOR_QUERY_EQ, o, ConstOp({want}));
    Arena a;
    const GDEFSTOR_QueryNode * w = ToNode(a, node);
    uint64_t count = 0;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_count(store, kTenant, strlen(kTenant),
                               kType2, strlen(kType2), w, &count));
    EXPECT_EQ(1u, count) << want;
  }
  /* The first of a repeated key is the one read. */
  {
    QNode second = Cmp(GDEFSTOR_QUERY_EQ, JsonOp("en", "title", false), ConstOp({"second"}));
    Arena s;
    uint64_t found = 7;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_count(store, kTenant, strlen(kTenant),
                               kType2, strlen(kType2), ToNode(s, second), &found));
    EXPECT_EQ(0u, found);
  }
  QNode nul = Cmp(GDEFSTOR_QUERY_EQ, JsonOp("en", "title", false),
      ConstOp({std::string("\0", 1)}));
  Arena a;
  uint64_t count = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_count(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), ToNode(a, nul), &count));
  EXPECT_EQ(1u, count);
}

TEST_F(Query, DamagedDocumentIsCorruptNeverAGuess) {
  const char * bad[] = {"not json", "{\"en\":{\"title\":[\"a\"", "{\"en\":{\"title\":[1]}}",
      "{\"en\":{\"title\":\"s\"}}", "{\"en\":[]}", "{\"en\":{\"title\":[\"\\ud800\"]}}",
      "{\"en\":{\"title\":[\"a\"]}} trailing", "{\"en\":{\"title\":[\"\\q\"]}}", ""};
  for (const char * doc : bad) {
    GDEFSTOR_Write * write = NULL;
    ASSERT_EQ(GDEFSTOR_OK,
        gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
    ASSERT_EQ(GDEFSTOR_OK, PutRow(write, kType2, "1", {{"lang", doc}}));
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
    QNode node = Cmp(GDEFSTOR_QUERY_EQ, JsonOp("en", "title", false), ConstOp({"a"}));
    Arena a;
    const GDEFSTOR_QueryNode * w = ToNode(a, node);
    uint64_t count = 7;
    EXPECT_EQ(GDEFSTOR_ERR_CORRUPT,
        gdefstor_projection_count(store, kTenant, strlen(kTenant), kType2,
            strlen(kType2), w, &count))
        << doc;
    GDEFSTOR_Query q;
    memset(&q, 0, sizeof q);
    q.size = sizeof q;
    q.where = w;
    GDEFSTOR_QueryResult * r = NULL;
    EXPECT_EQ(GDEFSTOR_ERR_CORRUPT,
        gdefstor_projection_query(store, kTenant, strlen(kTenant), kType2,
            strlen(kType2), &q, &r))
        << doc;
    EXPECT_TRUE(r == NULL);
  }
}

TEST_F(Query, AnUnknownTenantOrTypeHasNoRows) {
  World w = Simple({{{"n", "1"}}});
  Commit(store, w);
  GDEFSTOR_Query q;
  memset(&q, 0, sizeof q);
  q.size = sizeof q;
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, "nobody", 6, kType2,
                             strlen(kType2), &q, &r));
  EXPECT_EQ(0u, gdefstor_query_result_rows(r));
  EXPECT_EQ(0u, gdefstor_query_result_total(r));
  gdefstor_query_result_free(r);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             "no.such.type", 12, &q, &r));
  EXPECT_EQ(0u, gdefstor_query_result_rows(r));
  gdefstor_query_result_free(r);
  uint64_t n = 5;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_count(store, "nobody", 6, kType2,
                             strlen(kType2), NULL, &n));
  EXPECT_EQ(0u, n);
}

TEST_F(Query, SeesOnlyWhatIsCommitted) {
  World w = Simple({{{"n", "1"}}});
  Commit(store, w);
  GDEFSTOR_Write * write = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK, PutRow(write, kType2, "2", {{"n", "1"}}));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_delete(write, kType2, strlen(kType2), "1", 1));
  uint64_t n = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_count(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), NULL, &n));
  EXPECT_EQ(1u, n);
  GDEFSTOR_Query q;
  memset(&q, 0, sizeof q);
  q.size = sizeof q;
  GDEFSTOR_QueryResult * r = NULL;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  EXPECT_EQ("1,", RowsOf(r));
  gdefstor_query_result_free(r);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_query(store, kTenant, strlen(kTenant),
                             kType2, strlen(kType2), &q, &r));
  EXPECT_EQ("2,", RowsOf(r));
  gdefstor_query_result_free(r);
}

static QNode Chain(int depth) {
  QNode n;
  if (depth <= 1) {
    n.op = GDEFSTOR_QUERY_TRUE;
    return n;
  }
  n.op = GDEFSTOR_QUERY_NOT;
  n.kids.push_back(Chain(depth - 1));
  return n;
}

TEST_F(Query, LimitsAndShapesAreRefused) {
  World w = Simple({{{"n", "1"}}});
  Commit(store, w);
  auto run = [&](const QDesc & d) {
    Arena a;
    GDEFSTOR_Query q = ToQuery(a, d);
    GDEFSTOR_QueryResult * r = NULL;
    GDEFSTOR_Result rc = gdefstor_projection_query(store, kTenant, strlen(kTenant),
        kType2, strlen(kType2), &q, &r);
    EXPECT_EQ(rc == GDEFSTOR_OK, r != NULL);
    gdefstor_query_result_free(r);
    return rc;
  };
  QDesc d;
  d.has_where = true;
  d.where = Chain(GDEFSTOR_QUERY_MAX_DEPTH);
  EXPECT_EQ(GDEFSTOR_OK, run(d));
  d.where = Chain(GDEFSTOR_QUERY_MAX_DEPTH + 1);
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, run(d));
  /* Too many nodes, wide and shallow. */
  QDesc wide;
  wide.has_where = true;
  wide.where.op = GDEFSTOR_QUERY_OR;
  for (int i = 0; i < GDEFSTOR_QUERY_MAX_NODES; i++) {
    QNode t;
    t.op = GDEFSTOR_QUERY_FALSE;
    wide.where.kids.push_back(t);
  }
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, run(wide));
  wide.where.kids.pop_back();
  EXPECT_EQ(GDEFSTOR_OK, run(wide));
  QDesc texts;
  texts.has_where = true;
  std::vector<std::string> many(GDEFSTOR_QUERY_MAX_TEXTS, "a");
  texts.where = Cmp(GDEFSTOR_QUERY_IN, ColumnOp("n", false), ConstOp(many));
  EXPECT_EQ(GDEFSTOR_OK, run(texts));
  many.push_back("a");
  texts.where = Cmp(GDEFSTOR_QUERY_IN, ColumnOp("n", false), ConstOp(many));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, run(texts));
  QDesc bytes;
  bytes.has_where = true;
  bytes.where = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("n", false),
      ConstOp({std::string(GDEFSTOR_QUERY_MAX_TEXT_BYTES, 'a')}));
  EXPECT_EQ(GDEFSTOR_OK, run(bytes));
  bytes.where = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("n", false),
      ConstOp({std::string(GDEFSTOR_QUERY_MAX_TEXT_BYTES + 1, 'a')}));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, run(bytes));
  QDesc sorts;
  for (int i = 0; i < GDEFSTOR_QUERY_MAX_SORTS + 1; i++) {
    QSort s;
    s.key = ColumnOp("n", false);
    sorts.sorts.push_back(s);
  }
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, run(sorts));
  sorts.sorts.pop_back();
  EXPECT_EQ(GDEFSTOR_OK, run(sorts));
  QDesc sel;
  for (int i = 0; i < GDEFSTOR_QUERY_MAX_SELECT + 1; i++) {
    sel.select.push_back(ColumnOp("n", false));
  }
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, run(sel));
  /* Shapes that make no sense. */
  QDesc bad;
  bad.has_where = true;
  bad.where = Cmp(GDEFSTOR_QUERY_IN, ColumnOp("n", false), ColumnOp("n", false));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, run(bad));
  bad.where.op = 99;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, run(bad));
  bad.where.op = GDEFSTOR_QUERY_NOT;
  bad.where.kids.clear();
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, run(bad));
  bad.where.kids = {Chain(1), Chain(1)};
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, run(bad));
  QDesc ncol;
  ncol.has_where = true;
  ncol.where = Cmp(GDEFSTOR_QUERY_EQ, ColumnOp("", false), ConstOp({"a"}));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, run(ncol));
  QDesc sconst;
  QSort cs;
  cs.key = ConstOp({"a"});
  sconst.sorts.push_back(cs);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, run(sconst));
  QDesc selj;
  QOp j = ColumnOp("pub", false);
  j.join = kJoin;
  selj.select.push_back(j);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, run(selj));
  /* The structure itself. */
  GDEFSTOR_QueryResult * r = NULL;
  GDEFSTOR_Query q;
  memset(&q, 0, sizeof q);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_projection_query(store, kTenant,
                                      strlen(kTenant), kType2, strlen(kType2), &q, &r));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_projection_query(store, kTenant,
                                      strlen(kTenant), kType2, strlen(kType2), NULL, &r));
  q.size = sizeof q;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_projection_query(store, "", 0, kType2,
                                      strlen(kType2), &q, &r));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_projection_query(store, kTenant,
                                      strlen(kTenant), "", 0, &q, &r));
  q.sort_count = 1;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_projection_query(store, kTenant,
                                      strlen(kTenant), kType2, strlen(kType2), &q, &r));
}

TEST_F(Query, ANodeSharedByManyParentsIsCountedEachTime) {
  /* A tree built as a DAG could be exponentially large; the walk counts the
   * visits, so the cap holds. */
  World w = Simple({{{"n", "1"}}});
  Commit(store, w);
  GDEFSTOR_QueryNode leaf;
  memset(&leaf, 0, sizeof leaf);
  leaf.op = GDEFSTOR_QUERY_TRUE;
  std::deque<std::vector<const GDEFSTOR_QueryNode *>> kids;
  std::deque<GDEFSTOR_QueryNode> nodes;
  const GDEFSTOR_QueryNode * below = &leaf;
  for (int level = 0; level < 40; level++) {
    kids.push_back({below, below});
    GDEFSTOR_QueryNode n;
    memset(&n, 0, sizeof n);
    n.op = GDEFSTOR_QUERY_AND;
    n.children = kids.back().data();
    n.child_count = 2;
    nodes.push_back(n);
    below = &nodes.back();
  }
  uint64_t count = 0;
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, gdefstor_projection_count(store, kTenant,
                                    strlen(kTenant), kType2, strlen(kType2), below, &count));
}

} /* namespace qt */
