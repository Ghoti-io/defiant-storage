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
 * Conformance answers. Included by the port's scratch driver and, after
 * install, by the memory backend. The including file defines
 * ConformanceOpen and ConformanceClose.
 */

#include <ghoti.io/archive/reader.h>
#include <ghoti.io/archive/sink.h>
#include <ghoti.io/archive/stream.h>
#include <ghoti.io/archive/writer.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
GDEFSTOR_Result ConformanceOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * allocator);
void ConformanceClose(GDEFSTOR_Store * store);
}

static const char kTenant[] = "tenant";
static const char kEntity[] = "post1";
static const char kType[] = "core.post";
static const char kDoc[] =
    "{\"format\":\"defiant-revision/1\",\"seq\":1,\"entity\":\"post1\","
    "\"type\":\"core.post\",\"lang\":{\"en\":{\"title\":[\"Hello\"]}},"
    "\"body\":\"\"}";
static const char kHash[] = "abc123";
static const uint8_t kBlob[] = {'b', 'o', 'd', 'y'};
static const char kOutId[] = "evt1";
static const char kStream[] = "posts";
static const char kPayload[] = "{\"n\":1}";

class StoreFix : public ::testing::Test {
 protected:
  GDEFSTOR_Store * store = NULL;
  void SetUp() override {
    ASSERT_EQ(GDEFSTOR_OK, ConformanceOpen(&store, NULL));
  }
  void TearDown() override { ConformanceClose(store); }
};

class ReadSnapshot : public StoreFix {};
class Abort : public StoreFix {};
class Revision : public StoreFix {};
class Limits : public StoreFix {};
class Writers : public StoreFix {};
class Formats : public StoreFix {};
class Snapshot : public StoreFix {};
class Outbox : public StoreFix {};
class Blob : public StoreFix {};

static GDEFSTOR_Result commit_doc(GDEFSTOR_Store * store, GDEFSTOR_Write ** out) {
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Result rc =
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_revision_append(write, (const uint8_t *)kDoc, strlen(kDoc));
  if (rc != GDEFSTOR_OK) {
    gdefstor_write_abort(write);
    return rc;
  }
  if (out != NULL) {
    *out = write;
    return GDEFSTOR_OK;
  }
  return gdefstor_write_commit(write);
}

TEST_F(ReadSnapshot, OpenWriteIsInvisible) {
  GDEFSTOR_Write * write = NULL;
  uint8_t * bytes = NULL;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK, commit_doc(store, &write));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &bytes, &len));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &bytes, &len));
  gdefstor_bytes_free(NULL, bytes);
}

static GDEFSTOR_Result stage_all(GDEFSTOR_Write * write) {
  GDEFSTOR_Column column;
  GDEFSTOR_Result rc =
      gdefstor_revision_append(write, (const uint8_t *)kDoc, strlen(kDoc));
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_blob_put(write, kHash, strlen(kHash), kBlob, sizeof kBlob);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&column, 0, sizeof column);
  column.name = "title";
  column.name_len = 5;
  column.value = (const uint8_t *)"Hello";
  column.value_len = 5;
  rc = gdefstor_projection_put(write, kType, strlen(kType), kEntity,
      strlen(kEntity), &column, 1);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  return gdefstor_outbox_append(write, kOutId, strlen(kOutId), kStream,
      strlen(kStream), (const uint8_t *)kPayload, strlen(kPayload));
}

static void expect_absent(GDEFSTOR_Store * store) {
  uint8_t * bytes = NULL;
  size_t len = 0;
  GDEFSTOR_Projection * row = NULL;
  GDEFSTOR_Outbox_Row * event = NULL;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &bytes, &len));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_blob_get(store, kTenant, strlen(kTenant), kHash, strlen(kHash),
          &bytes, &len));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_projection_get(store, kTenant, strlen(kTenant), kType,
          strlen(kType), kEntity, strlen(kEntity), &row));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), kOutId,
          strlen(kOutId), &event));
}

TEST_F(Abort, NothingVisible) {
  GDEFSTOR_Write * write = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK, stage_all(write));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
  expect_absent(store);
}

TEST_F(Revision, AppendedTwice) {
  GDEFSTOR_Write * write = NULL;
  uint8_t * bytes = NULL;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK, commit_doc(store, NULL));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_revision_append(write, (const uint8_t *)kDoc, strlen(kDoc)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &bytes, &len));
  gdefstor_bytes_free(NULL, bytes);
}

