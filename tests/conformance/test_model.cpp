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
 * A scratch entity backend, written apart from the memory library's, so the
 * port runs the entity conformance suite (model.inc) without depending on
 * that library. It is deliberately plain: a transaction is a copy of the
 * tenant, and a query visits entities newest first, so an engine that forgot
 * the final tiebreak would answer in an order this backend does not give it.
 */

#include <gtest/gtest.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

struct OVal {
  int type = 0;
  int64_t integer = 0;
  double real = 0.0;
  std::string bytes;
  std::string subject;
};

struct Ver {
  uint32_t group = 0;
  uint64_t item = 0;
  uint64_t parent = 0;
  double order = 0.0;
  uint64_t from = 0;
  uint64_t to = 0;
  std::vector<OVal> vals;
};

struct Rev {
  uint64_t revision = 0;
  uint64_t commit_seq = 0;
  std::string who;
  int64_t when = 0;
  std::string why;
};

struct State {
  uint64_t seq = 0;
  int published = 0;
  int deleted = 0;
  std::string actor;
  int64_t when = 0;
};

struct Blind {
  uint64_t item = 0;
  uint64_t from = 0;
  uint32_t field = 0;
  std::string subject;
  std::string mac;
  uint32_t key_id = 0;
  uint32_t version = 0;
};

struct Ent {
  uint64_t row = 0;
  std::string type;
  std::string pid;
  std::string creator;
  int64_t created = 0;
  uint64_t revision = 0;
  uint64_t last_item = 0;
  std::vector<State> states;
  std::vector<Rev> revs;
  std::vector<Ver> versions; /* truth */
  std::vector<Blind> blinds; /* truth while the subject exists */
  /* derived */
  std::vector<Ver> current;
  int published = 0;
  int deleted = 0;
};

struct Draft {
  std::string type;
  uint64_t base = 0;
  std::string json;
};

struct Tenant {
  std::map<std::string, std::string> types;
  std::vector<Ent> ents;                 /* row - 1 */
  std::map<std::string, uint64_t> pids;  /* derived */
  std::map<std::string, Draft> drafts;
  uint64_t commit_seq = 0;
};

struct Txn {
  std::string tenant;
  Tenant copy;
  uint64_t seq = 0;
};

struct Scratch {
  const GDEFSTOR_Allocator * alloc = nullptr;
  std::map<std::string, Tenant> tenants;
  std::map<std::string, Txn *> open;
};

Tenant * view(Scratch * s, void * txn, const char * t, size_t tl) {
  if (txn != nullptr) {
    return &static_cast<Txn *>(txn)->copy;
  }
  auto it = s->tenants.find(std::string(t, tl));
  return it == s->tenants.end() ? nullptr : &it->second;
}

OVal own(const GDEFSTOR_Value & v) {
  OVal o;
  o.type = v.type;
  o.integer = v.integer;
  o.real = v.real;
  o.bytes.assign(reinterpret_cast<const char *>(v.bytes), v.len);
  o.subject.assign(reinterpret_cast<const char *>(v.subject), v.subject_len);
  return o;
}

std::vector<GDEFSTOR_Value> lend(const std::vector<OVal> & vals) {
  std::vector<GDEFSTOR_Value> out(vals.size());
  for (size_t i = 0; i < vals.size(); i++) {
    std::memset(&out[i], 0, sizeof out[i]);
    out[i].type = vals[i].type;
    out[i].integer = vals[i].integer;
    out[i].real = vals[i].real;
    out[i].bytes = reinterpret_cast<const uint8_t *>(vals[i].bytes.data());
    out[i].len = vals[i].bytes.size();
    out[i].subject = reinterpret_cast<const uint8_t *>(vals[i].subject.data());
    out[i].subject_len = vals[i].subject.size();
  }
  return out;
}

GDEFSTOR_ModelEntity lend(const Ent & e) {
  GDEFSTOR_ModelEntity m;
  std::memset(&m, 0, sizeof m);
  m.row = e.row;
  m.type = e.type.data();
  m.type_len = e.type.size();
  m.public_id = e.pid.data();
  m.public_id_len = e.pid.size();
  m.creator = e.creator.data();
  m.creator_len = e.creator.size();
  m.created_ms = e.created;
  m.revision = e.revision;
  m.last_item = e.last_item;
  m.published = e.published;
  m.deleted = e.deleted;
  return m;
}

