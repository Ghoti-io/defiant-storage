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
 * @file namespace.h
 * @brief Maps every public name into this build's version namespace.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 */

#ifndef GHOTI_IO_GDEFSTOR_NAMESPACE_H
#define GHOTI_IO_GDEFSTOR_NAMESPACE_H

#include <ghoti.io/defiant-storage/libver.h>

/// @cond HIDDEN_SYMBOLS

#define GDEFSTOR_Allocator GHOTIIO_DEFSTOR(GDEFSTOR_Allocator)
#define GDEFSTOR_Backend GHOTIIO_DEFSTOR(GDEFSTOR_Backend)
#define GDEFSTOR_Clock GHOTIIO_DEFSTOR(GDEFSTOR_Clock)
#define GDEFSTOR_Column GHOTIIO_DEFSTOR(GDEFSTOR_Column)
#define GDEFSTOR_Limits GHOTIIO_DEFSTOR(GDEFSTOR_Limits)
#define GDEFSTOR_Outbox_Row GHOTIIO_DEFSTOR(GDEFSTOR_Outbox_Row)
#define GDEFSTOR_Projection GHOTIIO_DEFSTOR(GDEFSTOR_Projection)
#define GDEFSTOR_Query GHOTIIO_DEFSTOR(GDEFSTOR_Query)
#define GDEFSTOR_QueryEnv GHOTIIO_DEFSTOR(GDEFSTOR_QueryEnv)
#define GDEFSTOR_QueryKey GHOTIIO_DEFSTOR(GDEFSTOR_QueryKey)
#define GDEFSTOR_QueryKind GHOTIIO_DEFSTOR(GDEFSTOR_QueryKind)
#define GDEFSTOR_QueryNode GHOTIIO_DEFSTOR(GDEFSTOR_QueryNode)
#define GDEFSTOR_QueryOp GHOTIIO_DEFSTOR(GDEFSTOR_QueryOp)
#define GDEFSTOR_QueryOperand GHOTIIO_DEFSTOR(GDEFSTOR_QueryOperand)
#define GDEFSTOR_QueryPlan GHOTIIO_DEFSTOR(GDEFSTOR_QueryPlan)
#define GDEFSTOR_QueryResult GHOTIIO_DEFSTOR(GDEFSTOR_QueryResult)
#define GDEFSTOR_QueryRowRef GHOTIIO_DEFSTOR(GDEFSTOR_QueryRowRef)
#define GDEFSTOR_QueryRows GHOTIIO_DEFSTOR(GDEFSTOR_QueryRows)
#define GDEFSTOR_QuerySort GHOTIIO_DEFSTOR(GDEFSTOR_QuerySort)
#define GDEFSTOR_QueryText GHOTIIO_DEFSTOR(GDEFSTOR_QueryText)
#define GDEFSTOR_Result GHOTIIO_DEFSTOR(GDEFSTOR_Result)
#define GDEFSTOR_Revision GHOTIIO_DEFSTOR(GDEFSTOR_Revision)
#define GDEFSTOR_Sink GHOTIIO_DEFSTOR(GDEFSTOR_Sink)
#define GDEFSTOR_Store GHOTIIO_DEFSTOR(GDEFSTOR_Store)
#define GDEFSTOR_Write GHOTIIO_DEFSTOR(GDEFSTOR_Write)

#define gdefstor_allocator_default GHOTIIO_DEFSTOR(gdefstor_allocator_default)
#define gdefstor_blob_get GHOTIIO_DEFSTOR(gdefstor_blob_get)
#define gdefstor_blob_put GHOTIIO_DEFSTOR(gdefstor_blob_put)
#define gdefstor_bytes_free GHOTIIO_DEFSTOR(gdefstor_bytes_free)
#define gdefstor_limits_default GHOTIIO_DEFSTOR(gdefstor_limits_default)
#define gdefstor_outbox_ack GHOTIIO_DEFSTOR(gdefstor_outbox_ack)
#define gdefstor_outbox_append GHOTIIO_DEFSTOR(gdefstor_outbox_append)
#define gdefstor_outbox_claim GHOTIIO_DEFSTOR(gdefstor_outbox_claim)
#define gdefstor_outbox_fail GHOTIIO_DEFSTOR(gdefstor_outbox_fail)
#define gdefstor_outbox_get GHOTIIO_DEFSTOR(gdefstor_outbox_get)
#define gdefstor_outbox_row_free GHOTIIO_DEFSTOR(gdefstor_outbox_row_free)
#define gdefstor_outbox_row_parked GHOTIIO_DEFSTOR(gdefstor_outbox_row_parked)
#define gdefstor_outbox_row_payload GHOTIIO_DEFSTOR(gdefstor_outbox_row_payload)
#define gdefstor_outbox_row_seq GHOTIIO_DEFSTOR(gdefstor_outbox_row_seq)
#define gdefstor_outbox_run GHOTIIO_DEFSTOR(gdefstor_outbox_run)
#define gdefstor_projection_column GHOTIIO_DEFSTOR(gdefstor_projection_column)
#define gdefstor_projection_column_count \
  GHOTIIO_DEFSTOR(gdefstor_projection_column_count)