TEST_F(Blob, StoredOnce) {
  GDEFSTOR_Write * write = NULL;
  uint8_t * bytes = NULL;
  size_t len = 0;
  const uint8_t other[] = {'n', 'o'};
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_put(write, kHash, strlen(kHash), kBlob, sizeof kBlob));
  EXPECT_EQ(GDEFSTOR_OK,
      gdefstor_blob_put(write, kHash, strlen(kHash), kBlob, sizeof kBlob));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_blob_put(write, kHash, strlen(kHash), other, sizeof other));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_get(store, kTenant, strlen(kTenant), kHash, strlen(kHash),
          &bytes, &len));
  ASSERT_EQ(sizeof kBlob, len);
  EXPECT_EQ(0, memcmp(bytes, kBlob, len));
  gdefstor_bytes_free(NULL, bytes);
}

TEST_F(Limits, DocumentStopsAtBound) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Write * write = NULL;
  gdefstor_limits_default(&limits);
  limits.max_document_bytes = 32;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), &limits, &write));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_revision_append(write, (const uint8_t *)kDoc, strlen(kDoc)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
  expect_absent(store);
}

TEST_F(Limits, BlobStopsAtBound) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Write * write = NULL;
  gdefstor_limits_default(&limits);
  limits.max_blob_bytes = 3;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), &limits, &write));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_blob_put(write, kHash, strlen(kHash), kBlob, sizeof kBlob));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  expect_absent(store);
}

static int make_zip(const char * name, size_t name_len, const uint8_t * body,
    size_t body_len, int deflate, uint8_t ** out, size_t * out_len) {
  GARC_Sink * sink = NULL;
  GARC_Writer * writer = NULL;
  GARC_Writer_Options opt;
  GARC_Member member;
  const void * data = NULL;
  size_t size = 0;
  uint8_t * copy;
  if (garc_sink_create_memory(&sink) != GARC_OK) {
    return 0;
  }
  garc_writer_options_default(&opt);
  if (deflate) {
    opt.zip_method = GARC_ZIP_METHOD_DEFLATE;
  }
  if (garc_writer_create(sink, GARC_FORMAT_ZIP, &opt, &writer) != GARC_OK) {
    garc_sink_destroy(sink);
    return 0;
  }
  memset(&member, 0, sizeof member);
  member.name = name;
  member.name_length = name_len;
  member.type = GARC_MEMBER_FILE;
  member.size = body_len;
  if (garc_writer_add(writer, &member) != GARC_OK ||
      (body_len > 0 && garc_writer_write(writer, body, body_len) != GARC_OK) ||
      garc_writer_finish(writer) != GARC_OK) {
    garc_writer_destroy(writer);
    garc_sink_destroy(sink);
    return 0;
  }
  garc_writer_destroy(writer);
  if (garc_sink_data(sink, &data, &size) != GARC_OK) {
    garc_sink_destroy(sink);
    return 0;
  }
  copy = (uint8_t *)malloc(size == 0 ? 1 : size);
  if (copy == NULL) {
    garc_sink_destroy(sink);
    return 0;
  }
  if (size > 0) {
    memcpy(copy, data, size);
  }
  garc_sink_destroy(sink);
  *out = copy;
  *out_len = size;
  return 1;
}

TEST_F(Limits, MemberStopsAtBound) {
  GDEFSTOR_Limits limits;
  uint8_t * zip = NULL;
  size_t zip_len = 0;
  const uint8_t body[] = {'h', 'e', 'l', 'l', 'o'};
  ASSERT_TRUE(make_zip("b:aa", 4, body, sizeof body, 0, &zip, &zip_len));
  gdefstor_limits_default(&limits);
  limits.max_member_bytes = 4;
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          &limits));
  free(zip);
  expect_absent(store);
}

TEST_F(Limits, ZipRatioStopsAtBound) {
  GDEFSTOR_Limits limits;
  uint8_t * zip = NULL;
  size_t zip_len = 0;
  std::vector<uint8_t> zeros(64 * 1024, 0);
  ASSERT_TRUE(make_zip("b:aa", 4, zeros.data(), zeros.size(), 1, &zip, &zip_len));
  gdefstor_limits_default(&limits);
  limits.max_zip_ratio = 2;
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          &limits));
  free(zip);
  expect_absent(store);
}

struct Park {
  GDEFSTOR_Store * store;
  GDEFSTOR_Result begin_rc;
  GDEFSTOR_Result get_rc;
};

static void park_entry(void * arg) {
  Park * park = (Park *)arg;
  GDEFSTOR_Write * write = NULL;
  uint8_t * bytes = NULL;
  size_t len = 0;
  park->begin_rc =
      gdefstor_write_begin(park->store, kTenant, strlen(kTenant), NULL, &write);
  if (park->begin_rc != GDEFSTOR_OK) {
    return;
  }
  park->get_rc = gdefstor_revision_get(park->store, kTenant, strlen(kTenant),
      kEntity, strlen(kEntity), 1, &bytes, &len);
  if (park->get_rc == GDEFSTOR_OK) {
    gdefstor_bytes_free(NULL, bytes);
  }
  gdefstor_write_abort(write);
}