GDEFSTOR_Result visit_item(const GDEFSTOR_ModelVisitor * v, uint64_t row,
    const Ver & x) {
  std::vector<GDEFSTOR_Value> vals = lend(x.vals);
  GDEFSTOR_ModelItem m;
  std::memset(&m, 0, sizeof m);
  m.group = x.group;
  m.item = x.item;
  m.parent = x.parent;
  m.order = x.order;
  m.valid_from = x.from;
  m.valid_to = x.to;
  m.values = vals.empty() ? nullptr : vals.data();
  m.value_count = vals.size();
  return v->item(v->ctx, row, &m);
}

GDEFSTOR_Result visit_blinds(const GDEFSTOR_ModelVisitor * v, const Ent & e) {
  if (v->blind == nullptr) {
    return GDEFSTOR_OK;
  }
  for (const Blind & b : e.blinds) {
    GDEFSTOR_ModelBlind m;
    std::memset(&m, 0, sizeof m);
    m.item = b.item;
    m.valid_from = b.from;
    m.field = b.field;
    m.subject = reinterpret_cast<const uint8_t *>(b.subject.data());
    m.subject_len = b.subject.size();
    m.mac = reinterpret_cast<const uint8_t *>(b.mac.data());
    m.key_id = b.key_id;
    m.version = b.version;
    GDEFSTOR_Result rc = v->blind(v->ctx, e.row, &m);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

/* The blind table is truth: derive() leaves it alone. */
void derive(Tenant & t) {
  t.pids.clear();
  for (Ent & e : t.ents) {
    t.pids[e.pid] = e.row;
    e.current.clear();
    for (const Ver & x : e.versions) {
      if (x.to == 0) {
        e.current.push_back(x);
      }
    }
    e.published = e.states.empty() ? 0 : e.states.back().published;
    e.deleted = e.states.empty() ? 0 : e.states.back().deleted;
  }
}

extern "C" {

static void s_destroy(void * ctx) {
  Scratch * s = static_cast<Scratch *>(ctx);
  for (auto & kv : s->open) {
    delete kv.second;
  }
  delete s;
}

static GDEFSTOR_Result s_begin(void * ctx, const char * t, size_t tl,
    void ** out) {
  Scratch * s = static_cast<Scratch *>(ctx);
  std::string name(t, tl);
  if (s->open.count(name) != 0) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  Txn * x = new Txn();
  x->tenant = name;
  x->copy = s->tenants[name];
  s->open[name] = x;
  *out = x;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_commit(void * ctx, void * txn) {
  Scratch * s = static_cast<Scratch *>(ctx);
  Txn * x = static_cast<Txn *>(txn);
  s->tenants[x->tenant] = x->copy;
  s->open.erase(x->tenant);
  delete x;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_abort(void * ctx, void * txn) {
  Scratch * s = static_cast<Scratch *>(ctx);
  Txn * x = static_cast<Txn *>(txn);
  s->open.erase(x->tenant);
  delete x;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_type_load(void * ctx, void * txn, const char * t,
    size_t tl, const char * type, size_t type_len, uint8_t ** out,
    size_t * out_len) {
  Scratch * s = static_cast<Scratch *>(ctx);
  Tenant * v = view(s, txn, t, tl);
  if (v == nullptr) {
    return GDEFSTOR_ERR_INVALID;
  }
  auto it = v->types.find(std::string(type, type_len));
  if (it == v->types.end()) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out = static_cast<uint8_t *>(gcu_allocator_malloc(s->alloc,
      it->second.size() + 1));
  if (*out == nullptr) {
    return GDEFSTOR_ERR_OOM;
  }
  std::memcpy(*out, it->second.data(), it->second.size());
  *out_len = it->second.size();
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_type_store(void * ctx, void * txn,
    const GDEFSTOR_Type * decl, const uint8_t * text, size_t len) {
  (void)ctx;
  GDEFSTOR_FieldInfo info;
  std::memset(&info, 0, sizeof info);
  info.size = sizeof info;
  /* The declaration's name is the first thing the text says after the format;
   * a backend would generate tables here. This one keeps the text. */
  std::string body(reinterpret_cast<const char *>(text), len);
  size_t at = body.find("\"name\":\"");
  size_t end = at == std::string::npos ? at : body.find('"', at + 8);
  if (at == std::string::npos || end == std::string::npos || decl == nullptr) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  static_cast<Txn *>(txn)->copy.types[body.substr(at + 8, end - at - 8)] = body;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_entity_find(void * ctx, void * txn, const char * t,
    size_t tl, const char * pid, size_t pid_len,
    const GDEFSTOR_ModelVisitor * v) {
  Tenant * ten = view(static_cast<Scratch *>(ctx), txn, t, tl);
  if (ten == nullptr) {
    return GDEFSTOR_ERR_INVALID;
  }
  auto it = ten->pids.find(std::string(pid, pid_len));
  if (it == ten->pids.end()) {
    return GDEFSTOR_ERR_INVALID;
  }
  GDEFSTOR_ModelEntity m = lend(ten->ents[it->second - 1]);
  return v->entity(v->ctx, &m);
}

static GDEFSTOR_Result s_entity_insert(void * ctx, void * txn,
    const GDEFSTOR_ModelEntity * e, uint64_t * out_row) {
  (void)ctx;
  Tenant & t = static_cast<Txn *>(txn)->copy;
  Ent n;
  n.row = t.ents.size() + 1;
  n.type.assign(e->type, e->type_len);
  n.pid.assign(e->public_id, e->public_id_len);
  n.creator.assign(e->creator, e->creator_len);
  n.created = e->created_ms;
  t.ents.push_back(n);
  t.pids[n.pid] = n.row;
  *out_row = n.row;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_state_append(void * ctx, void * txn, uint64_t row,
    const GDEFSTOR_ModelState * st) {
  (void)ctx;
  Tenant & t = static_cast<Txn *>(txn)->copy;
  if (row == 0 || row > t.ents.size()) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  Ent & e = t.ents[row - 1];
  State s;
  s.seq = e.states.size() + 1;
  s.published = st->published;
  s.deleted = st->deleted;
  s.actor.assign(st->actor, st->actor_len);
  s.when = st->when_ms;
  e.states.push_back(s);
  e.published = s.published;
  e.deleted = s.deleted;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_apply(void * ctx, void * txn,
    const GDEFSTOR_ModelApply * a) {
  (void)ctx;
  Txn * x = static_cast<Txn *>(txn);
  Tenant & t = x->copy;
  if (a->row == 0 || a->row > t.ents.size()) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  Ent e = t.ents[a->row - 1];
  uint64_t n = a->revision->revision;
  for (size_t i = 0; i < a->close_count; i++) {
    const GDEFSTOR_ModelClose & c = a->closes[i];
    bool done = false;
    for (Ver & v : e.versions) {
      if (v.group == c.group && v.item == c.item && v.from == c.valid_from) {
        if (v.to != 0) {
          return GDEFSTOR_ERR_INTERNAL; /* valid_to is written once */
        }
        v.to = n;
        done = true;
      }
    }
    if (!done) {
      return GDEFSTOR_ERR_INTERNAL;
    }
    for (size_t k = 0; k < e.current.size(); k++) {
      if (e.current[k].group == c.group && e.current[k].item == c.item) {
        e.current.erase(e.current.begin() + static_cast<long>(k));
        break;
      }
    }
  }
  for (size_t i = 0; i < a->insert_count; i++) {
    const GDEFSTOR_ModelItem & m = a->inserts[i];
    Ver v;
    v.group = m.group;
    v.item = m.item;
    v.parent = m.parent;
    v.order = m.order;
    v.from = n;
    v.to = 0;
    for (size_t k = 0; k < m.value_count; k++) {
      v.vals.push_back(own(m.values[k]));
    }
    e.versions.push_back(v);
    e.current.push_back(v);
  }
  for (size_t i = 0; i < a->blind_count; i++) {
    const GDEFSTOR_ModelBlind & m = a->blinds[i];
    bool indexed = false;
    /* A blind row indexes a version this same apply writes. */
    for (size_t k = 0; k < a->insert_count; k++) {
      indexed |= a->inserts[k].item == m.item;
    }
    if (!indexed || m.valid_from != n || m.mac == nullptr) {
      return GDEFSTOR_ERR_INTERNAL;
    }
    Blind b;
    b.item = m.item;
    b.from = m.valid_from;
    b.field = m.field;
    b.subject.assign(reinterpret_cast<const char *>(m.subject), m.subject_len);
    b.mac.assign(reinterpret_cast<const char *>(m.mac), GDEFSTOR_BLIND_MAC_BYTES);
    b.key_id = m.key_id;
    b.version = m.version;
    e.blinds.push_back(b);
  }
  if (x->seq == 0) {
    x->seq = t.commit_seq + 1;
    t.commit_seq = x->seq;
  }
  Rev r;
  r.revision = n;
  r.commit_seq = x->seq;
  r.who.assign(a->revision->who, a->revision->who_len);
  r.when = a->revision->when_ms;
  r.why.assign(a->revision->why, a->revision->why_len);
  e.revs.push_back(r);
  e.revision = n;
  e.last_item = a->last_item;
  t.ents[a->row - 1] = e;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_history(void * ctx, void * txn, const char * t,
    size_t tl, uint64_t row, const GDEFSTOR_ModelVisitor * v) {
  Tenant * ten = view(static_cast<Scratch *>(ctx), txn, t, tl);
  if (ten == nullptr || row == 0 || row > ten->ents.size()) {
    return GDEFSTOR_ERR_INVALID;
  }
  const Ent & e = ten->ents[row - 1];
  for (const Rev & r : e.revs) {
    GDEFSTOR_ModelRevision m;
    std::memset(&m, 0, sizeof m);
    m.revision = r.revision;
    m.commit_seq = r.commit_seq;
    m.who = r.who.data();
    m.who_len = r.who.size();
    m.when_ms = r.when;
    m.why = r.why.data();
    m.why_len = r.why.size();
    GDEFSTOR_Result rc = v->revision(v->ctx, row, &m);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (const State & s : e.states) {
    GDEFSTOR_ModelState m;
    std::memset(&m, 0, sizeof m);
    m.seq = s.seq;
    m.published = s.published;
    m.deleted = s.deleted;
    m.actor = s.actor.data();
    m.actor_len = s.actor.size();
    m.when_ms = s.when;
    GDEFSTOR_Result rc = v->state(v->ctx, row, &m);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_items(void * ctx, void * txn, const char * t,
    size_t tl, uint64_t row, uint64_t as_of, const GDEFSTOR_ModelVisitor * v) {
  Tenant * ten = view(static_cast<Scratch *>(ctx), txn, t, tl);
  if (ten == nullptr || row == 0 || row > ten->ents.size()) {
    return GDEFSTOR_ERR_INVALID;
  }
  const Ent & e = ten->ents[row - 1];
  const std::vector<Ver> & from = as_of == 0 ? e.current : e.versions;
  for (const Ver & x : from) {
    if (as_of != 0 && !(x.from <= as_of && (x.to == 0 || x.to > as_of))) {
      continue;
    }
    GDEFSTOR_Result rc = visit_item(v, row, x);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return visit_blinds(v, e);
}

static GDEFSTOR_Result s_draft_put(void * ctx, void * txn,
    const GDEFSTOR_ModelDraft * d) {
  (void)ctx;
  Draft x;
  x.type.assign(d->type, d->type_len);
  x.base = d->base_revision;
  x.json.assign(reinterpret_cast<const char *>(d->json), d->json_len);
  static_cast<Txn *>(txn)->copy.drafts[std::string(d->public_id,
      d->public_id_len)] = x;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_draft_get(void * ctx, void * txn, const char * t,
    size_t tl, const char * pid, size_t pid_len,
    const GDEFSTOR_ModelVisitor * v) {
  Tenant * ten = view(static_cast<Scratch *>(ctx), txn, t, tl);
  if (ten == nullptr) {
    return GDEFSTOR_ERR_INVALID;
  }
  auto it = ten->drafts.find(std::string(pid, pid_len));
  if (it == ten->drafts.end()) {
    return GDEFSTOR_ERR_INVALID;
  }
  GDEFSTOR_ModelDraft m;
  std::memset(&m, 0, sizeof m);
  m.public_id = it->first.data();
  m.public_id_len = it->first.size();
  m.type = it->second.type.data();
  m.type_len = it->second.type.size();
  m.base_revision = it->second.base;
  m.json = reinterpret_cast<const uint8_t *>(it->second.json.data());
  m.json_len = it->second.json.size();
  return v->draft(v->ctx, &m);
}

static GDEFSTOR_Result s_draft_delete(void * ctx, void * txn,
    const char * pid, size_t pid_len) {
  (void)ctx;
  Tenant & t = static_cast<Txn *>(txn)->copy;
  return t.drafts.erase(std::string(pid, pid_len)) == 1 ? GDEFSTOR_OK
                                                       : GDEFSTOR_ERR_INVALID;
}

struct ScanCtx {
  Tenant * tenant;
};

static GDEFSTOR_Result s_scan(void * ctx, const char * type, size_t type_len,
    const GDEFSTOR_ModelVisitor * v) {
  Tenant * t = static_cast<ScanCtx *>(ctx)->tenant;
  if (t == nullptr) {
    return GDEFSTOR_OK;
  }
  std::string want(type, type_len);
  /* Newest first: see the file comment. */
  for (size_t i = t->ents.size(); i > 0; i--) {
    const Ent & e = t->ents[i - 1];
    if (e.type != want) {
      continue;
    }
    GDEFSTOR_ModelEntity m = lend(e);
    GDEFSTOR_Result rc = v->entity(v->ctx, &m);
    for (size_t k = 0; rc == GDEFSTOR_OK && k < e.current.size(); k++) {
      rc = visit_item(v, e.row, e.current[k]);
    }
    if (rc == GDEFSTOR_OK) {
      rc = visit_blinds(v, e);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_query(void * ctx, const char * t, size_t tl,
    const GDEFSTOR_ModelPlan * plan, GDEFSTOR_ModelHits * out) {
  ScanCtx sc;
  sc.tenant = view(static_cast<Scratch *>(ctx), nullptr, t, tl);
  return gdefstor_model_scan(plan, s_scan, &sc, out);
}

static GDEFSTOR_Result s_rebuild(void * ctx, void * txn) {
  (void)ctx;
  derive(static_cast<Txn *>(txn)->copy);
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_dump(void * ctx, const char * t, size_t tl, int which,
    const GDEFSTOR_ModelVisitor * v) {
  Tenant * ten = view(static_cast<Scratch *>(ctx), nullptr, t, tl);
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (ten == nullptr) {
    return GDEFSTOR_OK;
  }
  if (which == GDEFSTOR_MODEL_TRUTH) {
    for (const auto & kv : ten->types) {
      rc = v->type(v->ctx, kv.first.data(), kv.first.size(),
          reinterpret_cast<const uint8_t *>(kv.second.data()), kv.second.size());
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    for (const auto & kv : ten->drafts) {
      GDEFSTOR_ModelDraft m;
      std::memset(&m, 0, sizeof m);
      m.public_id = kv.first.data();
      m.public_id_len = kv.first.size();
      m.type = kv.second.type.data();
      m.type_len = kv.second.type.size();
      m.base_revision = kv.second.base;
      m.json = reinterpret_cast<const uint8_t *>(kv.second.json.data());
      m.json_len = kv.second.json.size();
      rc = v->draft(v->ctx, &m);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  for (const Ent & e : ten->ents) {
    GDEFSTOR_ModelEntity m = lend(e);
    rc = v->entity(v->ctx, &m);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    if (which == GDEFSTOR_MODEL_TRUTH) {
      rc = s_history(ctx, nullptr, t, tl, e.row, v);
      for (size_t k = 0; rc == GDEFSTOR_OK && k < e.versions.size(); k++) {
        rc = visit_item(v, e.row, e.versions[k]);
      }
      if (rc == GDEFSTOR_OK) {
        rc = visit_blinds(v, e);
      }
    } else {
      for (size_t k = 0; rc == GDEFSTOR_OK && k < e.current.size(); k++) {
        rc = visit_item(v, e.row, e.current[k]);
      }
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_blind_history(void * ctx, const char * t, size_t tl,
    const GDEFSTOR_Type * type, size_t field, const uint8_t * mac,
    GDEFSTOR_ModelBlindFn fn, void * fn_ctx) {
  Tenant * ten = view(static_cast<Scratch *>(ctx), nullptr, t, tl);
  const char * name = nullptr;
  size_t name_len = 0;
  GDEFSTOR_FieldInfo f;
  std::memset(&f, 0, sizeof f);
  f.size = sizeof f;
  if (gdefstor_type_name(type, &name, &name_len) != GDEFSTOR_OK ||
      gdefstor_type_field(type, field, &f) != GDEFSTOR_OK) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (ten == nullptr) {
    return GDEFSTOR_OK;
  }
  std::string want(name, name_len);
  std::string m(reinterpret_cast<const char *>(mac), GDEFSTOR_BLIND_MAC_BYTES);
  for (const Ent & e : ten->ents) {
    if (e.type != want) {
      continue;
    }
    for (const Blind & b : e.blinds) {
      if (b.field != f.id || b.mac != m) {
        continue;
      }
      uint64_t to = UINT64_MAX;
      for (const Ver & x : e.versions) {
        if (x.item == b.item && x.from == b.from) {
          to = x.to;
        }
      }
      if (to == UINT64_MAX) {
        return GDEFSTOR_ERR_CORRUPT; /* a blind row with no version */
      }
      GDEFSTOR_ModelBlindHit h;
      std::memset(&h, 0, sizeof h);
      h.row = e.row;
      h.public_id = e.pid.data();
      h.public_id_len = e.pid.size();
      h.item = b.item;
      h.valid_from = b.from;
      h.valid_to = to;
      h.subject = reinterpret_cast<const uint8_t *>(b.subject.data());
      h.subject_len = b.subject.size();
      h.key_id = b.key_id;
      h.version = b.version;
      GDEFSTOR_Result rc = fn(fn_ctx, &h);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  return GDEFSTOR_OK;
}

/* Every version's subject columns: a personal value is the one kind that
 * carries a subject. */
static GDEFSTOR_Result s_subject_lookup(void * ctx, const char * t, size_t tl,
    const uint8_t * subject, size_t subject_len, GDEFSTOR_ModelSubjectFn fn,
    void * fn_ctx) {
  Tenant * ten = view(static_cast<Scratch *>(ctx), nullptr, t, tl);
  if (ten == nullptr) {
    return GDEFSTOR_OK;
  }
  std::string want(reinterpret_cast<const char *>(subject), subject_len);
  for (const Ent & e : ten->ents) {
    for (const Ver & x : e.versions) {
      for (size_t k = 0; k < x.vals.size(); k++) {
        if (x.vals[k].type == 0 || x.vals[k].subject != want) {
          continue;
        }
        GDEFSTOR_ModelSubjectHit h;
        std::memset(&h, 0, sizeof h);
        h.type = e.type.data();
        h.type_len = e.type.size();
        h.row = e.row;
        h.public_id = e.pid.data();
        h.public_id_len = e.pid.size();
        h.group = x.group;
        h.column = static_cast<uint32_t>(k);
        h.item = x.item;
        h.valid_from = x.from;
        h.valid_to = x.to;
        GDEFSTOR_Result rc = fn(fn_ctx, &h);
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
      }
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result s_blind_delete(void * ctx, void * txn,
    const uint8_t * subject, size_t subject_len) {
  (void)ctx;
  Tenant & t = static_cast<Txn *>(txn)->copy;
  std::string want(reinterpret_cast<const char *>(subject), subject_len);
  for (Ent & e : t.ents) {
    std::vector<Blind> kept;
    for (const Blind & b : e.blinds) {
      if (b.subject != want) {
        kept.push_back(b);
      }
    }
    e.blinds = kept;
  }
  return GDEFSTOR_OK;
}

} /* extern "C" */

} /* namespace */

extern "C" GDEFSTOR_Result ModelOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * alloc);
extern "C" void ModelClose(GDEFSTOR_Store * store);

/* The key/bytes half: the port's tests already cover it with their own
 * scratch backend; this store needs one only because the port opens over
 * both. A bag that holds nothing is enough. */
extern "C" {
static void kb_destroy(void * ctx) {
  (void)ctx;
}
static GDEFSTOR_Result kb_put(void * ctx, const char * t, size_t tl,
    const char * k, size_t kl, const uint8_t * b, size_t n) {
  (void)ctx; (void)t; (void)tl; (void)k; (void)kl; (void)b; (void)n;
  return GDEFSTOR_ERR_IO;
}
static GDEFSTOR_Result kb_get(void * ctx, const char * t, size_t tl,
    const char * k, size_t kl, uint8_t ** out, size_t * n) {
  (void)ctx; (void)t; (void)tl; (void)k; (void)kl; (void)out; (void)n;
  return GDEFSTOR_ERR_INVALID;
}
static GDEFSTOR_Result kb_remove(void * ctx, const char * t, size_t tl,
    const char * k, size_t kl) {
  (void)ctx; (void)t; (void)tl; (void)k; (void)kl;
  return GDEFSTOR_ERR_INVALID;
}
static GDEFSTOR_Result kb_list(void * ctx, const char * t, size_t tl,
    const char * p, size_t pl, char *** keys, size_t ** lens, size_t * n) {
  (void)ctx; (void)t; (void)tl; (void)p; (void)pl;
  *keys = nullptr;
  *lens = nullptr;
  *n = 0;
  return GDEFSTOR_OK;
}
}

GDEFSTOR_Result ModelOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * alloc) {
  GDEFSTOR_Backend kb;
  GDEFSTOR_ModelBackend mb;
  Scratch * s = new Scratch();
  s->alloc = alloc == nullptr ? gdefstor_allocator_default() : alloc;
  std::memset(&kb, 0, sizeof kb);
  kb.destroy = kb_destroy;
  kb.put = kb_put;
  kb.get = kb_get;
  kb.remove = kb_remove;
  kb.list = kb_list;
  std::memset(&mb, 0, sizeof mb);
  mb.size = sizeof mb;
  mb.ctx = s;
  mb.destroy = s_destroy;
  mb.begin = s_begin;
  mb.commit = s_commit;
  mb.abort = s_abort;
  mb.type_load = s_type_load;
  mb.type_store = s_type_store;
  mb.entity_find = s_entity_find;
  mb.entity_insert = s_entity_insert;
  mb.state_append = s_state_append;
  mb.apply = s_apply;
  mb.history = s_history;
  mb.items = s_items;
  mb.draft_put = s_draft_put;
  mb.draft_get = s_draft_get;
  mb.draft_delete = s_draft_delete;
  mb.query = s_query;
  mb.rebuild = s_rebuild;
  mb.dump = s_dump;
  mb.blind_ctx = s;
  mb.blind_history = s_blind_history;
  mb.subject_lookup = s_subject_lookup;
  mb.blind_delete = s_blind_delete;
  GDEFSTOR_Result rc = gdefstor_store_create_model(&kb, &mb, alloc, nullptr, out);
  if (rc != GDEFSTOR_OK) {
    delete s;
  }
  return rc;
}

void ModelClose(GDEFSTOR_Store * store) {
  gdefstor_store_destroy(store);
}

#include "model.inc"

/* A scratch key store over the cipher it is given, apart from the memory
 * library's, so the port runs keystore.inc on its own. */
namespace {

struct KSub {
  std::string key;
  bool erased = false;
};

struct KScratch {
  const GDEFSTOR_Allocator * alloc = nullptr;
  GDEFSTOR_KeyCipher cipher;
  std::map<std::string, KSub> subjects; /* tenant NUL subject */
  std::vector<std::pair<std::string, std::pair<std::string, int64_t>>> ledger;
};

std::string KName(const char * t, size_t tl, const char * s, size_t sl) {
  return std::string(t, tl) + std::string(1, '\0') + std::string(s, sl);
}

extern "C" {
static void ks_destroy(void * ctx) {
  delete static_cast<KScratch *>(ctx);
}
static GDEFSTOR_Result ks_create(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl) {
  KScratch * k = static_cast<KScratch *>(ctx);
  std::string name = KName(t, tl, s, sl);
  if (k->subjects.count(name) != 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  std::string key(k->cipher.key_bytes, '\0');
  GDEFSTOR_Result rc = k->cipher.random(k->cipher.ctx,
      reinterpret_cast<uint8_t *>(&key[0]), key.size());
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  k->subjects[name].key = key;
  return GDEFSTOR_OK;
}
static GDEFSTOR_Result ks_seal(void * ctx, int seal, const char * t, size_t tl,
    const char * s, size_t sl, const uint8_t * in, size_t n, int * erased,
    uint8_t ** out, size_t * out_len) {
  KScratch * k = static_cast<KScratch *>(ctx);
  auto it = k->subjects.find(KName(t, tl, s, sl));
  *erased = 0;
  if (it == k->subjects.end()) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (it->second.erased) {
    *erased = 1;
    return GDEFSTOR_OK;
  }
  const uint8_t * key = reinterpret_cast<const uint8_t *>(it->second.key.data());
  return (seal ? k->cipher.seal : k->cipher.open)(k->cipher.ctx, k->alloc, key,
      in, n, out, out_len);
}
static GDEFSTOR_Result ks_wrap(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, const uint8_t * in, size_t n, int * erased,
    uint8_t ** out, size_t * out_len) {
  return ks_seal(ctx, 1, t, tl, s, sl, in, n, erased, out, out_len);
}
static GDEFSTOR_Result ks_unwrap(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, const uint8_t * in, size_t n, int * erased,
    uint8_t ** out, size_t * out_len) {
  return ks_seal(ctx, 0, t, tl, s, sl, in, n, erased, out, out_len);
}
static GDEFSTOR_Result ks_erase(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, int64_t when) {
  KScratch * k = static_cast<KScratch *>(ctx);
  KSub & sub = k->subjects[KName(t, tl, s, sl)];
  if (sub.erased) {
    return GDEFSTOR_OK;
  }
  sub.key.assign(sub.key.size(), '\0');
  sub.key.clear();
  sub.erased = true;
  k->ledger.push_back(std::make_pair(std::string(t, tl),
      std::make_pair(std::string(s, sl), when)));
  return GDEFSTOR_OK;
}
static GDEFSTOR_Result ks_ledger(void * ctx, const char * t, size_t tl,
    GDEFSTOR_Result (*visit)(void *, const char *, size_t, int64_t),
    void * vctx) {
  KScratch * k = static_cast<KScratch *>(ctx);
  for (const auto & row : k->ledger) {
    if (row.first == std::string(t, tl)) {
      GDEFSTOR_Result rc = visit(vctx, row.second.first.data(),
          row.second.first.size(), row.second.second);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  return GDEFSTOR_OK;
}
}

} /* namespace */

GDEFSTOR_Result KeyStoreOpen(const GDEFSTOR_KeyCipher * cipher,
    GDEFSTOR_KeyStore ** out) {
  KScratch * k = new KScratch();
  k->alloc = gdefstor_allocator_default();
  k->cipher = *cipher;
  GDEFSTOR_KeyStoreBackend b;
  std::memset(&b, 0, sizeof b);
  b.size = sizeof b;
  b.ctx = k;
  b.destroy = ks_destroy;
  b.create = ks_create;
  b.wrap = ks_wrap;
  b.unwrap = ks_unwrap;
  b.erase = ks_erase;
  b.ledger = ks_ledger;
  GDEFSTOR_Result rc = gdefstor_keystore_create(&b, nullptr, out);
  if (rc != GDEFSTOR_OK) {
    delete k;
  }
  return rc;
}

#include "keystore.inc"

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
