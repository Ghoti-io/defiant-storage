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
 * The table every size-member struct is held to (AD-34): accepted at exactly
 * its own size, refused at any other. Installed beside the port with the
 * conformance suites; the including file includes gtest first.
 *
 * A struct is listed once, with the file that runs it. The home calls
 * size_structs::Run() with a probe for every struct listed for it, and the
 * probe says whether the call that takes the struct accepted it at the size it
 * is given (for a struct the library fills, whether it filled that size). Run
 * puts each struct to its own size, which must be accepted, and to sizes
 * shorter and longer, which must not. A struct listed for a home with no
 * probe fails, and so does a probe for one not listed.
 * tools/check-size-structs.sh holds the list to the headers.
 */

#ifndef GHOTI_DEFSTOR_SIZE_PROBE_INC
#define GHOTI_DEFSTOR_SIZE_PROBE_INC

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace size_structs {

struct Entry {
  const char * name;
  const char * home;
  size_t size;
};

/** True when the call accepted the struct at the given size. */
using Probe = std::function<bool(size_t size)>;

inline void Run(const std::vector<Entry> & all, const char * home,
    const std::map<std::string, Probe> & probes) {
  size_t ran = 0;
  for (const Entry & e : all) {
    if (std::string(e.home) != home) {
      continue;
    }
    auto it = probes.find(e.name);
    if (it == probes.end()) {
      ADD_FAILURE() << e.name << " is listed for " << home
                    << " and has no probe";
      continue;
    }
    ran++;
    EXPECT_TRUE(it->second(e.size))
        << e.name << " was not accepted at its own size " << e.size;
    const size_t others[] = {0, sizeof(size_t), e.size / 2, e.size - 1,
        e.size + 1, e.size + sizeof(void *)};
    for (size_t other : others) {
      if (other == e.size) {
        continue;
      }
      EXPECT_FALSE(it->second(other))
          << e.name << " was accepted at size " << other << ", its own is "
          << e.size;
    }
  }
  for (const auto & p : probes) {
    bool listed = false;
    for (const Entry & e : all) {
      listed = listed || (p.first == e.name && std::string(e.home) == home);
    }
    EXPECT_TRUE(listed) << p.first << " has a probe in " << home
                        << " and is not listed for it";
  }
  EXPECT_GT(ran, 0u) << home << " runs no struct";
}

}  // namespace size_structs

#endif