#define gdefstor_projection_free GHOTIIO_DEFSTOR(gdefstor_projection_free)
#define gdefstor_projection_get GHOTIIO_DEFSTOR(gdefstor_projection_get)
#define gdefstor_projection_delete GHOTIIO_DEFSTOR(gdefstor_projection_delete)
#define gdefstor_projection_put GHOTIIO_DEFSTOR(gdefstor_projection_put)
#define gdefstor_projection_count GHOTIIO_DEFSTOR(gdefstor_projection_count)
#define gdefstor_projection_key GHOTIIO_DEFSTOR(gdefstor_projection_key)
#define gdefstor_projection_key_entity \
  GHOTIIO_DEFSTOR(gdefstor_projection_key_entity)
#define gdefstor_projection_key_prefix \
  GHOTIIO_DEFSTOR(gdefstor_projection_key_prefix)
#define gdefstor_projection_query GHOTIIO_DEFSTOR(gdefstor_projection_query)
#define gdefstor_query_first_text GHOTIIO_DEFSTOR(gdefstor_query_first_text)
#define gdefstor_query_key_compare GHOTIIO_DEFSTOR(gdefstor_query_key_compare)
#define gdefstor_query_key_free GHOTIIO_DEFSTOR(gdefstor_query_key_free)
#define gdefstor_query_key_make GHOTIIO_DEFSTOR(gdefstor_query_key_make)
#define gdefstor_query_leaf GHOTIIO_DEFSTOR(gdefstor_query_leaf)
#define gdefstor_query_result_entity \
  GHOTIIO_DEFSTOR(gdefstor_query_result_entity)
#define gdefstor_query_result_free GHOTIIO_DEFSTOR(gdefstor_query_result_free)
#define gdefstor_query_result_rows GHOTIIO_DEFSTOR(gdefstor_query_result_rows)
#define gdefstor_query_result_total GHOTIIO_DEFSTOR(gdefstor_query_result_total)
#define gdefstor_query_result_value GHOTIIO_DEFSTOR(gdefstor_query_result_value)
#define gdefstor_result_string GHOTIIO_DEFSTOR(gdefstor_result_string)
#define gdefstor_revision_append GHOTIIO_DEFSTOR(gdefstor_revision_append)
#define gdefstor_revision_body GHOTIIO_DEFSTOR(gdefstor_revision_body)
#define gdefstor_revision_entity GHOTIIO_DEFSTOR(gdefstor_revision_entity)
#define gdefstor_revision_free GHOTIIO_DEFSTOR(gdefstor_revision_free)
#define gdefstor_revision_lang GHOTIIO_DEFSTOR(gdefstor_revision_lang)
#define gdefstor_revision_get GHOTIIO_DEFSTOR(gdefstor_revision_get)
#define gdefstor_revision_parse GHOTIIO_DEFSTOR(gdefstor_revision_parse)
#define gdefstor_revision_row GHOTIIO_DEFSTOR(gdefstor_revision_row)
#define gdefstor_revision_seq GHOTIIO_DEFSTOR(gdefstor_revision_seq)
#define gdefstor_revision_write GHOTIIO_DEFSTOR(gdefstor_revision_write)
#define gdefstor_snapshot_export GHOTIIO_DEFSTOR(gdefstor_snapshot_export)
#define gdefstor_snapshot_export_ledger                                        \
  GHOTIIO_DEFSTOR(gdefstor_snapshot_export_ledger)
#define gdefstor_snapshot_import GHOTIIO_DEFSTOR(gdefstor_snapshot_import)
#define gdefstor_snapshot_ledger GHOTIIO_DEFSTOR(gdefstor_snapshot_ledger)
#define gdefstor_store_create GHOTIIO_DEFSTOR(gdefstor_store_create)
#define gdefstor_store_destroy GHOTIIO_DEFSTOR(gdefstor_store_destroy)
#define gdefstor_write_abort GHOTIIO_DEFSTOR(gdefstor_write_abort)
#define gdefstor_write_begin GHOTIIO_DEFSTOR(gdefstor_write_begin)
#define gdefstor_write_commit GHOTIIO_DEFSTOR(gdefstor_write_commit)

/// @endcond

#endif /* GHOTI_IO_GDEFSTOR_NAMESPACE_H */