TEST_F(Writers, FiberParks) {
  GDEFSTOR_Write * write = NULL;
  GCU_Fiber * fiber = NULL;
  Park park;
  uint8_t * bytes = NULL;
  size_t len = 0;
  memset(&park, 0, sizeof park);
  park.store = store;
  park.begin_rc = GDEFSTOR_RESULT_COUNT;
  park.get_rc = GDEFSTOR_RESULT_COUNT;
  ASSERT_EQ(GDEFSTOR_OK, commit_doc(store, &write));
  ASSERT_EQ(GCU_FIBER_OK,
      gcu_fiber_create(&fiber, park_entry, &park, GCU_FIBER_DEFAULT_STACK_SIZE,
          NULL));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fiber));
  EXPECT_EQ(GDEFSTOR_RESULT_COUNT, park.begin_rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &bytes, &len));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  EXPECT_EQ(GDEFSTOR_OK, park.begin_rc);
  EXPECT_EQ(GDEFSTOR_OK, park.get_rc);
  gcu_fiber_destroy(fiber);
}

TEST_F(Writers, NonFiberBusy) {
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Write * second = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  EXPECT_EQ(GDEFSTOR_ERR_BUSY,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &second));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
}

TEST_F(Formats, UnknownName) {
  GDEFSTOR_Write * write = NULL;
  const char doc[] =
      "{\"format\":\"nope/1\",\"seq\":1,\"entity\":\"post1\","
      "\"type\":\"core.post\",\"lang\":{},\"body\":\"\"}";
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED,
      gdefstor_revision_append(write, (const uint8_t *)doc, strlen(doc)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
}

TEST_F(Formats, NewerVersion) {
  GDEFSTOR_Write * write = NULL;
  const char doc[] =
      "{\"format\":\"defiant-revision/2\",\"seq\":1,\"entity\":\"post1\","
      "\"type\":\"core.post\",\"lang\":{},\"body\":\"\"}";
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED,
      gdefstor_revision_append(write, (const uint8_t *)doc, strlen(doc)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
}

TEST_F(Snapshot, RoundTrip) {
  GDEFSTOR_Store * other = NULL;
  GDEFSTOR_Write * write = NULL;
  uint8_t * zip = NULL;
  size_t zip_len = 0;
  uint8_t * a = NULL;
  uint8_t * b = NULL;
  size_t a_len = 0;
  size_t b_len = 0;
  GDEFSTOR_Projection * left = NULL;
  GDEFSTOR_Projection * right = NULL;
  GDEFSTOR_Column ca;
  GDEFSTOR_Column cb;
  GDEFSTOR_Outbox_Row * oa = NULL;
  GDEFSTOR_Outbox_Row * ob = NULL;
  size_t pa = 0;
  size_t pb = 0;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK, stage_all(write));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_snapshot_export(store, kTenant, strlen(kTenant), NULL, &zip,
          &zip_len));
  ASSERT_EQ(GDEFSTOR_OK, ConformanceOpen(&other, NULL));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_snapshot_import(other, kTenant, strlen(kTenant), zip, zip_len,
          NULL));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &a, &a_len));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_get(other, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &b, &b_len));
  ASSERT_EQ(a_len, b_len);
  EXPECT_EQ(0, memcmp(a, b, a_len));
  gdefstor_bytes_free(NULL, a);
  gdefstor_bytes_free(NULL, b);
  a = NULL;
  b = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_get(store, kTenant, strlen(kTenant), kHash, strlen(kHash),
          &a, &a_len));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_get(other, kTenant, strlen(kTenant), kHash, strlen(kHash),
          &b, &b_len));
  ASSERT_EQ(a_len, b_len);
  EXPECT_EQ(0, memcmp(a, b, a_len));
  gdefstor_bytes_free(NULL, a);
  gdefstor_bytes_free(NULL, b);
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_projection_get(store, kTenant, strlen(kTenant), kType,
          strlen(kType), kEntity, strlen(kEntity), &left));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_projection_get(other, kTenant, strlen(kTenant), kType,
          strlen(kType), kEntity, strlen(kEntity), &right));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_column(left, 0, &ca));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_column(right, 0, &cb));
  ASSERT_EQ(ca.value_len, cb.value_len);
  EXPECT_EQ(0, memcmp(ca.value, cb.value, ca.value_len));
  gdefstor_projection_free(left);
  gdefstor_projection_free(right);
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), kOutId,
          strlen(kOutId), &oa));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_get(other, kTenant, strlen(kTenant), kOutId,
          strlen(kOutId), &ob));
  EXPECT_EQ(gdefstor_outbox_row_seq(oa), gdefstor_outbox_row_seq(ob));
  ASSERT_NE(nullptr, gdefstor_outbox_row_payload(oa, &pa));
  ASSERT_NE(nullptr, gdefstor_outbox_row_payload(ob, &pb));
  ASSERT_EQ(pa, pb);
  EXPECT_EQ(0, memcmp(gdefstor_outbox_row_payload(oa, NULL),
                    gdefstor_outbox_row_payload(ob, NULL), pa));
  gdefstor_outbox_row_free(oa);
  gdefstor_outbox_row_free(ob);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          NULL));
  gdefstor_bytes_free(NULL, zip);
  ConformanceClose(other);
}

