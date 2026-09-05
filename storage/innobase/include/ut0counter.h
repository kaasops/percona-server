/*****************************************************************************

Copyright (c) 2012, 2025, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/ut0counter.h

 Counter utility class

 Created 2012/04/12 by Sunny Bains
 *******************************************************/

#ifndef ut0counter_h
#define ut0counter_h

#include <my_rdtsc.h>

#include "univ.i"

#include "os0numa.h" /* os_getcpu(), HAVE_OS_GETCPU */
#include "os0thread.h"
#include "ut0cpu_cache.h"
#include "ut0dbg.h"

#include <array>
#include <atomic>
#include <functional>

/** Default number of slots to use in ib_counter_t */
constexpr uint32_t IB_N_SLOTS = 64;

/** Number of slots for counters indexed by the CPU running the thread. Sized
so that every logical CPU of the largest machine we run on keeps a slot of its
own: two sockets and 128 CPUs today, four sockets later. Oracle sizes the
lock_sys shard mutexes by the same reasoning (lock_sys_t::SHARDS_COUNT). Slots
cost a cache line each, so the headroom is paid in kilobytes, while a shared
slot costs coherency traffic on every increment. */
constexpr uint32_t IB_N_CPU_SLOTS = 512;

/** Get the offset into the counter array. */
template <typename Type, int N>
struct generic_indexer_t {
  /** Default constructor/destructor should be OK. */

  /** @return offset within m_counter */
  static size_t offset(size_t index) UNIV_NOTHROW {
    return (((index % N) + 1) * (ut::INNODB_CACHE_LINE_SIZE / sizeof(Type)));
  }
};

/** Use the result of my_timer_cycles(), which mainly uses RDTSC for cycles,
to index into the counter array. See the comments for my_timer_cycles() */
template <typename Type = ulint, int N = 1>
struct counter_indexer_t : public generic_indexer_t<Type, N> {
  /** Default constructor/destructor should be OK. */

  enum { fast = 1 };

  /** @return result from RDTSC or similar functions. */
  static size_t get_rnd_index() UNIV_NOTHROW {
    size_t c = static_cast<size_t>(my_timer_cycles());

    if (c != 0) {
      return (c);
    } else {
      /* We may go here if my_timer_cycles() returns 0,
      so we have to have the plan B for the counter. */
#if !defined(_WIN32)
      return ut::this_thread_hash;
#else
      LARGE_INTEGER cnt;
      QueryPerformanceCounter(&cnt);

      return static_cast<size_t>(cnt.QuadPart);
#endif /* !_WIN32 */
    }
  }
};

/** For counters where N=1 */
template <typename Type = ulint, int N = 1>
struct single_indexer_t {
  /** Default constructor/destructor should are OK. */

  enum { fast = 0 };

  /** @return offset within m_counter */
  static size_t offset(size_t index [[maybe_unused]]) UNIV_NOTHROW {
    static_assert(N == 1);
    return ((ut::INNODB_CACHE_LINE_SIZE / sizeof(Type)));
  }

  /** @return 1 */
  static size_t get_rnd_index() UNIV_NOTHROW {
    static_assert(N == 1);
    return (1);
  }
};

/** Use the id of the CPU running the current thread to index into the counter
array. Unlike counter_indexer_t, which derives the index from RDTSC, this keeps
one shard per CPU: threads on the same core share a shard instead of pulling
two different cache lines, and a shard is not dragged between cores. Accuracy
is not the point - a thread migrating between the read of the index and the
increment merely lands in a neighbouring shard, and readers sum all shards.

The type is declared unconditionally, and only its behaviour depends on
HAVE_OS_GETCPU: that macro is added by storage/innobase/innodb.cmake for
InnoDB's own targets, while headers declaring counters of this type (btr0cur.h)
are also included by utilities built without those definitions. Guarding the
declaration itself would make the type vanish there. */
template <typename Type = ulint, int N = 1>
struct cpu_indexer_t : public generic_indexer_t<Type, N> {
  /** Default constructor/destructor should be OK. */

  enum { fast = 1 };

  /** @return the number of the CPU executing the current thread, or a
  per-thread hash where the platform cannot report it. */
  static size_t get_rnd_index() UNIV_NOTHROW {
#ifdef HAVE_OS_GETCPU
    return static_cast<size_t>(os_getcpu());
#else
    return ut::this_thread_hash;
#endif /* HAVE_OS_GETCPU */
  }
};

#define default_indexer_t counter_indexer_t

/** Class for using fuzzy counters. The counter is not protected by any
mutex and the results are not guaranteed to be 100% accurate but close
enough. Creates an array of counters and separates each element by the
ut::INNODB_CACHE_LINE_SIZE bytes */
template <typename Type, int N = IB_N_SLOTS,
          template <typename, int> class Indexer = default_indexer_t>
class ib_counter_t {
 public:
  ib_counter_t() { memset(m_counter, 0x0, sizeof(m_counter)); }

  ~ib_counter_t() { ut_ad(validate()); }

