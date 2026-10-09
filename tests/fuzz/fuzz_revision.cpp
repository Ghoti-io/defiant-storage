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
 * Fuzz defiant-revision documents. The first byte selects tighter limits.
 */

#include <ghoti.io/defiant-storage/defiant-storage.h>

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  GDEFSTOR_Limits limits;
  GDEFSTOR_Revision * revision = NULL;
  uint8_t * written = NULL;
  size_t written_len = 0;
  unsigned opt;
  if (data == NULL || size < 2) {
    return 0;
  }
  opt = data[0];
  gdefstor_limits_default(&limits);
  if (opt & 1u) {
    limits.max_document_bytes = 64;
  }
  if (opt & 2u) {
    limits.max_json_depth = 3;
  }
  if (opt & 4u) {
    limits.max_public_id_bytes = 8;
  }
  if (opt & 8u) {
    limits.max_body_bytes = 4;
  }
  if (gdefstor_revision_parse(data + 1, size - 1, &limits, NULL, &revision) ==
      GDEFSTOR_OK) {
    if (gdefstor_revision_write(revision, &limits, NULL, &written,
            &written_len) == GDEFSTOR_OK) {
      gdefstor_bytes_free(NULL, written);
    }
    gdefstor_revision_free(revision);
  }
  return 0;
}