TEST_F(Snapshot, BadMember) {
  uint8_t * zip = NULL;
  size_t zip_len = 0;
  const char * stray = "defiant-storage-stray-member";
  const uint8_t body[] = {'x'};
  unlink(stray);
  ASSERT_TRUE(make_zip(stray, strlen(stray), body, sizeof body, 0, &zip, &zip_len));
  EXPECT_EQ(GDEFSTOR_ERR_CORRUPT,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          NULL));
  free(zip);
  EXPECT_NE(0, access(stray, F_OK));
  expect_absent(store);
  unlink(stray);
  ASSERT_TRUE(make_zip("../x", 4, body, sizeof body, 0, &zip, &zip_len));
  EXPECT_EQ(GDEFSTOR_ERR_CORRUPT,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          NULL));
  free(zip);
}

struct ClockBox {
  int64_t now;
};

static int64_t clock_now(void * ctx) {
  return ((ClockBox *)ctx)->now;
}

struct LogBox {
  std::string data;
  GDEFSTOR_Result fail;
};

static GDEFSTOR_Result log_append(void * ctx, const uint8_t * line, size_t len) {
  LogBox * log = (LogBox *)ctx;
  if (log->fail != GDEFSTOR_OK) {
    return log->fail;
  }
  log->data.append(reinterpret_cast<const char *>(line), len);
  return GDEFSTOR_OK;
}

TEST_F(Outbox, LogAfterCommit) {
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Outbox_Row * row = NULL;
  ClockBox box;
  GDEFSTOR_Clock clock;
  LogBox log;
  GDEFSTOR_Sink sink;
  box.now = 0;
  clock.now_ms = clock_now;
  clock.ctx = &box;
  log.fail = GDEFSTOR_OK;
  sink.append = log_append;
  sink.ctx = &log;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK, stage_all(write));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), kOutId,
          strlen(kOutId), &row));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  EXPECT_TRUE(log.data.empty());
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_run(store, kTenant, strlen(kTenant), kStream,
          strlen(kStream), 1000, &clock, &sink, NULL));
  EXPECT_NE(log.data.find("defiant-outbox/1"), std::string::npos);
  EXPECT_EQ('\n', log.data.back());
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), kOutId,
          strlen(kOutId), &row));
}

TEST_F(Outbox, FailedRowStaysParked) {
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Outbox_Row * row = NULL;
  ClockBox box;
  GDEFSTOR_Clock clock;
  LogBox log;
  GDEFSTOR_Sink sink;
  box.now = 10;
  clock.now_ms = clock_now;
  clock.ctx = &box;
  log.fail = GDEFSTOR_ERR_IO;
  sink.append = log_append;
  sink.ctx = &log;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_append(write, kOutId, strlen(kOutId), kStream,
          strlen(kStream), (const uint8_t *)kPayload, strlen(kPayload)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  EXPECT_EQ(GDEFSTOR_ERR_IO,
      gdefstor_outbox_run(store, kTenant, strlen(kTenant), kStream,
          strlen(kStream), 1000, &clock, &sink, NULL));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), kOutId,
          strlen(kOutId), &row));
  EXPECT_EQ(1, gdefstor_outbox_row_parked(row));
  gdefstor_outbox_row_free(row);
  row = NULL;
  box.now = 100000;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_outbox_claim(store, kTenant, strlen(kTenant), kStream,
          strlen(kStream), 1000, &clock, &row));
}