  static bool is_fast() { return (Indexer<Type, N>::fast); }

  bool validate() UNIV_NOTHROW {
#ifdef UNIV_DEBUG
    size_t n = (ut::INNODB_CACHE_LINE_SIZE / sizeof(Type));

    /* Check that we aren't writing outside our defined bounds. */
    for (size_t i = 0; i < UT_ARR_SIZE(m_counter); i += n) {
      for (size_t j = 1; j < n - 1; ++j) {
        ut_ad(m_counter[i + j] == 0);
      }
    }
#endif /* UNIV_DEBUG */
    return (true);
  }

  /** If you can't use a good index id. Increment by 1. */
  void inc() UNIV_NOTHROW { add(1); }

  /** If you can't use a good index id.
  @param n is the amount to increment */
  void add(Type n) UNIV_NOTHROW {
    size_t i = m_policy.offset(m_policy.get_rnd_index());

    ut_ad(i < UT_ARR_SIZE(m_counter));

    m_counter[i] += n;
  }

  /** Use this if you can use a unique identifier, saves a
  call to get_rnd_index().
  @param        index   index into a slot
  @param        n       amount to increment */
  void add(size_t index, Type n) UNIV_NOTHROW {
    size_t i = m_policy.offset(index);

    ut_ad(i < UT_ARR_SIZE(m_counter));

    m_counter[i] += n;
  }

  /** If you can't use a good index id. Decrement by 1. */
  void dec() UNIV_NOTHROW { sub(1); }

  /** If you can't use a good index id.
  @param n the amount to decrement */
  void sub(Type n) UNIV_NOTHROW {
    size_t i = m_policy.offset(m_policy.get_rnd_index());

    ut_ad(i < UT_ARR_SIZE(m_counter));

    m_counter[i] -= n;
  }

  /** Use this if you can use a unique identifier, saves a
  call to get_rnd_index().
  @param        index   index into a slot
  @param        n       amount to decrement */
  void sub(size_t index, Type n) UNIV_NOTHROW {
    size_t i = m_policy.offset(index);

    ut_ad(i < UT_ARR_SIZE(m_counter));

    m_counter[i] -= n;
  }

  /* @return total value - not 100% accurate, since it is not atomic. */
  operator Type() const UNIV_NOTHROW {
    Type total = 0;

    for (size_t i = 0; i < N; ++i) {
      total += m_counter[m_policy.offset(i)];
    }

    return (total);
  }

  Type operator[](size_t index) const UNIV_NOTHROW {
    size_t i = m_policy.offset(index);

    ut_ad(i < UT_ARR_SIZE(m_counter));

    return (m_counter[i]);
  }

 private:
  /** Indexer into the array */
  Indexer<Type, N> m_policy;

  /** Slot 0 is unused. */
  Type m_counter[(N + 1) * (ut::INNODB_CACHE_LINE_SIZE / sizeof(Type))];
};

/** Sharded atomic counter. */
namespace Counter {

using Type = uint64_t;

using N = std::atomic<Type>;

static_assert(ut::INNODB_CACHE_LINE_SIZE >= sizeof(N),
              "Atomic counter size > ut::INNODB_CACHE_LINE_SIZE");

using Pad = byte[ut::INNODB_CACHE_LINE_SIZE - sizeof(N)];

/** Counter shard. */
struct Shard {
  /** Separate on cache line. */
  Pad m_pad;

