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
 * Result strings, default limits and the default allocator.
 */

#include <ghoti.io/defiant-storage/macros.h>
#include <ghoti.io/defiant-storage/core.h>
#include <ghoti.io/cutil/allocator.h>

const GDEFSTOR_Allocator * gdefstor_allocator_default(void) {
  return gcu_allocator_default();
}

const char * gdefstor_result_string(GDEFSTOR_Result result) {
  switch (result) {
  case GDEFSTOR_OK:
    return "No error";
  case GDEFSTOR_ERR_IO:
    return "I/O error";
  case GDEFSTOR_ERR_FORMAT:
    return "Invalid format";
  case GDEFSTOR_ERR_UNSUPPORTED:
    return "Unsupported format";
  case GDEFSTOR_ERR_LIMIT:
    return "Limit exceeded";
  case GDEFSTOR_ERR_CORRUPT:
    return "Corrupt data";
  case GDEFSTOR_ERR_OOM:
    return "Out of memory";
  case GDEFSTOR_ERR_INVALID:
    return "Invalid argument";
  case GDEFSTOR_ERR_INTERNAL:
    return "Internal error";
  case GDEFSTOR_ERR_BUSY:
    return "Writer busy";
  case GDEFSTOR_ERR_CONFLICT:
    return "Revision conflict";
  case GDEFSTOR_RESULT_COUNT:
  default:
    return "Unknown error";
  }
}

void gdefstor_limits_default(GDEFSTOR_Limits * limits) {
  if (limits == NULL) {
    return;
  }
  limits->max_document_bytes = 1024u * 1024u;
  limits->max_blob_bytes = 16u * 1024u * 1024u;
  limits->max_member_bytes = 16u * 1024u * 1024u;
  limits->max_members = 100000u;
  limits->max_snapshot_bytes = 64u * 1024u * 1024u;
  limits->max_zip_ratio = 100u;
  limits->max_public_id_bytes = 256u;
  limits->max_tenant_bytes = 256u;
  limits->max_type_bytes = 256u;
  limits->max_body_bytes = 4096u;
  limits->max_hash_bytes = 128u;
  limits->max_json_depth = 32u;
  limits->max_columns = 64u;
  limits->max_column_name_bytes = 128u;
  limits->max_column_value_bytes = 1024u * 1024u;
  limits->max_outbox_payload_bytes = 1024u * 1024u;
  limits->max_stream_bytes = 256u;
  limits->max_revisions = 100000u;
  limits->max_blobs = 100000u;
  limits->max_projections = 100000u;
  limits->max_outbox_rows = 100000u;
}

void gdefstor_bytes_free(const GDEFSTOR_Allocator * allocator, void * bytes) {
  gcu_allocator_free(allocator == NULL ? gdefstor_allocator_default()
                                       : allocator,
      bytes);
}