TEST_F(Outbox, LeaseExpires) {
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Outbox_Row * row = NULL;
  ClockBox box;
  GDEFSTOR_Clock clock;
  box.now = 0;
  clock.now_ms = clock_now;
  clock.ctx = &box;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_append(write, kOutId, strlen(kOutId), kStream,
          strlen(kStream), (const uint8_t *)kPayload, strlen(kPayload)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_claim(store, kTenant, strlen(kTenant), kStream,
          strlen(kStream), 1000, &clock, &row));
  EXPECT_EQ(1u, gdefstor_outbox_row_seq(row));
  gdefstor_outbox_row_free(row);
  row = NULL;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_outbox_claim(store, kTenant, strlen(kTenant), kStream,
          strlen(kStream), 1000, &clock, &row));
  box.now = 1000;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_claim(store, kTenant, strlen(kTenant), kStream,
          strlen(kStream), 1000, &clock, &row));
  gdefstor_outbox_row_free(row);
}

TEST_F(ReadSnapshot, OtherTenantMisses) {
  uint8_t * bytes = NULL;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK, commit_doc(store, NULL));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_revision_get(store, "other", 5, kEntity, strlen(kEntity), 1,
          &bytes, &len));
}

TEST_F(ReadSnapshot, CommittedStaysVisible) {
  GDEFSTOR_Write * write = NULL;
  uint8_t * bytes = NULL;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK, commit_doc(store, NULL));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &bytes, &len));
  EXPECT_EQ(strlen(kDoc), len);
  gdefstor_bytes_free(NULL, bytes);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
}

TEST_F(Writers, OtherTenantProceeds) {
  GDEFSTOR_Write * held = NULL;
  GDEFSTOR_Write * other = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &held));
  EXPECT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, "other", 5, NULL, &other));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(other));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(held));
}

TEST_F(Writers, AbortResumes) {
  GDEFSTOR_Write * write = NULL;
  GCU_Fiber * fiber = NULL;
  Park park;
  memset(&park, 0, sizeof park);
  park.store = store;
  park.begin_rc = GDEFSTOR_RESULT_COUNT;
  park.get_rc = GDEFSTOR_RESULT_COUNT;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_append(write, (const uint8_t *)kDoc, strlen(kDoc)));
  ASSERT_EQ(GCU_FIBER_OK,
      gcu_fiber_create(&fiber, park_entry, &park, GCU_FIBER_DEFAULT_STACK_SIZE,
          NULL));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fiber));
  EXPECT_EQ(GDEFSTOR_RESULT_COUNT, park.begin_rc);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
  EXPECT_EQ(GDEFSTOR_OK, park.begin_rc);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, park.get_rc);
  gcu_fiber_destroy(fiber);
}

TEST_F(Revision, DuplicateKeepsBytes) {
  GDEFSTOR_Write * write = NULL;
  uint8_t * first = NULL;
  uint8_t * again = NULL;
  size_t first_len = 0;
  size_t again_len = 0;
  const char other[] =
      "{\"format\":\"defiant-revision/1\",\"seq\":1,\"entity\":\"post1\","
      "\"type\":\"core.post\",\"lang\":{\"en\":{\"title\":[\"Other\"]}},"
      "\"body\":\"\"}";
  ASSERT_EQ(GDEFSTOR_OK, commit_doc(store, NULL));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &first, &first_len));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_revision_append(write, (const uint8_t *)other, strlen(other)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &again, &again_len));
  ASSERT_EQ(first_len, again_len);
  EXPECT_EQ(0, memcmp(first, again, first_len));
  gdefstor_bytes_free(NULL, first);
  gdefstor_bytes_free(NULL, again);
}

TEST_F(Revision, SeqZero) {
  GDEFSTOR_Write * write = NULL;
  const char doc[] =
      "{\"format\":\"defiant-revision/1\",\"seq\":0,\"entity\":\"post1\","
      "\"type\":\"core.post\",\"lang\":{},\"body\":\"\"}";
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  EXPECT_EQ(GDEFSTOR_ERR_CORRUPT,
      gdefstor_revision_append(write, (const uint8_t *)doc, strlen(doc)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
  expect_absent(store);
}

TEST_F(Revision, DestroyDropsStaged) {
  GDEFSTOR_Write * write = NULL;
  uint8_t * bytes = NULL;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_append(write, (const uint8_t *)kDoc, strlen(kDoc)));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &bytes, &len));
  ConformanceClose(store);
  store = NULL;
  ASSERT_EQ(GDEFSTOR_OK, ConformanceOpen(&store, NULL));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &bytes, &len));
}

