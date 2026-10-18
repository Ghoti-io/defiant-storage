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
 * @file value.h
 * @brief Logical field types and the typed value the entity port speaks.
 * @stability free
 *
 * The model is specified in documentation/model.md.
 */

#ifndef GHOTI_IO_GDEFSTOR_VALUE_H
#define GHOTI_IO_GDEFSTOR_VALUE_H

#include <ghoti.io/defiant-storage/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The logical type of a field (AD-13). Each backend maps it.
 *
 * Money is ::GDEFSTOR_FIELD_DECIMAL, never ::GDEFSTOR_FIELD_FLOAT.
 */
typedef enum GDEFSTOR_FieldType {
  GDEFSTOR_FIELD_TEXT = 1,  ///< UTF-8 without NUL. GDEFSTOR_Value::bytes.
  GDEFSTOR_FIELD_INTEGER,   ///< int64. GDEFSTOR_Value::integer.
  GDEFSTOR_FIELD_DECIMAL,   ///< Exact: the unscaled integer at the field's scale.
  GDEFSTOR_FIELD_FLOAT,     ///< A finite double. GDEFSTOR_Value::real.
  GDEFSTOR_FIELD_BOOLEAN,   ///< 0 or 1 in GDEFSTOR_Value::integer.
  GDEFSTOR_FIELD_DATE,      ///< Days since 1970-01-01, years 1 to 9999.
  GDEFSTOR_FIELD_DATETIME,  ///< Milliseconds since 1970-01-01T00:00Z, years 1 to 9999.
  GDEFSTOR_FIELD_REFERENCE, ///< Another entity's public id, in GDEFSTOR_Value::bytes.
  GDEFSTOR_FIELD_GROUP      ///< A group of fields. Holds items, never a value.
} GDEFSTOR_FieldType;

/** A field flag: the field holds any number of items, kept in order. */
#define GDEFSTOR_FIELD_MULTIPLE 0x1u
/** A field flag: a save must supply it (a multiple: at least one item). */
#define GDEFSTOR_FIELD_REQUIRED 0x2u
/** A field flag: a backend indexes it. A hint; it never changes an answer. */
#define GDEFSTOR_FIELD_INDEXED 0x4u
/** A field flag: personal data (AD-19). The value is ciphertext the caller
 * made, stored with its subject; the port never sees a key or a plaintext. */
#define GDEFSTOR_FIELD_PERSONAL 0x8u

/**
 * @brief One typed value. Borrowed for the call that takes it.
 *
 * @ref type 0 is "no value" (a null column). Otherwise it is the field's
 * logical type and the member that type names carries the value. A value of
 * a ::GDEFSTOR_FIELD_PERSONAL field carries the ciphertext in @ref bytes,
 * whatever the field's logical type, and the subject's public id in
 * @ref subject; the port compares the bytes, so a caller that re-encrypts an
 * unchanged plaintext passes the stored ciphertext through instead (AD-19:
 * whether a value changed is decided on the plaintext, by the caller).
 *
 * Not size-prefixed: it is a plain datum, like GDEFSTOR_Column.
 */
typedef struct GDEFSTOR_Value {
  int type;                ///< A ::GDEFSTOR_FieldType, or 0 for no value.
  int64_t integer;         ///< INTEGER, DECIMAL (unscaled), BOOLEAN, DATE, DATETIME.
  double real;             ///< FLOAT.
  const uint8_t * bytes;   ///< TEXT, REFERENCE, or a personal ciphertext.
  size_t len;              ///< Length of @ref bytes.
  const uint8_t * subject; ///< Personal values only: the subject's public id.
  size_t subject_len;      ///< Length of @ref subject.
} GDEFSTOR_Value;

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSTOR_VALUE_H */
