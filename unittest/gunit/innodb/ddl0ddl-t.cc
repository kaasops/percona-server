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

/* ddl::io_alignment is an external-linkage global (the innodb_ddl_io_alignment
sysvar's storage) shared by every translation unit merged into this gunit
binary. A test that needs a non-default value saves and restores it, instead
of leaving a mutation for whichever test happens to run next - this class
exists so that restore cannot be forgotten (also covers the ASSERT_TRUE early
returns below, unlike a plain save/restore pair would). */
class Io_alignment_guard {
 public:
  explicit Io_alignment_guard(ulong value) : m_saved(ddl::io_alignment) {
    ddl::io_alignment = value;
  }
  ~Io_alignment_guard() { ddl::io_alignment = m_saved; }

 private:
  ulong m_saved;
};

/* Deliberately does not set ddl::io_alignment itself (unlike the tests
below): this checks the actual link-time initializer in ddl0ddl.cc. Every
other test in this file that changes the value restores it via
Io_alignment_guard, so by the time this test runs - in any order - the
initializer is what's left to observe. */
TEST(ddl0ddl, default_alignment_is_upstream_sector_size) {
  EXPECT_EQ(ddl::io_alignment, static_cast<ulong>(UNIV_SECTOR_SIZE));
  EXPECT_EQ(ddl::io_buffer_alignment(), UNIV_SECTOR_SIZE);
}

TEST(ddl0ddl, is_valid_io_alignment_boundaries) {
  for (auto v : ddl::IO_ALIGNMENTS) {
    EXPECT_TRUE(ddl::is_valid_io_alignment(v)) << "v=" << v;
  }
  for (size_t v :
       {size_t{0}, size_t{256}, size_t{1024}, size_t{2048}, size_t{8192}}) {
    EXPECT_FALSE(ddl::is_valid_io_alignment(v)) << "v=" << v;
  }
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

/* For each valid alignment (512 and 4096 today), at least 64 address checks
are required as a witness that the loop below actually ran to completion
rather than exiting early (e.g. on the first allocation failure) - n_checks is
asserted, not just implied by the loop bounds. */
TEST(ddl0ddl, alloc_io_buffer_addresses_are_aligned) {
  constexpr size_t n_repeats = 8;

  for (auto alignment : ddl::IO_ALIGNMENTS) {
    Io_alignment_guard guard(alignment);
    ASSERT_EQ(ddl::io_buffer_alignment(), alignment);

    size_t n_checks = 0;
    for (size_t repeat = 0; repeat < n_repeats; ++repeat) {
      for (auto size : k_sizes) {
        auto buf = ddl::alloc_io_buffer(size);

        ASSERT_TRUE(buf) << "OOM at alignment=" << alignment
                         << ", size=" << size << ", repeat=" << repeat;
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buf.get()) % alignment, 0u)
            << "alignment=" << alignment << ", size=" << size
            << ", repeat=" << repeat;

        ++n_checks;
      }
    }

    EXPECT_GE(n_checks, 64u) << "alignment=" << alignment;
  }
}

}  // namespace innodb_ddl0ddl_unittest
