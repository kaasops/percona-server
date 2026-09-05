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

#include "storage/innobase/include/log0types.h"
#include "storage/innobase/include/ut0link_buf.h"

namespace innodb_ut0link_buf_unittest {

/* Tested against Link_buf<lsn_t> specifically, matching the only
production instantiation (log.recent_written/recent_closed in
log0buf.cc), not the class template's uint64_t default. */
using Position = lsn_t;
using Buf = Link_buf<Position>;

/* Exercises the owner-branch sweep added by this patch: previously the
owner branch (the call whose own position equals the current tail)
advanced the tail by exactly its own link; now it continues forward
through every already-published, contiguous link. Fails on the mutation
that reverts the sweep to a single store - see the comment below.

An earlier revision of this test also covered a sampled non-owner CAS
path (a second sub-assertion, "m2"), which INT02's own bench-gate
measured and rejected (see the invariant comment on
Link_buf::add_link_advance_tail()): the non-owner branch that ships is
the same unconditional, always-attempting form as HEAD, so there is
nothing sampling-specific left to test, and this test was simplified back
to a single, fully deterministic assertion. */
TEST(ut0link_buf, owner_branch_sweeps_contiguous_published_links) {
  static constexpr size_t CAPACITY = 1u << 14;
  static constexpr Position LEN_A = 5;
  static constexpr Position LEN_B = 7;

  Buf buf(CAPACITY);

  /* Setup: advance the tail as owner to a non-zero baseline, purely so
  the test does not start degenerate at position 0. */
  const Position round_start = 100;
  ASSERT_EQ(buf.tail(), Position{0});
  buf.add_link_advance_tail(0, round_start);
  ASSERT_EQ(buf.tail(), round_start);

  /* B publishes the link for the position that A's own (about to be
  made) link will end at - "the stuck link of the next position" - via
  the raw add_link() API, i.e. without going through any advance/sweep
  path. This is the deterministic equivalent of the "race + non-owner
  publish landed first" outcome: a link exists but nobody has swept up
  to it yet. */
  const Position p1 = round_start + LEN_A; /* end of link A (A's own link) */
  const Position p2 = p1 + LEN_B;          /* end of link B */
  buf.add_link(p1, p2);

  /* A now arrives as the owner (its own position equals the current
  tail) and publishes+advances its own link. If the sweep added by this
  patch works, it continues past its own link into B's already-published
  link, landing on p2. If the sweep were removed (regressing to the old
  behavior of only storing `to`), it would land on p1 instead. */
  buf.add_link_advance_tail(round_start, p1);
  ASSERT_EQ(buf.tail(), p2);
}

}  // namespace innodb_ut0link_buf_unittest
