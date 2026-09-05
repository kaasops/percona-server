/* Copyright (c) 2021, 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* See http://code.google.com/p/googletest/wiki/Primer */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

#include "os0thread.h"
#include "sync0arr_impl.h"

namespace innodb_sync0rw_unittest {

struct phase_task {
  size_t thread_idx;
  std::function<void(void)> action;
  bool blocking{true};
};
/** It runs a machinery to execute the phases supplied. It creates
 threads that move through phases to perform tasks needed to complete the
 algorithm specified by these phases.*/
void execute_multithreaded_phase_plan(const std::vector<phase_task> phases[],
                                      const size_t phase_count) {
  std::vector<std::atomic<size_t>> completed_tasks_in_phase(phase_count);
  std::vector<std::thread> threads;
  std::atomic<size_t> current_phase{0};
  size_t max_thread_idx_used = 0;

  /* Scan phase list to search for max thread index used to create that many
   threads. */
  for (size_t p = 0; p < phase_count; ++p) {
    completed_tasks_in_phase[p] = 0;
    for (const auto &t : phases[p]) {
      max_thread_idx_used = std::max(max_thread_idx_used, t.thread_idx);
    }
  }

  /* Specifies actions taken by a single thread to be created, depending on the
   thread index. It waits for each phase to be executed in chronological
   order, and for each phase it executes all tasks that are assigned for the
   thread index, as specified by the phases algorithm supplied. */
  auto thread_action = [&](auto my_thread_idx) {
    /* Run phases one by one in chronological order. */
    for (size_t p = 0; p < phase_count; ++p) {
      /* Wait for a specified phase to start. */
      while (current_phase < p) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      /* Find all tasks in this phase that are assigned for current thread
       index. */
      for (const auto &t : phases[p]) {
        if (t.thread_idx == my_thread_idx) {
          /* If the action is blocking, execute it first and only then
           allow to move the phases forward. */
          if (t.blocking) t.action();
          /* Move phases forward - set this task as executed. */
          if (++completed_tasks_in_phase[p] == phases[p].size()) {
            /* If this task was last in the phase to execute, then move to
             the next phase. */
            current_phase++;
          }
          /* If the task was meant to not be blocking, execute it after
           moving phases forward. Note, that tasks in next phase may
           want to wait a bit for this thread to do its work. As this is
           blocking, there is no way to synchronize and tell other when
           to move on. */
          if (!t.blocking) t.action();
        }
      }
    }
  };
  /* Creates threads that execute the algorithm. */
  for (size_t thread_idx = 0; thread_idx <= max_thread_idx_used; ++thread_idx) {
    threads.emplace_back(thread_action, thread_idx);
  }
  /* Cleanup */
  for (auto &t : threads) {
    t.join();
  }
  EXPECT_EQ(current_phase.load(), phase_count);
}

TEST(sync0rw, rw_lock_reader_thread) {
  /* This test tests if the reader_thread is calculated correctly.
  We have three rw_locks:
  - lock 0 is S-latched by 3 threads in this order: t2, t1, t3. Then t2
  and t3 unlocks it, so only t1 have it locked.
  - lock 1 is S-latched by t1 and t2.
  - lock 2 is only X-latched by t1.
  Now we try to  X-latch locks 0, 1 and 2 with threads t2, t3 and t4.
  They will be waiting on rw_locks with different number of readers: 1, 2 and 0,
  respectively.
  We print cell info and let t1 to continue, which in turn lets all threads to
  finish. */

  os_event_global_init();
  sync_check_init(4);

  rw_lock_t *rw_locks[3];
  for (auto &rw_lock : rw_locks) {
    rw_lock = static_cast<rw_lock_t *>(malloc(sizeof(rw_lock_t)));
    rw_lock_create(PSI_NOT_INSTRUMENTED, rw_lock, LATCH_ID_BUF_BLOCK_LOCK);
  }

  std::atomic<std::thread::id> thread_1_id;

  auto check_reader_counts_action = [&] {
    /* Let all threads place their X-lock waits. */
    std::this_thread::sleep_for(std::chrono::seconds(2));

    EXPECT_EQ(rw_lock_get_reader_count(rw_locks[0]), 1);
    EXPECT_EQ(rw_lock_get_reader_count(rw_locks[1]), 2);
    EXPECT_EQ(rw_lock_get_reader_count(rw_locks[2]), 0);
#ifdef UNIV_DEBUG
    EXPECT_EQ(rw_locks[0]->reader_thread.recover_if_single(),
              thread_1_id.load());
#endif /* UNIV_DEBUG */

    FILE *tmp_file = tmpfile();
    EXPECT_NE(tmp_file, nullptr);

    for (size_t i = 0; i < sync_array_size; ++i) {
      sync_array_t *arr = sync_wait_array[i];

      mutex_enter(&arr->mutex);

      for (size_t j = 0; j < arr->next_free_slot; j++) {
        sync_cell_t &cell = arr->cells[j];

        void *latch = cell.latch.mutex;

        if (latch == nullptr || !cell.waiting) {
          continue;
        }

        sync_array_cell_print(tmp_file, &cell);
      }

      mutex_exit(&arr->mutex);
    }

    /* Print cells to temporary file, read all content and check it is correct.
     */
    const auto len = ftell(tmp_file);
    EXPECT_NE(-1, len);
    auto content = std::make_unique<char[]>(len);
    EXPECT_EQ(0, fseek(tmp_file, 0, SEEK_SET));
    EXPECT_EQ(len, fread(content.get(), 1, len, tmp_file));
    EXPECT_EQ(0, fclose(tmp_file));
    std::string cell_print(content.get(), len);

    EXPECT_THAT(cell_print, testing::HasSubstr("number of readers 0, waiters"));
    /* Note that std::to_string(thread_id_to_uint64(thread_id) could be
     something different than to_string(thread_id). */
#ifdef UNIV_DEBUG
    EXPECT_THAT(cell_print, testing::HasSubstr(
                                "number of readers 1 (thread id " +
                                to_string(thread_1_id.load()) + "), waiters"));
#else
    EXPECT_THAT(cell_print, testing::HasSubstr("number of readers 1, waiters"));
#endif /* UNIV_DEBUG */
    EXPECT_THAT(cell_print, testing::HasSubstr("number of readers 2, waiters"));
  };

  /* We proceed the test in the following phases: */
  std::vector<phase_task> phases[] = {
      /* Place all required S-latches and X-latches for locks 1 and 2.
      Place first S-latch for lock 0 (in thread 2) - other threads will
      place their lock 1 latches in next phases to synchronize order of
      S-latching.
    */
      {
          phase_task{1, [&] { thread_1_id = std::this_thread::get_id(); }},
          phase_task{1, [&] { rw_lock_s_lock(rw_locks[1], UT_LOCATION_HERE); }},
          phase_task{1, [&] { rw_lock_x_lock(rw_locks[2], UT_LOCATION_HERE); }},
          phase_task{2, [&] { rw_lock_s_lock(rw_locks[0], UT_LOCATION_HERE); }},
          phase_task{2, [&] { rw_lock_s_lock(rw_locks[1], UT_LOCATION_HERE); }},
      },
      /* Place second S-latch on lock 0, now from thread 1. */
      {
          phase_task{1, [&] { rw_lock_s_lock(rw_locks[0], UT_LOCATION_HERE); }},
      },
      /* Place third S-latch on lock 0, now from thread 3. */
      {
          phase_task{3, [&] { rw_lock_s_lock(rw_locks[0], UT_LOCATION_HERE); }},
      },
      /* Unlatch S-latches on lock 0 from threads 2 and 3. */
      {
          phase_task{2, [&] { rw_lock_s_unlock(rw_locks[0]); }},
          phase_task{3, [&] { rw_lock_s_unlock(rw_locks[0]); }},
      },
      /* Now place all blocking X-latches from threads 2, 3 and 4. */
      {
          phase_task{2, [&] { rw_lock_x_lock(rw_locks[0], UT_LOCATION_HERE); },
                     false},
          phase_task{3, [&] { rw_lock_x_lock(rw_locks[1], UT_LOCATION_HERE); },
                     false},
          phase_task{4, [&] { rw_lock_x_lock(rw_locks[2], UT_LOCATION_HERE); },
                     false},
      },
      /* Now run the rw_locks check. */
      {
          phase_task{0, check_reader_counts_action},
      },
      /* Unlock all remaining latches. */
      {
          phase_task{1, [&] { rw_lock_s_unlock(rw_locks[0]); }},
          phase_task{1, [&] { rw_lock_s_unlock(rw_locks[1]); }},

          /* Unlocking thread 1, it will allow next threads to be able to
          get their X-latches and then release them all. */
          phase_task{1, [&] { rw_lock_x_unlock(rw_locks[2]); }},
          phase_task{2, [&] { rw_lock_s_unlock(rw_locks[1]); }},
          phase_task{2, [&] { rw_lock_x_unlock(rw_locks[0]); }},
          phase_task{3, [&] { rw_lock_x_unlock(rw_locks[1]); }},
          phase_task{4, [&] { rw_lock_x_unlock(rw_locks[2]); }},
      }};

  execute_multithreaded_phase_plan(phases, UT_ARR_SIZE(phases));

  for (auto &rw_lock : rw_locks) {
    rw_lock_free(rw_lock);
    free(rw_lock);
  }

  sync_check_close();
  os_event_global_destroy();
}

TEST(sync0rw, rw_lock_word_accounting) {
  /* The lock_word accessors (rw_lock_get_writer, rw_lock_get_reader_count,
  rw_lock_get_x_lock_count) decode a single int32_t into the S/X/SX state, and
  the recursive x-lock and x-unlock paths walk an if/else chain over that same
  word. This test pins down what each accessor reports for every state a lock
  can be driven into by a single owning thread, so that changes to the types
  used for those snapshots, or to how often the word is read, have to keep the
  reported state identical. */

  os_event_global_init();
  sync_check_init(1);

  auto *lock = static_cast<rw_lock_t *>(malloc(sizeof(rw_lock_t)));
  rw_lock_create(PSI_NOT_INSTRUMENTED, lock, LATCH_ID_BUF_BLOCK_LOCK);

  auto expect_state = [&](ulint writer, ulint readers, ulint x_locks) {
    EXPECT_EQ(rw_lock_get_writer(lock), writer);
    EXPECT_EQ(rw_lock_get_reader_count(lock), readers);
    EXPECT_EQ(rw_lock_get_x_lock_count(lock), x_locks);
  };

  /* Readers are taken and released through the lock_word primitives rather
  than rw_lock_s_lock(), because a single thread is not allowed to hold two
  S-locks on the same latch (sync debug asserts on it) while the counting
  under test is precisely about several readers being present at once. */
  auto add_reader = [&]() { EXPECT_TRUE(rw_lock_lock_word_decr(lock, 1, 0)); };
  auto remove_reader = [&]() { rw_lock_lock_word_incr(lock, 1); };

  /* A free lock. */
  expect_state(RW_LOCK_NOT_LOCKED, 0, 0);

  /* S-locks are counted, and do not make the lock look written to. */
  for (ulint readers = 1; readers <= 3; ++readers) {
    add_reader();
    expect_state(RW_LOCK_NOT_LOCKED, readers, 0);
  }
  for (ulint readers = 3; readers > 0; --readers) {
    expect_state(RW_LOCK_NOT_LOCKED, readers, 0);
    remove_reader();
  }
  expect_state(RW_LOCK_NOT_LOCKED, 0, 0);

  /* A single real S-lock goes through the same accounting. */
  rw_lock_s_lock(lock, UT_LOCATION_HERE);
  expect_state(RW_LOCK_NOT_LOCKED, 1, 0);
  rw_lock_s_unlock(lock);
  expect_state(RW_LOCK_NOT_LOCKED, 0, 0);

  /* Recursive x-locks. The first one zeroes lock_word, the second subtracts a
  whole X_LOCK_DECR, every further one subtracts 1 - three different branches
  of the same chain. */
  for (ulint x_locks = 1; x_locks <= 3; ++x_locks) {
    rw_lock_x_lock(lock, UT_LOCATION_HERE);
    expect_state(RW_LOCK_X, 0, x_locks);
  }
  for (ulint x_locks = 3; x_locks > 0; --x_locks) {
    expect_state(RW_LOCK_X, 0, x_locks);
    rw_lock_x_unlock(lock);
  }
  expect_state(RW_LOCK_NOT_LOCKED, 0, 0);

  /* The same recursion through the nowait path, which has its own copy of the
  relock chain. */
  for (ulint x_locks = 1; x_locks <= 3; ++x_locks) {
    EXPECT_TRUE(rw_lock_x_lock_nowait(lock, UT_LOCATION_HERE));
    expect_state(RW_LOCK_X, 0, x_locks);
  }
  for (ulint x_locks = 3; x_locks > 0; --x_locks) {
    rw_lock_x_unlock(lock);
  }
  expect_state(RW_LOCK_NOT_LOCKED, 0, 0);

  /* SX is reported as a writer state of its own, but does not exclude
  readers. */
  rw_lock_sx_lock(lock, UT_LOCATION_HERE);
  expect_state(RW_LOCK_SX, 0, 0);

  add_reader();
  add_reader();
  expect_state(RW_LOCK_SX, 2, 0);
  remove_reader();
  remove_reader();
  expect_state(RW_LOCK_SX, 0, 0);

  /* Upgrading our own SX to X and back: this drives lock_word to the
  -X_LOCK_HALF_DECR value, which the chains treat as "one x-lock" but which is
  a different constant than the plain single-x-lock case above. */
  rw_lock_x_lock(lock, UT_LOCATION_HERE);
  expect_state(RW_LOCK_X, 0, 1);
  rw_lock_x_unlock(lock);
  expect_state(RW_LOCK_SX, 0, 0);

  rw_lock_sx_unlock(lock);
  expect_state(RW_LOCK_NOT_LOCKED, 0, 0);

  rw_lock_free(lock);
  free(lock);

  sync_check_close();
  os_event_global_destroy();
}

#ifdef UNIV_PFS_RWLOCK
/* The two tests below exist only under UNIV_PFS_RWLOCK: the _pfsni family
is built to bypass the PFS dispatch layer, and without UNIV_PFS_RWLOCK
(e.g. built with -DDISABLE_PSI_RWLOCK=ON) the ordinary rw_lock_*() macros
already take the same non-instrumented path the _pfsni functions take -
there would be nothing left for a parity check to tell apart. Do not
"simplify" this away in a future port: it is deliberate, not an oversight.
*/

TEST(sync0rw, rw_lock_pfsni_state_parity) {
  /* Two identically-created locks: lock_a is driven exclusively through the
  ordinary rw_lock_*() macros, lock_b through their _pfsni counterparts.
  Every publicly observable piece of state is compared after each step, so
  any behavioral divergence between the two families shows up immediately.
  */

  os_event_global_init();
  sync_check_init(1);

  auto *lock_a = static_cast<rw_lock_t *>(malloc(sizeof(rw_lock_t)));
  auto *lock_b = static_cast<rw_lock_t *>(malloc(sizeof(rw_lock_t)));
  rw_lock_create(PSI_NOT_INSTRUMENTED, lock_a, LATCH_ID_BUF_BLOCK_LOCK);
  rw_lock_create(PSI_NOT_INSTRUMENTED, lock_b, LATCH_ID_BUF_BLOCK_LOCK);

  auto expect_parity = [&] {
    EXPECT_EQ(lock_a->lock_word.load(), lock_b->lock_word.load());
    EXPECT_EQ(rw_lock_get_reader_count(lock_a),
              rw_lock_get_reader_count(lock_b));
    EXPECT_EQ(rw_lock_get_writer(lock_a), rw_lock_get_writer(lock_b));
    EXPECT_EQ(rw_lock_get_sx_lock_count(lock_a),
              rw_lock_get_sx_lock_count(lock_b));
#ifdef UNIV_DEBUG
    /* rw_lock_own()/rw_lock_own_flagged() are declared only under
    UNIV_DEBUG (sync0rw.h), so this comparison has to be compiled out in
    release builds rather than merely skipped at runtime. */
    EXPECT_EQ(rw_lock_own(lock_a, RW_LOCK_S), rw_lock_own(lock_b, RW_LOCK_S));
    EXPECT_EQ(rw_lock_own(lock_a, RW_LOCK_X), rw_lock_own(lock_b, RW_LOCK_X));
    EXPECT_EQ(rw_lock_own(lock_a, RW_LOCK_SX), rw_lock_own(lock_b, RW_LOCK_SX));
    EXPECT_EQ(rw_lock_own_flagged(lock_a, RW_LOCK_FLAG_S),
              rw_lock_own_flagged(lock_b, RW_LOCK_FLAG_S));
    EXPECT_EQ(rw_lock_own_flagged(lock_a, RW_LOCK_FLAG_X),
              rw_lock_own_flagged(lock_b, RW_LOCK_FLAG_X));
    EXPECT_EQ(rw_lock_own_flagged(lock_a, RW_LOCK_FLAG_SX),
              rw_lock_own_flagged(lock_b, RW_LOCK_FLAG_SX));
    EXPECT_EQ(rw_lock_own_flagged(
                  lock_a, RW_LOCK_FLAG_S | RW_LOCK_FLAG_X | RW_LOCK_FLAG_SX),
              rw_lock_own_flagged(
                  lock_b, RW_LOCK_FLAG_S | RW_LOCK_FLAG_X | RW_LOCK_FLAG_SX));
#endif /* UNIV_DEBUG */
  };

  expect_parity();

  /* S lock/unlock, plain form. */
  rw_lock_s_lock(lock_a, UT_LOCATION_HERE);
  rw_lock_s_lock_pfsni(lock_b, UT_LOCATION_HERE);
  expect_parity();
  rw_lock_s_unlock(lock_a);
  rw_lock_s_unlock_pfsni(lock_b);
  expect_parity();

  /* S lock/unlock, _gen form. */
  rw_lock_s_lock_gen(lock_a, 0, UT_LOCATION_HERE);
  rw_lock_s_lock_gen_pfsni(lock_b, 0, UT_LOCATION_HERE);
  expect_parity();
  rw_lock_s_unlock_gen(lock_a, 0);
  rw_lock_s_unlock_gen_pfsni(lock_b, 0);
  expect_parity();

  /* S _nowait lock (single owner, so it is expected to succeed), plain
  unlock. Not repeated on the same lock without an intervening unlock: a
  recursive S-lock on one latch trips the sync-debug recursion assert (see
  rw_lock_word_accounting above). */
  bool s_nowait_a = rw_lock_s_lock_nowait(lock_a, UT_LOCATION_HERE);
  bool s_nowait_b = rw_lock_s_lock_nowait_pfsni(lock_b, UT_LOCATION_HERE);
  EXPECT_EQ(s_nowait_a, s_nowait_b);
  EXPECT_TRUE(s_nowait_a);
  expect_parity();
  rw_lock_s_unlock(lock_a);
  rw_lock_s_unlock_pfsni(lock_b);
  expect_parity();

  /* SX lock/unlock, plain form. */
  rw_lock_sx_lock(lock_a, UT_LOCATION_HERE);
  rw_lock_sx_lock_pfsni(lock_b, UT_LOCATION_HERE);
  expect_parity();
  rw_lock_sx_unlock(lock_a);
  rw_lock_sx_unlock_pfsni(lock_b);
  expect_parity();

  /* SX lock/unlock, _gen form. */
  rw_lock_sx_lock_gen(lock_a, 0, UT_LOCATION_HERE);
  rw_lock_sx_lock_gen_pfsni(lock_b, 0, UT_LOCATION_HERE);
  expect_parity();
  rw_lock_sx_unlock_gen(lock_a, 0);
  rw_lock_sx_unlock_gen_pfsni(lock_b, 0);
  expect_parity();

  /* SX _nowait lock, plain unlock. */
  bool sx_nowait_a = rw_lock_sx_lock_nowait(lock_a, 0, UT_LOCATION_HERE);
  bool sx_nowait_b = rw_lock_sx_lock_nowait_pfsni(lock_b, 0, UT_LOCATION_HERE);
  EXPECT_EQ(sx_nowait_a, sx_nowait_b);
  EXPECT_TRUE(sx_nowait_a);
  expect_parity();
  rw_lock_sx_unlock(lock_a);
  rw_lock_sx_unlock_pfsni(lock_b);
  expect_parity();

  /* X lock/unlock, plain form. */
  rw_lock_x_lock(lock_a, UT_LOCATION_HERE);
  rw_lock_x_lock_pfsni(lock_b, UT_LOCATION_HERE);
  expect_parity();
  rw_lock_x_unlock(lock_a);
  rw_lock_x_unlock_pfsni(lock_b);
  expect_parity();

  /* X lock/unlock, _gen form. */
  rw_lock_x_lock_gen(lock_a, 0, UT_LOCATION_HERE);
  rw_lock_x_lock_gen_pfsni(lock_b, 0, UT_LOCATION_HERE);
  expect_parity();
  rw_lock_x_unlock_gen(lock_a, 0);
  rw_lock_x_unlock_gen_pfsni(lock_b, 0);
  expect_parity();

  /* X _nowait lock, plain unlock. */
  bool x_nowait_a = rw_lock_x_lock_nowait(lock_a, UT_LOCATION_HERE);
  bool x_nowait_b = rw_lock_x_lock_nowait_pfsni(lock_b, UT_LOCATION_HERE);
  EXPECT_EQ(x_nowait_a, x_nowait_b);
  EXPECT_TRUE(x_nowait_a);
  expect_parity();
  rw_lock_x_unlock(lock_a);
  rw_lock_x_unlock_pfsni(lock_b);
  expect_parity();

  rw_lock_free(lock_a);
  rw_lock_free(lock_b);
  free(lock_a);
  free(lock_b);

  sync_check_close();
  os_event_global_destroy();
}

TEST(sync0rw, rw_lock_pfsni_concurrency) {
  /* Drives a single lock through S/X/SX states using only the _pfsni
  family, across three threads, and checks that the nowait variants report
  the compatibility rules correctly at each transition:
  - two readers (t1, t2) block a nowait X attempt (t3);
  - after the readers release, an X holder (t3) blocks a nowait S attempt
    (t1);
  - once t3 downgrades to SX, a nowait S attempt (t1) succeeds (SX admits
    readers) while a nowait X attempt (t2) still fails (SX excludes
    writers). */

  os_event_global_init();
  sync_check_init(3);

  auto *lock = static_cast<rw_lock_t *>(malloc(sizeof(rw_lock_t)));
  rw_lock_create(PSI_NOT_INSTRUMENTED, lock, LATCH_ID_BUF_BLOCK_LOCK);

  std::vector<phase_task> phases[] = {
      /* t1 and t2 both take S. */
      {
          phase_task{1, [&] { rw_lock_s_lock_pfsni(lock, UT_LOCATION_HERE); }},
          phase_task{2, [&] { rw_lock_s_lock_pfsni(lock, UT_LOCATION_HERE); }},
      },
      /* With two readers present, a nowait X attempt must fail. */
      {
          phase_task{1, [&] { EXPECT_EQ(rw_lock_get_reader_count(lock), 2); }},
          phase_task{3,
                     [&] {
                       EXPECT_FALSE(rw_lock_x_lock_nowait_pfsni(
                           lock, UT_LOCATION_HERE));
                     }},
      },
      /* Readers release. */
      {
          phase_task{1, [&] { rw_lock_s_unlock_pfsni(lock); }},
          phase_task{2, [&] { rw_lock_s_unlock_pfsni(lock); }},
      },
      /* t3 takes X (blocking task: completes before the next phase). */
      {
          phase_task{3, [&] { rw_lock_x_lock_pfsni(lock, UT_LOCATION_HERE); }},
      },
      /* With t3 holding X, a nowait S attempt must fail. */
      {
          phase_task{1,
                     [&] {
                       EXPECT_FALSE(rw_lock_s_lock_nowait_pfsni(
                           lock, UT_LOCATION_HERE));
                     }},
      },
      /* t3 downgrades X to SX. */
      {
          phase_task{3,
                     [&] {
                       rw_lock_x_unlock_pfsni(lock);
                       rw_lock_sx_lock_pfsni(lock, UT_LOCATION_HERE);
                     }},
      },
      /* SX admits readers: a nowait S attempt now succeeds (and actually
      takes the lock, released in the cleanup phase below). */
      {
          phase_task{1,
                     [&] {
                       EXPECT_TRUE(rw_lock_s_lock_nowait_pfsni(
                           lock, UT_LOCATION_HERE));
                     }},
      },
      /* SX excludes writers: a nowait X attempt still fails. */
      {
          phase_task{2,
                     [&] {
                       EXPECT_FALSE(rw_lock_x_lock_nowait_pfsni(
                           lock, UT_LOCATION_HERE));
                     }},
      },
      /* Cleanup: release t1's S and t3's SX. */
      {
          phase_task{1, [&] { rw_lock_s_unlock_pfsni(lock); }},
          phase_task{3, [&] { rw_lock_sx_unlock_pfsni(lock); }},
      }};

  execute_multithreaded_phase_plan(phases, UT_ARR_SIZE(phases));

  rw_lock_free(lock);
  free(lock);

  sync_check_close();
  os_event_global_destroy();
}

#endif /* UNIV_PFS_RWLOCK */

}  // namespace innodb_sync0rw_unittest
