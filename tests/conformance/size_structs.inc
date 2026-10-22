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
 * Every public struct of defiant-storage that begins with a `size` member,
 * once, with the file that runs it (size_probe.inc). Installed with the
 * conformance suites: a backend's model.inc includes sizes.inc, which runs the
 * structs of the port that every backend takes. tools/check-size-structs.sh
 * fails, naming the struct, when a header declares one that is missing here.
 */

#ifndef GHOTI_DEFSTOR_SIZE_STRUCTS_INC
#define GHOTI_DEFSTOR_SIZE_STRUCTS_INC

#include "size_probe.inc"

#define GDEFSTOR_SIZE_STRUCTS(X) \
  X(GDEFSTOR_KeyStoreBackend, "tests/conformance/test_model.cpp") \
  X(GDEFSTOR_KeyCipher, "tests/conformance/keystore.inc") \
  X(GDEFSTOR_FieldSpec, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_TypeSpec, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_FieldInfo, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_TreeItem, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_BlindMac, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_EntityInfo, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_RevisionMeta, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_LanguageTree, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_RevisionInfo, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_StateInfo, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_DiffItem, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_DraftTree, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_DraftInfo, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_BlindHit, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_SubjectHit, "tests/conformance/sizes.inc") \
  X(GDEFSTOR_OutboxEntry, "tests/conformance/system.inc") \
  X(GDEFSTOR_ModelBackend, "tests/conformance/test_model.cpp") \
  X(GDEFSTOR_Query, "tests/conformance/sizes.inc")

namespace size_structs {

inline const std::vector<Entry> & Storage() {
  static const std::vector<Entry> all = {
#define X(NAME, HOME) {#NAME, HOME, sizeof(NAME)},
      GDEFSTOR_SIZE_STRUCTS(X)
#undef X
  };
  return all;
}

}  // namespace size_structs

#endif