  /** Sharded counter. */
  N m_n{};
};

using Function = std::function<void(const Type)>;

/** Relaxed order by default. */
constexpr auto Memory_order = std::memory_order_relaxed;

/* Memory order is a template parameter, not a runtime field. That makes the
order part of the counter's type: instances with different orders cannot be
mixed in copy()/add() by accident - the compiler rejects it, where the runtime
field allowed copy() to silently combine a source and a destination that
disagreed on order. It also removes what the runtime field actually cost,
measured on the built server (GCC 14.2, -O2, LTO, x86-64 - the only production
target): the emitted strength of a store no longer depends on what the
compiler picks for a non-constant order (GCC emitted seq_cst XCHG where a
compile-time relaxed store produces a plain MOV - clear()/copy()), and the
4-byte field cost 8 bytes per instance after padding (sizeof(Shards<512>):
32776 with it, 32768 without). Loads and RMW ops compile identically either
way on x86-64, so the hot path neither gains nor loses. For relaxed
instantiations the store in clear()/copy() also weakens from the seq_cst the
runtime field could pick to a compile-time relaxed store, which is safe at
every call site: it either runs before the counter is shared across threads
(happens-before via thread creation) or only ever fed an approximately-correct
monitoring statistic that never carried a synchronization contract. Revisit if
the build matrix grows to include another compiler or a weaker memory model.
Usage in the tree: exactly one instance (mtr_t::Logging) needs a non-default
order, fixed in its type in mtr0mtr.h; under the runtime field that order was
set exactly once, in Logging::init(), at server startup - the flexibility was
never used. All other instantiations keep the relaxed default and never
change it. */
template <size_t COUNT = 128, std::memory_order M_ORDER = Memory_order>
struct Shards {
  /* Shard array. */
  std::array<Shard, COUNT> m_arr{};
};

/** Increment the counter for a shard by n.
@param[in,out]  shards          Sharded counter to increment.
@param[in] id                   Shard key.
@param[in] n                    Number to add.
@return previous value. */
template <size_t COUNT, std::memory_order M_ORDER>
inline Type add(Shards<COUNT, M_ORDER> &shards, size_t id, size_t n) {
  auto &shard_arr = shards.m_arr;

  return (shard_arr[id % shard_arr.size()].m_n.fetch_add(n, M_ORDER));
}

/** Decrement the counter for a shard by n.
@param[in,out]  shards          Sharded counter to increment.
@param[in] id                   Shard key.
@param[in] n                    Number to add.
@return previous value. */
template <size_t COUNT, std::memory_order M_ORDER>
inline Type sub(Shards<COUNT, M_ORDER> &shards, size_t id, size_t n) {
  ut_ad(get(shards, id) >= n);

  auto &shard_arr = shards.m_arr;
  return (shard_arr[id % shard_arr.size()].m_n.fetch_sub(n, M_ORDER));
}

/** Increment the counter of a shard by 1.
@param[in,out]  shards          Sharded counter to increment.
@param[in] id                   Shard key.
@return previous value. */
template <size_t COUNT, std::memory_order M_ORDER>
inline Type inc(Shards<COUNT, M_ORDER> &shards, size_t id) {
  return (add(shards, id, 1));
}

/** Increment the shard belonging to the CPU that runs this thread. Use this
where the counter is a plain event count and the key would otherwise come from
the object being counted: keying by the object walks the whole shard array as
the objects change, while keying by the CPU keeps each CPU on one cache line.
Accuracy is unaffected - readers sum all shards.
@param[in,out]  shards          Sharded counter to increment.
@return previous value. */
template <size_t COUNT, std::memory_order M_ORDER>
inline Type inc_by_cpu(Shards<COUNT, M_ORDER> &shards) {
  return (add(shards, cpu_indexer_t<>::get_rnd_index(), 1));
}

/** Decrement the counter of a shard by 1.
@param[in,out]  shards          Sharded counter to decrement.
@param[in] id                   Shard key.
@return previous value. */
template <size_t COUNT, std::memory_order M_ORDER>
inline Type dec(Shards<COUNT, M_ORDER> &shards, size_t id) {
  return (sub(shards, id, 1));
}

/** Get the counter value for a shard.
@param[in,out]  shards          Sharded counter to increment.
@param[in] id                   Shard key.
@return current value. */
template <size_t COUNT, std::memory_order M_ORDER>
inline Type get(const Shards<COUNT, M_ORDER> &shards, size_t id) noexcept {
  auto &shard_arr = shards.m_arr;

  return (shard_arr[id % shard_arr.size()].m_n.load(M_ORDER));
}

/** Iterate over the shards.
@param[in] shards               Shards to iterate over
@param[in] f                    Callback function
*/
template <size_t COUNT, std::memory_order M_ORDER>
inline void for_each(const Shards<COUNT, M_ORDER> &shards,
                     Function &&f) noexcept {
  for (const auto &shard : shards.m_arr) {
    f(shard.m_n);
  }
}

/** Get the total value of all shards.
@param[in] shards               Shards to sum.
@return total value. */
template <size_t COUNT, std::memory_order M_ORDER>
inline Type total(const Shards<COUNT, M_ORDER> &shards) noexcept {
  Type n = 0;

  for_each(shards, [&](const Type count) { n += count; });

  return (n);
}

/** Clear the counter - reset to 0.
@param[in,out] shards          Shards to clear. */
template <size_t COUNT, std::memory_order M_ORDER>
inline void clear(Shards<COUNT, M_ORDER> &shards) noexcept {
  for (auto &shard : shards.m_arr) {
    shard.m_n.store(0, M_ORDER);
  }
}

/** Copy the counters, overwrite destination.
@param[in,out] dst              Destination shard
@param[in]     src              Source shard. */
template <size_t COUNT, std::memory_order M_ORDER>
inline void copy(Shards<COUNT, M_ORDER> &dst,
                 const Shards<COUNT, M_ORDER> &src) noexcept {
  size_t i{0};
  for_each(src,
           [&](const Type count) { dst.m_arr[i++].m_n.store(count, M_ORDER); });
}

/** Accumulate the counters, add source to destination.
@param[in,out] dst              Destination shard
@param[in]     src              Source shard. */
template <size_t COUNT, std::memory_order M_ORDER>
inline void add(Shards<COUNT, M_ORDER> &dst,
                const Shards<COUNT, M_ORDER> &src) noexcept {
  size_t i{0};
  for_each(src, [&](const Type count) {
    dst.m_arr[i++].m_n.fetch_add(count, M_ORDER);
  });
}
}  // namespace Counter

#endif /* ut0counter_h */