TEST_F(Outbox, StreamSequences) {
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Outbox_Row * row = NULL;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_append(write, "a", 1, kStream, strlen(kStream),
          (const uint8_t *)kPayload, strlen(kPayload)));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_append(write, "b", 1, kStream, strlen(kStream),
          (const uint8_t *)kPayload, strlen(kPayload)));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_append(write, "c", 1, "other", 5,
          (const uint8_t *)kPayload, strlen(kPayload)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), "a", 1, &row));
  EXPECT_EQ(1u, gdefstor_outbox_row_seq(row));
  gdefstor_outbox_row_free(row);
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), "b", 1, &row));
  EXPECT_EQ(2u, gdefstor_outbox_row_seq(row));
  gdefstor_outbox_row_free(row);
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), "c", 1, &row));
  EXPECT_EQ(1u, gdefstor_outbox_row_seq(row));
  gdefstor_outbox_row_free(row);
}

TEST_F(Outbox, FailedCommitAbsent) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Write * write = NULL;
  gdefstor_limits_default(&limits);
  limits.max_document_bytes = 40;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), &limits, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_put(write, kHash, strlen(kHash), kBlob, sizeof kBlob));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_append(write, kOutId, strlen(kOutId), kStream,
          strlen(kStream), (const uint8_t *)kPayload, strlen(kPayload)));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, gdefstor_write_commit(write));
  expect_absent(store);
}

struct ZipPart {
  const char * name;
  const uint8_t * body;
  size_t body_len;
};

static int make_zip_n(const ZipPart * parts, size_t n, int deflate,
    uint8_t ** out, size_t * out_len) {
  GARC_Sink * sink = NULL;
  GARC_Writer * writer = NULL;
  GARC_Writer_Options opt;
  size_t i;
  const void * data = NULL;
  size_t size = 0;
  uint8_t * copy;
  if (garc_sink_create_memory(&sink) != GARC_OK) {
    return 0;
  }
  garc_writer_options_default(&opt);
  if (deflate) {
    opt.zip_method = GARC_ZIP_METHOD_DEFLATE;
  }
  if (garc_writer_create(sink, GARC_FORMAT_ZIP, &opt, &writer) != GARC_OK) {
    garc_sink_destroy(sink);
    return 0;
  }
  for (i = 0; i < n; i++) {
    GARC_Member member;
    memset(&member, 0, sizeof member);
    member.name = parts[i].name;
    member.name_length = strlen(parts[i].name);
    member.type = GARC_MEMBER_FILE;
    member.size = parts[i].body_len;
    if (garc_writer_add(writer, &member) != GARC_OK ||
        (parts[i].body_len > 0 &&
            garc_writer_write(writer, parts[i].body, parts[i].body_len) !=
                GARC_OK)) {
      garc_writer_destroy(writer);
      garc_sink_destroy(sink);
      return 0;
    }
  }
  if (garc_writer_finish(writer) != GARC_OK) {
    garc_writer_destroy(writer);
    garc_sink_destroy(sink);
    return 0;
  }
  garc_writer_destroy(writer);
  if (garc_sink_data(sink, &data, &size) != GARC_OK) {
    garc_sink_destroy(sink);
    return 0;
  }
  copy = (uint8_t *)malloc(size == 0 ? 1 : size);
  if (copy == NULL) {
    garc_sink_destroy(sink);
    return 0;
  }
  if (size > 0) {
    memcpy(copy, data, size);
  }
  garc_sink_destroy(sink);
  *out = copy;
  *out_len = size;
  return 1;
}

TEST_F(Formats, SnapshotVersion) {
  uint8_t * zip = NULL;
  size_t zip_len = 0;
  const char manifest[] = "{\"format\":\"defiant-snapshot/2\",\"entries\":[]}";
  const char unknown[] = "{\"format\":\"nope/1\",\"entries\":[]}";
  const char listed[] =
      "{\"format\":\"defiant-snapshot/1\",\"entries\":[{\"key\":\"o:65767431\","
      "\"kind\":\"outbox\"}]}";
  const char out_v2[] =
      "{\"format\":\"defiant-outbox/2\",\"id\":\"evt1\",\"stream\":\"posts\","
      "\"seq\":1,\"payload\":{\"n\":1}}";
  const char out_name[] =
      "{\"format\":\"nope/1\",\"id\":\"evt1\",\"stream\":\"posts\","
      "\"seq\":1,\"payload\":{\"n\":1}}";
  ZipPart one;
  ZipPart two[2];
  one.name = "manifest";
  one.body = (const uint8_t *)manifest;
  one.body_len = strlen(manifest);
  ASSERT_TRUE(make_zip_n(&one, 1, 0, &zip, &zip_len));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          NULL));
  free(zip);
  expect_absent(store);
  one.body = (const uint8_t *)unknown;
  one.body_len = strlen(unknown);
  ASSERT_TRUE(make_zip_n(&one, 1, 0, &zip, &zip_len));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          NULL));
  free(zip);
  expect_absent(store);
  two[0].name = "manifest";
  two[0].body = (const uint8_t *)listed;
  two[0].body_len = strlen(listed);
  two[1].name = "o:65767431";
  two[1].body = (const uint8_t *)out_v2;
  two[1].body_len = strlen(out_v2);
  ASSERT_TRUE(make_zip_n(two, 2, 0, &zip, &zip_len));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          NULL));
  free(zip);
  expect_absent(store);
  two[1].body = (const uint8_t *)out_name;
  two[1].body_len = strlen(out_name);
  ASSERT_TRUE(make_zip_n(two, 2, 0, &zip, &zip_len));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          NULL));
  free(zip);
  expect_absent(store);
}

