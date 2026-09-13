/* Copyright (c) 2026, Percona and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* See http://code.google.com/p/googletest/wiki/Primer */

#include <gtest/gtest.h>

#include <cstdint>

#include "storage/innobase/include/ddl0ddl.h"
#include "storage/innobase/include/univ.i"

namespace innodb_ddl0ddl_unittest {

/* The alignment is not configurable: the single allocation point keeps the
upstream UNIV_SECTOR_SIZE. Pinning the value here is what
makes a regression ("helper silently starts returning IO_BLOCK_SIZE" or
similar) visible as a one-line diff instead of only as address-check noise
below. */
TEST(ddl0ddl, io_buffer_alignment_is_unchanged_from_upstream) {
  EXPECT_EQ(ddl::io_buffer_alignment(), UNIV_SECTOR_SIZE);
}

/* Sizes exercised below mirror the shapes the 8 real call sites pass:
below/above/straddling IO_BLOCK_SIZE, and a couple of odd (+1/+512) lengths to
make sure alloc_io_buffer() does not depend on the caller rounding to a block
boundary. */
static const size_t k_sizes[] = {
    16 * 1024,            // 16 KiB
    20 * 1024,            // 20 KiB
    64 * 1024 + 512,      // 64 KiB + 512
    1024 * 1024,          // 1 MiB
    4 * 1024 * 1024 + 1,  // 4 MiB + 1
    UNIV_SECTOR_SIZE,     // smallest legal block
    4096,                 // IO_BLOCK_SIZE
    8 * 1024 * 1024,      // 8 MiB
};

/* At least 64 address checks are required as a witness that the loop
below actually ran to completion rather than exiting early (e.g. on the first
allocation failure) - n_checks is asserted, not just implied by the loop
bounds. */
TEST(ddl0ddl, alloc_io_buffer_addresses_are_aligned) {
  constexpr size_t n_repeats = 8;
  size_t n_checks = 0;

  for (size_t repeat = 0; repeat < n_repeats; ++repeat) {
    for (auto size : k_sizes) {
      auto buf = ddl::alloc_io_buffer(size);

      ASSERT_TRUE(buf) << "OOM at size=" << size << ", repeat=" << repeat;
      EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buf.get()) %
                    ddl::io_buffer_alignment(),
                0u)
          << "size=" << size << ", repeat=" << repeat;

      ++n_checks;
    }
  }

  EXPECT_GE(n_checks, 64u);
}

}  // namespace innodb_ddl0ddl_unittest
