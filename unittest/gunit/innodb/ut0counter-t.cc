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

#include <type_traits>

#include "storage/innobase/include/buf0buf.h"
#include "storage/innobase/include/mtr0mtr.h"
#include "storage/innobase/include/ut0counter.h"

namespace innodb_ut0counter_unittest {

/* Extracts the std::memory_order template parameter out of a Shards<>
instantiation regardless of its COUNT - lets the runtime contract tests
below run the same body once per configured order instead of duplicating it
per order. */
template <typename T>
struct Shards_order;

template <size_t COUNT, std::memory_order M_ORDER>
struct Shards_order<Counter::Shards<COUNT, M_ORDER>> {
  static constexpr std::memory_order value = M_ORDER;
};

template <typename T>
constexpr std::memory_order Shards_order_v = Shards_order<T>::value;

/* (a) mtr_t::Logging::Shards names its non-default order in its type, which
is only checkable because the alias is public (moved out of the private
section for exactly this reason). Both forms are asserted: is_same_v pins
down COUNT together with the order, the trait-based form isolates the order
on its own so it fails on an order regression even if COUNT also changed. */
static_assert(std::is_same_v<mtr_t::Logging::Shards,
                             Counter::Shards<128, std::memory_order_seq_cst>>);
static_assert(Shards_order_v<mtr_t::Logging::Shards> ==
              std::memory_order_seq_cst);

/* (b) buf_pool_stat_t::Shards keeps the relaxed default - nothing pins it
to a non-default order. */
static_assert(std::is_same_v<buf_pool_stat_t::Shards,
                             Counter::Shards<IB_N_CPU_SLOTS,
                                             Counter::Memory_order>>);
static_assert(Shards_order_v<buf_pool_stat_t::Shards> == Counter::Memory_order);

/* (c) Guards the runtime memory_order field staying gone: with it, this
would be IB_N_CPU_SLOTS * ut::INNODB_CACHE_LINE_SIZE + 8 (padded to the
struct's 8-byte alignment). This is a type-size fact, not a statement about
how many cache lines a live instance occupies in memory - the struct's own
base address is not guaranteed to be cache-line aligned. */
static_assert(sizeof(Counter::Shards<IB_N_CPU_SLOTS>) ==
              IB_N_CPU_SLOTS * ut::INNODB_CACHE_LINE_SIZE);

template <typename ShardsT>
class ShardsContract : public ::testing::Test {};

using OrderedShards =
    ::testing::Types<Counter::Shards<8>,
                     Counter::Shards<8, std::memory_order_seq_cst>>;
TYPED_TEST_SUITE(ShardsContract, OrderedShards);

TYPED_TEST(ShardsContract, add_get_total) {
  TypeParam shards;
  EXPECT_EQ(Counter::total(shards), 0U);

  EXPECT_EQ(Counter::add(shards, 0, 5), 0U);
  EXPECT_EQ(Counter::get(shards, 0), 5U);

  EXPECT_EQ(Counter::inc(shards, 1), 0U);
  EXPECT_EQ(Counter::get(shards, 1), 1U);

  EXPECT_EQ(Counter::total(shards), 6U);

  EXPECT_EQ(Counter::sub(shards, 0, 2), 5U);
  EXPECT_EQ(Counter::dec(shards, 1), 1U);
  EXPECT_EQ(Counter::total(shards), 3U);
}

TYPED_TEST(ShardsContract, clear) {
  TypeParam shards;
  Counter::add(shards, 0, 42);
  ASSERT_EQ(Counter::total(shards), 42U);

  Counter::clear(shards);
  EXPECT_EQ(Counter::total(shards), 0U);
}

TYPED_TEST(ShardsContract, copy_overwrites_destination) {
  TypeParam src;
  TypeParam dst;
  Counter::add(src, 0, 7);
  Counter::add(dst, 0, 100);

  Counter::copy(dst, src);
  EXPECT_EQ(Counter::total(dst), 7U);

  /* copy() overwrites, add() below must show accumulation instead. */
  Counter::add(dst, src);
  EXPECT_EQ(Counter::total(dst), 14U);
}

TYPED_TEST(ShardsContract, inc_by_cpu_is_counted_in_total) {
  TypeParam shards;
  Counter::inc_by_cpu(shards);
  Counter::inc_by_cpu(shards);
  EXPECT_EQ(Counter::total(shards), 2U);
}

}  // namespace innodb_ut0counter_unittest