TEST_F(Limits, ZipRatioAtBound) {
  GDEFSTOR_Limits limits;
  uint8_t * zip = NULL;
  size_t zip_len = 0;
  const char manifest[] = "{\"format\":\"defiant-snapshot/1\",\"entries\":[]}";
  uint8_t extra[6];
  ZipPart one;
  memset(extra, 0, sizeof extra);
  one.name = "manifest";
  one.body = (const uint8_t *)manifest;
  one.body_len = strlen(manifest);
  ASSERT_TRUE(make_zip_n(&one, 1, 0, &zip, &zip_len));
  gdefstor_limits_default(&limits);
  limits.max_zip_ratio = 1;
  EXPECT_EQ(GDEFSTOR_OK,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          &limits));
  free(zip);
  expect_absent(store);
  one.body = extra;
  one.body_len = sizeof extra;
  ASSERT_TRUE(make_zip_n(&one, 1, 1, &zip, &zip_len));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          &limits));
  free(zip);
  expect_absent(store);
}

TEST_F(Limits, RevisionsAtBound) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Write * write = NULL;
  uint8_t * bytes = NULL;
  size_t len = 0;
  const char second[] =
      "{\"format\":\"defiant-revision/1\",\"seq\":2,\"entity\":\"post1\","
      "\"type\":\"core.post\",\"lang\":{},\"body\":\"\"}";
  gdefstor_limits_default(&limits);
  limits.max_revisions = 1;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), &limits, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_append(write, (const uint8_t *)kDoc, strlen(kDoc)));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_revision_append(write, (const uint8_t *)second, strlen(second)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 1, &bytes, &len));
  gdefstor_bytes_free(NULL, bytes);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_revision_get(store, kTenant, strlen(kTenant), kEntity,
          strlen(kEntity), 2, &bytes, &len));
}

TEST_F(Limits, OutboxRowsAtBound) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Outbox_Row * row = NULL;
  gdefstor_limits_default(&limits);
  limits.max_outbox_rows = 1;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), &limits, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_append(write, "a", 1, kStream, strlen(kStream),
          (const uint8_t *)kPayload, strlen(kPayload)));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_outbox_append(write, "b", 1, kStream, strlen(kStream),
          (const uint8_t *)kPayload, strlen(kPayload)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_commit(write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), "a", 1, &row));
  gdefstor_outbox_row_free(row);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID,
      gdefstor_outbox_get(store, kTenant, strlen(kTenant), "b", 1, &row));
}

TEST_F(Limits, ColumnsAtBound) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Column columns[2];
  gdefstor_limits_default(&limits);
  limits.max_columns = 1;
  memset(columns, 0, sizeof columns);
  columns[0].name = "title";
  columns[0].name_len = 5;
  columns[0].value = (const uint8_t *)"Hello";
  columns[0].value_len = 5;
  columns[1].name = "body";
  columns[1].name_len = 4;
  columns[1].value = (const uint8_t *)"x";
  columns[1].value_len = 1;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), &limits, &write));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_projection_put(write, kType, strlen(kType), kEntity,
          strlen(kEntity), columns, 1));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_projection_put(write, kType, strlen(kType), kEntity,
          strlen(kEntity), columns, 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
}

TEST_F(Limits, MembersAtBound) {
  GDEFSTOR_Limits limits;
  uint8_t * zip = NULL;
  size_t zip_len = 0;
  const char empty[] = "{\"format\":\"defiant-snapshot/1\",\"entries\":[]}";
  const char listed[] =
      "{\"format\":\"defiant-snapshot/1\",\"entries\":[{\"key\":\"b:61\","
      "\"kind\":\"blob\"}]}";
  const uint8_t blob[] = {'x'};
  ZipPart one;
  ZipPart two[2];
  gdefstor_limits_default(&limits);
  limits.max_members = 1;
  one.name = "manifest";
  one.body = (const uint8_t *)empty;
  one.body_len = strlen(empty);
  ASSERT_TRUE(make_zip_n(&one, 1, 0, &zip, &zip_len));
  EXPECT_EQ(GDEFSTOR_OK,
      gdefstor_snapshot_import(store, kTenant, strlen(kTenant), zip, zip_len,
          &limits));
  free(zip);
  two[0].name = "manifest";
  two[0].body = (const uint8_t *)listed;
  two[0].body_len = strlen(listed);
  two[1].name = "b:61";
  two[1].body = blob;
  two[1].body_len = sizeof blob;
  ASSERT_TRUE(make_zip_n(two, 2, 0, &zip, &zip_len));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_snapshot_import(store, "other", 5, zip, zip_len, &limits));
  free(zip);
}

TEST_F(Limits, JsonDepthAtBound) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Write * write = NULL;
  gdefstor_limits_default(&limits);
  limits.max_json_depth = 4;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), &limits, &write));
  EXPECT_EQ(GDEFSTOR_OK,
      gdefstor_revision_append(write, (const uint8_t *)kDoc, strlen(kDoc)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
  limits.max_json_depth = 3;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_write_begin(store, kTenant, strlen(kTenant), &limits, &write));
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT,
      gdefstor_revision_append(write, (const uint8_t *)kDoc, strlen(kDoc)));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(write));
}

struct CountAlloc {
  const GCU_Allocator * base;
  int fail_at;
  int calls;
  int live;
  int underflow;
};

static int count_hit(CountAlloc * c) {
  c->calls += 1;
  return c->fail_at > 0 && c->calls == c->fail_at;
}

static void * count_malloc(void * ctx, size_t size) {
  CountAlloc * c = (CountAlloc *)ctx;
  void * p;
  if (count_hit(c)) {
    return NULL;
  }
  p = gcu_allocator_malloc(c->base, size);
  if (p != NULL) {
    c->live += 1;
  }
  return p;
}

static void * count_calloc(void * ctx, size_t nitems, size_t size) {
  CountAlloc * c = (CountAlloc *)ctx;
  void * p;
  if (count_hit(c)) {
    return NULL;
  }
  p = gcu_allocator_calloc(c->base, nitems, size);
  if (p != NULL) {
    c->live += 1;
  }
  return p;
}

static void * count_realloc(void * ctx, void * ptr, size_t size) {
  CountAlloc * c = (CountAlloc *)ctx;
  void * p;
  if (count_hit(c)) {
    return NULL;
  }
  p = gcu_allocator_realloc(c->base, ptr, size);
  if (p != NULL && ptr == NULL) {
    c->live += 1;
  }
  return p;
}

static void count_free(void * ctx, void * ptr) {
  CountAlloc * c = (CountAlloc *)ctx;
  if (ptr == NULL) {
    return;
  }
  if (c->live <= 0) {
    c->underflow += 1;
  } else {
    c->live -= 1;
  }
  gcu_allocator_free(c->base, ptr);
}

static GDEFSTOR_Result oom_once(const GDEFSTOR_Allocator * alloc) {
  GDEFSTOR_Store * store = NULL;
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Result rc = ConformanceOpen(&store, alloc);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefstor_write_begin(store, kTenant, strlen(kTenant), NULL, &write);
  if (rc != GDEFSTOR_OK) {
    ConformanceClose(store);
    return rc;
  }
  rc = stage_all(write);
  if (rc != GDEFSTOR_OK) {
    gdefstor_write_abort(write);
    ConformanceClose(store);
    return rc;
  }
  rc = gdefstor_write_commit(write);
  ConformanceClose(store);
  return rc;
}

TEST(Allocator, FailsClean) {
  CountAlloc count;
  GCU_Allocator alloc;
  int budget;
  int i;
  memset(&count, 0, sizeof count);
  count.base = gcu_allocator_default();
  alloc.ctx = &count;
  alloc.malloc_fn = count_malloc;
  alloc.calloc_fn = count_calloc;
  alloc.realloc_fn = count_realloc;
  alloc.free_fn = count_free;
  ASSERT_EQ(GDEFSTOR_OK, oom_once((const GDEFSTOR_Allocator *)&alloc));
  EXPECT_EQ(0, count.live);
  EXPECT_EQ(0, count.underflow);
  budget = count.calls;
  ASSERT_GT(budget, 0);
  for (i = 1; i <= budget; i++) {
    memset(&count, 0, sizeof count);
    count.base = gcu_allocator_default();
    count.fail_at = i;
    alloc.ctx = &count;
    EXPECT_EQ(GDEFSTOR_ERR_OOM, oom_once((const GDEFSTOR_Allocator *)&alloc))
        << "allocation " << i;
    EXPECT_EQ(0, count.live) << "allocation " << i;
    EXPECT_EQ(0, count.underflow) << "allocation " << i;
  }
}
