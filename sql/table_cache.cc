/* Copyright (c) 2012, 2025, Oracle and/or its affiliates.

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

#include "sql/table_cache.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <new>  // placement new for segment construction in init()
#include <vector>

#include "my_compiler.h"
#include "my_config.h"  // CPU_PAGE_SIZE
#include "my_dbug.h"
#include "my_macros.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/my_loglevel.h"
#include "mysqld_error.h"
#include "sql/cpu_topology.h"
#include "sql/current_thd.h"
#include "sql/psi_memory_key.h"  // key_memory_table_cache_segment
#include "sql/sql_test.h"        // lock_descriptions[]
#include "storage/innobase/include/ut0page_alloc.h"
#include "thr_lock.h"
#include "thr_mutex.h"

#ifdef __linux__
#include <sched.h>
#endif

/**
  Container for all table cache instances in the system.
*/
Table_cache_manager table_cache_manager;

/* Each segment is placed on its own page-aligned allocation instead of
being embedded in Table_cache_manager::m_table_cache (a prerequisite for
the NUMA placement a later step adds), which only pays off memory-wise if
one Table_cache still fits the single data page the bridge allocator
reserves per segment - this guards that assumption at compile time rather
than letting it silently regress into a second page per segment. */
static_assert(sizeof(Table_cache) <= CPU_PAGE_SIZE,
              "Table_cache must fit in one CPU_PAGE_SIZE page - each "
              "instance is now allocated as its own page-aligned segment "
              "via ut_malloc_page_withkey().");

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key Table_cache::m_lock_key;
PSI_mutex_info Table_cache::m_mutex_keys[] = {
    {&m_lock_key, "LOCK_table_cache", 0, 0, PSI_DOCUMENT_ME}};
#endif

/**
  Initialize instance of table cache.

  @param index  This instance's slot index in
                Table_cache_manager::m_table_cache. @sa index().

  @retval false - success.
  @retval true  - failure.
*/

bool Table_cache::init(uint index) {
  m_index = index;
  mysql_mutex_init(m_lock_key, &m_lock, MY_MUTEX_INIT_FAST);
  m_unused_tables = nullptr;
  m_table_count = 0;
  m_table_triggers_count = 0;
  return false;
}

/** Destroy instance of table cache. */

void Table_cache::destroy() { mysql_mutex_destroy(&m_lock); }

/** Init P_S instrumentation key for mutex protecting Table_cache instance. */

void Table_cache::init_psi_keys() {
#ifdef HAVE_PSI_INTERFACE
  mysql_mutex_register("sql", m_mutex_keys,
                       static_cast<int>(array_elements(m_mutex_keys)));
#endif
}

#ifdef EXTRA_DEBUG
void Table_cache::check_unused() {
  uint count = 0;

  if (m_unused_tables != NULL) {
    TABLE *cur_link = m_unused_tables;
    TABLE *start_link = m_unused_tables;
    do {
      if (cur_link != cur_link->next->prev ||
          cur_link != cur_link->prev->next) {
        DBUG_PRINT("error", ("Unused_links aren't linked properly"));
        return;
      }
    } while (count++ < m_table_count &&
             (cur_link = cur_link->next) != start_link);
    if (cur_link != start_link)
      DBUG_PRINT("error", ("Unused_links aren't connected"));
  }

  for (const auto &hp : m_cache) {
    Table_cache_element *el = hp.second.get();

    auto check_free_tables =
        [&count](const Table_cache_element::TABLE_list &free_tables) {
          Table_cache_element::TABLE_list::Iterator it(free_tables);
          TABLE *entry;
          while ((entry = it++)) {
            /*
              We must not have TABLEs in the free list that have their file
              closed.
            */
            assert(entry->db_stat && entry->file);

            if (entry->in_use)
              DBUG_PRINT("error",
                         ("Used table is in share's list of unused tables"));
            count--;
          }
        };

    check_free_tables(el->free_tables_slim);
    check_free_tables(el->free_tables_full_triggers);

    Table_cache_element::TABLE_list::Iterator it(el->used_tables);
    TABLE *entry;
    while ((entry = it++)) {
      if (!entry->in_use)
        DBUG_PRINT("error", ("Unused table is in share's list of used tables"));
    }
  }

  if (count != 0)
    DBUG_PRINT("error",
               ("Unused_links doesn't match open_cache: diff: %d", count));
}
#endif

/** Free all unused TABLE objects in the table cache. */

void Table_cache::free_all_unused_tables() {
  assert_owner();

  while (m_unused_tables) {
    TABLE *table_to_free = m_unused_tables;
    remove_table(table_to_free);
    intern_close_table(table_to_free);
  }
}

#ifndef NDEBUG
/**
  Print debug information for the contents of the table cache.
*/

void Table_cache::print_tables() {
  uint unused = 0;
  uint count = 0;

  static_assert(TL_WRITE_ONLY + 1 == array_elements(lock_descriptions), "");

  for (const auto &key_and_value : m_cache) {
    Table_cache_element *el = key_and_value.second.get();

    auto print_free_tables =
        [&unused](const Table_cache_element::TABLE_list &free_tables) {
          Table_cache_element::TABLE_list::Iterator it(free_tables);
          TABLE *entry;
          while ((entry = it++)) {
            unused++;
            printf("%-14.14s %-32s%6ld%8ld%6d  %s\n", entry->s->db.str,
                   entry->s->table_name.str, entry->s->version(), 0L,
                   entry->db_stat ? 1 : 0, "Not in use");
          }
        };

    Table_cache_element::TABLE_list::Iterator it(el->used_tables);
    TABLE *entry;
    while ((entry = it++)) {
      printf("%-14.14s %-32s%6ld%8u%6d  %s\n", entry->s->db.str,
             entry->s->table_name.str, entry->s->version(),
             entry->in_use->thread_id(), entry->db_stat ? 1 : 0,
             lock_descriptions[(int)entry->reginfo.lock_type]);
    }

    print_free_tables(el->free_tables_slim);
    print_free_tables(el->free_tables_full_triggers);
  }

  if (m_unused_tables != nullptr) {
    TABLE *start_link = m_unused_tables;
    TABLE *lnk = m_unused_tables;
    do {
      if (lnk != lnk->next->prev || lnk != lnk->prev->next) {
        printf("unused_links isn't linked properly\n");
        return;
      }
    } while (count++ < m_table_count && (lnk = lnk->next) != start_link);
    if (lnk != start_link) printf("Unused_links aren't connected\n");
  }

  if (count != unused)
    printf("Unused_links (%d) doesn't match table_def_cache: %d\n", count,
           unused);
}
#endif

/**
  Initialize all instances of table cache to be used by server.

  Each instance is now a separate page-aligned segment obtained through
  the ut_malloc_page_withkey()/ut_free_page() bridge
  (storage/innobase/include/ut0page_alloc.h) instead of an element of a
  statically embedded array - see the ownership-invariant comment on
  m_table_cache for what a non-null slot guarantees.

  @retval false - success.
  @retval true  - failure.
*/

bool Table_cache_manager::init() {
  Table_cache::init_psi_keys();

#ifndef NDEBUG
  /* A prior cycle that init()'d without a matching destroy()/
  teardown_slots() (e.g. a gunit fixture that early-returned past its
  TearDown()) would otherwise be papered over by silently re-populating
  already-non-null slots, corrupting whatever the leftover Table_cache
  objects still reference. Fail loudly instead. */
  for (uint i = 0; i < MAX_TABLE_CACHES; i++) {
    assert(m_table_cache[i] == nullptr);
  }
#endif

  for (uint i = 0; i < table_cache_instances; i++) {
    void *seg;
    if (static_cast<int>(i) == m_fail_segment_alloc_at_index_for_tests) {
      // Test-only injection point, @sa set_segment_alloc_failure_for_tests():
      // take exactly the branch a real allocator failure would.
      seg = nullptr;
    } else {
      seg = ut_malloc_page_withkey(key_memory_table_cache_segment,
                                   sizeof(Table_cache));
    }
    if (seg == nullptr) {
      teardown_slots();
      return true;
    }

    /* Value-initialization: byte-deterministic object state (including
    padding) ahead of Table_cache::init() below, which is what actually
    assigns every member the class relies on - this placement-new form
    carries no functional weight of its own. */
    Table_cache *c = new (seg) Table_cache();
    if (c->init(i)) {
      c->~Table_cache();
      ut_free_page(seg);
      teardown_slots();
      return true;
    }
    m_table_cache[i] = c;
  }

  return false;
}

/**
  @sa the teardown_slots() declaration in table_cache.h for the contract.
*/
void Table_cache_manager::teardown_slots() {
  for (uint i = 0; i < MAX_TABLE_CACHES; i++) {
    if (m_table_cache[i] == nullptr) continue;
    m_table_cache[i]->destroy();
    m_table_cache[i]->~Table_cache();
    ut_free_page(m_table_cache[i]);
    m_table_cache[i] = nullptr;
  }
}

/**
  Test-only: @sa the segments_alive_for_tests() declaration in
  table_cache.h for the contract.
*/
uint Table_cache_manager::segments_alive_for_tests() const {
  uint alive = 0;
  for (uint i = 0; i < MAX_TABLE_CACHES; i++) {
    if (m_table_cache[i] != nullptr) alive++;
  }
  return alive;
}

/**
  Pure cap formula. @sa Table_cache_manager::compute_effective_instance_count()
  in table_cache.h for the contract; kept free of global reads so unit tests
  can drive it directly.
*/

ulong Table_cache_manager::compute_effective_instance_count(
    ulong requested, bool topology_ok, ulong phys_cores) {
  /* cap = topology_ok ? phys_cores : no cap (min(requested, cap) reduces to
  just "requested" when there is no cap to apply). */
  return std::clamp<ulong>(
      topology_ok ? std::min<ulong>(requested, phys_cores) : requested, 1,
      MAX_TABLE_CACHES);
}

/**
  Resolve the effective number of table cache instances.
  @sa Table_cache_manager::resolve_instance_count() in table_cache.h for
  the full contract.
*/

void Table_cache_manager::resolve_instance_count() {
  const ulong requested = table_cache_instances;

  const Sql_cpu_topology &cpu_topology = sql_machine_topology().cpu;

  /* Same guard as the buffer-pool-instances cap (ha_innodb.cc): a
  topology snapshot that failed to build reports initialized=false and
  effective_cpu_capacity()==0 - treat that as "cap unavailable" rather
  than as "0 physical cores", which would otherwise collapse every
  configuration (including the default one) down to 1 instance. */
  bool topology_ok =
      cpu_topology.initialized && cpu_topology.effective_cpu_capacity() > 0;
  DBUG_EXECUTE_IF("table_cache_topology_unavailable", topology_ok = false;);

  ulong phys_cores = static_cast<ulong>(cpu_topology.effective_cpu_capacity());
  /* Fixed value deliberately not matching any real host in the fleet,
  including the 64-physical-core target machine: lets mtr exercise the
  cap's arithmetic (request vs. cap, log line content) deterministically,
  without the test depending on - or being invalidated by - the actual
  core count of whatever host runs it. */
  DBUG_EXECUTE_IF("table_cache_fake_topology_61", phys_cores = 61;);

  const ulong effective =
      compute_effective_instance_count(requested, topology_ok, phys_cores);

  if (effective < requested) {
    LogErr(SYSTEM_LEVEL, ER_TABLE_CACHE_INSTANCES_CAPPED, requested, effective,
           phys_cores);
  }

  table_cache_instances = effective;
  table_cache_size_per_instance =
      table_cache_per_instance_limit(table_cache_size, effective);
  table_cache_triggers_per_instance =
      table_cache_per_instance_limit(table_cache_triggers, effective);
}

namespace {

/**
  Process-wide CPU-shard ordinal map, indexed by logical CPU id. Empty
  until Table_cache_manager::init_cpu_shard_map() runs (production: once,
  from init_server_components(), after table_cache_instances is final) or
  a test calls Table_cache_manager::set_cpu_shard_ordinals_for_tests() -
  both write through this same static, so cache_index_for_thread() never
  reads a different copy than whichever of the two last populated it.
  Empty in every gunit fixture that never calls either (the map stays
  empty, so Table_cache_manager::resolve_shard() always takes branch (d)
  there - see that function's contract).
*/
std::vector<int> cpu_shard_ordinals;

const std::vector<int> &shard_ordinals() noexcept { return cpu_shard_ordinals; }

/* sched_getcpu() is only declared/implemented for Linux targets in this
tree (HAVE_SCHED_GETCPU is set only in other subsystems' own directory
scopes - InnoDB's innodb.cmake, RocksDB's build macro - never for
sql_main) - this is a platform guard, not a build-profile one, so
current_cpu_id() takes the same shape on debug and release. */
#ifdef __linux__
int current_cpu_id() noexcept { return sched_getcpu(); }
#else
int current_cpu_id() noexcept { return -1; }
#endif

}  // namespace

/**
  @sa Table_cache_manager::build_cpu_shard_ordinals() in table_cache.h for
  the contract.
*/
std::vector<int> Table_cache_manager::build_cpu_shard_ordinals(
    const Sql_machine_topology &topology) {
  std::vector<int> ordinals;

  const Sql_cpu_topology &cpu = topology.cpu;
  if (!cpu.initialized) return ordinals;

  int max_cpu_id = -1;
  for (const auto &socket : cpu.sockets) {
    for (const auto &core : socket.cores) {
      for (uint32_t cpu_id : core.logical_cpu_ids) {
        if (static_cast<int>(cpu_id) > max_cpu_id)
          max_cpu_id = static_cast<int>(cpu_id);
      }
    }
  }
  if (max_cpu_id < 0) return ordinals;

  ordinals.assign(static_cast<size_t>(max_cpu_id) + 1, -1);

  /* Nodes that still have CPUs after the affinity filter, in ascending
  node_id order - this list doubles as the node -> dense-index map used
  below. Sorted explicitly rather than trusting snapshot order: nothing
  in the Sql_numa_snapshot contract promises online_nodes is sorted. */
  std::vector<uint32_t> node_ids_with_cpus;
  if (topology.numa.initialized) {
    for (const auto &node : topology.numa.nodes) {
      if (!node.logical_cpu_ids.empty())
        node_ids_with_cpus.push_back(node.node_id);
    }
    std::sort(node_ids_with_cpus.begin(), node_ids_with_cpus.end());
  }
  const size_t node_count = node_ids_with_cpus.size();

  if (node_count <= 1) {
    /* No usable multi-node NUMA snapshot: degenerate to a fixed
    socket-major numbering - sequential physical-core index, sockets and
    cores visited in the ascending id order they are already stored in
    (see build_cpu_topology_from_sysfs(): sockets_map is a std::map, so
    both the socket and the per-socket core iteration below are already
    ascending by id). */
    int core_index = 0;
    for (const auto &socket : cpu.sockets) {
      for (const auto &core : socket.cores) {
        for (uint32_t cpu_id : core.logical_cpu_ids) {
          ordinals[cpu_id] = core_index;
        }
        ++core_index;
      }
    }
    return ordinals;
  }

  /* Node-minor (interleaved) numbering: ordinal = core_rank_in_node * N +
  dense_node_index. Sockets/cores are visited in ascending id order (see
  above), and a stable per-node counter over that single sweep yields
  per-node core ranks in that same ascending order - this is what the
  "cores in ascending id order" part of the contract reduces to, without
  needing a single global scalar id for a physical core. */
  std::vector<uint32_t> rank_in_node(node_count, 0);
  for (const auto &socket : cpu.sockets) {
    for (const auto &core : socket.cores) {
      if (core.logical_cpu_ids.empty()) continue;

      /* A physical core's HT siblings always share one NUMA node, so any
      representative logical CPU id resolves the whole core's node. */
      const uint32_t representative_cpu = core.logical_cpu_ids.front();
      const int32_t node_id =
          sql_numa_cpu_to_node(topology.numa, representative_cpu);
      if (node_id < 0) continue;  // Core not covered by the NUMA snapshot.

      const auto node_it = std::lower_bound(
          node_ids_with_cpus.begin(), node_ids_with_cpus.end(),
          static_cast<uint32_t>(node_id));
      if (node_it == node_ids_with_cpus.end() ||
          *node_it != static_cast<uint32_t>(node_id)) {
        continue;  // Node had no CPUs left after the affinity filter.
      }

      const size_t dense_node_index =
          static_cast<size_t>(node_it - node_ids_with_cpus.begin());
      const uint32_t ordinal =
          rank_in_node[dense_node_index] * static_cast<uint32_t>(node_count) +
          static_cast<uint32_t>(dense_node_index);
      ++rank_in_node[dense_node_index];

      for (uint32_t cpu_id : core.logical_cpu_ids) {
        ordinals[cpu_id] = static_cast<int>(ordinal);
      }
    }
  }

  return ordinals;
}

/**
  @sa Table_cache_manager::summarize_shard_map() in table_cache.h for the
  contract.
*/
Shard_map_summary Table_cache_manager::summarize_shard_map(
    const std::vector<int> &ordinals, const Sql_numa_snapshot &numa,
    ulong instances) {
  Shard_map_summary summary{};
  summary.key_is_cpu = !ordinals.empty();
  summary.shards = instances;

  /* Distinct ordinal values, not the count of populated entries: HT
  siblings of the same core share an ordinal, so counting entries would
  overcount physical cores by the SMT factor. */
  std::vector<int> distinct_ordinals;
  distinct_ordinals.reserve(ordinals.size());
  for (int ordinal : ordinals) {
    if (ordinal >= 0) distinct_ordinals.push_back(ordinal);
  }
  std::sort(distinct_ordinals.begin(), distinct_ordinals.end());
  distinct_ordinals.erase(
      std::unique(distinct_ordinals.begin(), distinct_ordinals.end()),
      distinct_ordinals.end());
  summary.phys_cores = distinct_ordinals.size();

  ulong nodes_with_cpus = 0;
  if (numa.initialized) {
    for (const auto &node : numa.nodes) {
      if (!node.logical_cpu_ids.empty()) ++nodes_with_cpus;
    }
  }
  summary.nodes_with_cpus = nodes_with_cpus;

  summary.node_confined = summary.key_is_cpu && nodes_with_cpus > 1 &&
                          (instances % nodes_with_cpus == 0);

  return summary;
}

/**
  @sa Table_cache_manager::resolve_shard() in table_cache.h for the
  contract.
*/
Shard_resolution Table_cache_manager::resolve_shard(
    int cpu_id, int last_cpu_id, int cached_shard,
    const std::vector<int> &ordinals, ulong instances, my_thread_id tid) {
  const bool cached_valid =
      cached_shard >= 0 && static_cast<ulong>(cached_shard) < instances;

  // (a) No migration since the last resolution: the cached shard is still
  // current - reuse it, no write.
  if (cpu_id >= 0 && cached_valid && cpu_id == last_cpu_id) {
    return {static_cast<uint>(cached_shard), false};
  }

  // (b) Current CPU resolves through the ordinal map: the only branch
  // that produces a fresh (cpu_id, shard) pair worth memoizing.
  if (cpu_id >= 0 && static_cast<size_t>(cpu_id) < ordinals.size() &&
      ordinals[cpu_id] >= 0) {
    const uint shard = static_cast<uint>(
        static_cast<ulong>(ordinals[cpu_id]) % instances);
    return {shard, true};
  }

  // (c) Current CPU unresolvable (non-Linux, or a CPU absent from the
  // topology snapshot), but the last known shard is still a valid index:
  // prefer it over flapping to the thread_id()-based mapping.
  if (cached_valid) {
    return {static_cast<uint>(cached_shard), false};
  }

  // (d) Legacy fallback: neither the current CPU nor a cached shard is
  // available. Deliberately not memoized - in gunit, where ordinals is
  // always empty (init_cpu_shard_map() never runs there), this is the
  // only branch ever taken, so its non-memoization is what keeps every
  // existing gunit TABLE-cache test's behavior unchanged by this change.
  return {static_cast<uint>(tid % instances), false};
}

/**
  @sa Table_cache_manager::init_cpu_shard_map() in table_cache.h for the
  contract.
*/
void Table_cache_manager::init_cpu_shard_map() {
  cpu_shard_ordinals = build_cpu_shard_ordinals(sql_machine_topology());

  const Shard_map_summary summary = summarize_shard_map(
      cpu_shard_ordinals, sql_machine_topology().numa, table_cache_instances);

  LogErr(SYSTEM_LEVEL, ER_TABLE_CACHE_SHARD_MAP,
         summary.key_is_cpu ? "physical CPU core"
                            : "thread id (CPU topology unavailable)",
         summary.shards, summary.phys_cores, summary.nodes_with_cpus,
         summary.node_confined ? "yes" : "no");
}

/**
  @sa Table_cache_manager::set_cpu_shard_ordinals_for_tests() in
  table_cache.h for the contract.
*/
void Table_cache_manager::set_cpu_shard_ordinals_for_tests(
    std::vector<int> ordinals) {
  cpu_shard_ordinals = std::move(ordinals);
}

/**
  Compute index of table cache instance for the given thread.
  @sa Table_cache_manager::cache_index_for_thread() in table_cache.h for
  the contract.
*/
uint Table_cache_manager::cache_index_for_thread(THD *thd) const {
  const ulong instances = table_cache_instances;
  /* resolve_instance_count() (called once from init_server_components(),
  before table_def_init()) clamps table_cache_instances to
  [1, MAX_TABLE_CACHES] and nothing lowers it afterwards, so a plain
  assert suffices here - a reference implementation without that
  guarantee would instead need to clamp a possible 0 to 1. */
  assert(instances >= 1);

  /* Test-only: force a different shard on every resolution so that the
  acquire-time and release-time resolutions for the same TABLE are
  guaranteed to disagree - this is what makes
  percona.table_cache_shard_rotation crash/assert on a debug build
  without the release-by-stamp fix and pass with it. */
  DBUG_EXECUTE_IF("table_cache_rotate_shard", {
    static std::atomic<uint> rotor{0};
    return rotor.fetch_add(1, std::memory_order_relaxed) %
           static_cast<uint>(instances);
  });

  const int cpu_id = current_cpu_id();
  const Shard_resolution resolution =
      resolve_shard(cpu_id, thd->last_cpu_id(), thd->cpu_shard(),
                    shard_ordinals(), instances, thd->thread_id());

  if (resolution.cache_result) {
    /* thd must be the calling thread's own THD: nothing locks
    THD::m_cpu_shard/m_last_cpu_id (by design - they are a single
    thread's own hot-path memoization, not shared state), so writing into
    a THD other than current_thd from here would race that thread's own
    reads/writes of the same fields. The assert turns an unknown caller
    passing a foreign THD into a diagnosable debug crash instead of a
    silent data race; the "if" is what keeps a release build safe once
    the assert compiles away - it skips the memoization rather than
    racing the write. Branches (a)/(c)/(d) never reach here at all, and
    in gunit (empty ordinals) branch (b) itself is unreachable, so no
    existing test exercises this guard either way. */
    assert(thd == current_thd);
    if (thd == current_thd) {
      thd->set_cpu_shard(static_cast<int>(resolution.shard));
      thd->set_last_cpu_id(cpu_id);
    }
  }

  return resolution.shard;
}

/**
  Destroy all instances of table cache which were used by server.
  @sa teardown_slots() - the single owner of the destroy/free sequence,
  shared with init()'s unwind-on-failure path.
*/

void Table_cache_manager::destroy() { teardown_slots(); }

/**
  Get total number of used and unused TABLE objects in all table caches.

  @note Doesn't require acquisition of table cache locks if inexact number
        of tables is acceptable.
*/

uint Table_cache_manager::cached_tables() {
  uint result = 0;

  for (uint i = 0; i < table_cache_instances; i++)
    result += m_table_cache[i]->cached_tables();

  return result;
}

/**
  Get total number of used and unused TABLE objects with fully-loaded triggers
  in all table caches.
*/

uint Table_cache_manager::loaded_triggers_tables() const {
  uint result = 0;

  for (uint i = 0; i < table_cache_instances; i++)
    result += m_table_cache[i]->loaded_triggers_tables();

  return result;
}

/**
  Acquire locks on all instances of table cache and table definition
  cache (i.e. LOCK_open).
*/

void Table_cache_manager::lock_all_and_tdc() {
  for (uint i = 0; i < table_cache_instances; i++) m_table_cache[i]->lock();

  mysql_mutex_lock(&LOCK_open);
}

/**
  Release locks on all instances of table cache and table definition
  cache.
*/

void Table_cache_manager::unlock_all_and_tdc() {
  mysql_mutex_unlock(&LOCK_open);

  for (uint i = 0; i < table_cache_instances; i++) m_table_cache[i]->unlock();
}

/**
  Assert that caller owns locks on all instances of table cache.
*/

void Table_cache_manager::assert_owner_all() {
  for (uint i = 0; i < table_cache_instances; i++)
    m_table_cache[i]->assert_owner();
}

/**
  Assert that caller owns locks on all instances of table cache
  and table definition cache.
*/

void Table_cache_manager::assert_owner_all_and_tdc() {
  assert_owner_all();

  mysql_mutex_assert_owner(&LOCK_open);
}

/**
   Remove and free all or some (depending on parameter) TABLE objects
   for the table from all table cache instances.

   @param  thd          Thread context
   @param  remove_type  Type of removal. @sa tdc_remove_table().
   @param  share        TABLE_SHARE for the table to be removed.

   @note Caller should own LOCK_open and locks on all table cache
         instances.
*/
void Table_cache_manager::free_table(THD *thd [[maybe_unused]],
                                     enum_tdc_remove_table_type remove_type
                                     [[maybe_unused]],
                                     TABLE_SHARE *share) {
  Table_cache_element *cache_el[MAX_TABLE_CACHES];

  assert_owner_all_and_tdc();

  /*
    Freeing last TABLE instance for the share will destroy the share
    and corresponding TABLE_SHARE::cache_element[] array. To make
    iteration over this array safe, even when share is destroyed in
    the middle of iteration, we create copy of this array on the stack
    and iterate over it.
  */
  memcpy(&cache_el, share->cache_element,
         table_cache_instances * sizeof(Table_cache_element *));

  auto remove_and_close_free_tables =
      [](Table_cache &cache, Table_cache_element::TABLE_list &free_list) {
        Table_cache_element::TABLE_list::Iterator it(free_list);
        TABLE *table;
        while ((table = it++)) {
          cache.remove_table(table);
          intern_close_table(table);
        }
      };

  for (uint i = 0; i < table_cache_instances; i++) {
    if (cache_el[i]) {
      /*
        Since freeing last TABLE object for the share will destroy all
        related Table_cache_element objects and hence their list members,
        we need to remember the fact whether unused TABLE objects lists
        are empty (and avoid iterating through them) before proceeding to
        freeing TABLE objects.
      */
      bool has_free_tables_slim = !cache_el[i]->free_tables_slim.is_empty();
      bool has_free_tables_full_triggers =
          !cache_el[i]->free_tables_full_triggers.is_empty();

#ifndef NDEBUG
      if (remove_type == TDC_RT_REMOVE_ALL)
        assert(cache_el[i]->used_tables.is_empty());
      else if (remove_type == TDC_RT_REMOVE_NOT_OWN ||
               remove_type == TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE) {
        Table_cache_element::TABLE_list::Iterator it2(cache_el[i]->used_tables);
        TABLE *table;
        while ((table = it2++)) {
          if (table->in_use != thd) assert(0);
        }
      }
#endif
      if (remove_type == TDC_RT_MARK_FOR_REOPEN) {
        Table_cache_element::TABLE_list::Iterator it2(cache_el[i]->used_tables);
        TABLE *table;
        while ((table = it2++)) {
          table->invalidate_stats();
        }
      }

      if (has_free_tables_slim)
        remove_and_close_free_tables(*m_table_cache[i],
                                     cache_el[i]->free_tables_slim);
      if (has_free_tables_full_triggers)
        remove_and_close_free_tables(*m_table_cache[i],
                                     cache_el[i]->free_tables_full_triggers);
    }
  }
}

/** Free all unused TABLE objects in all table cache instances. */

void Table_cache_manager::free_all_unused_tables() {
  assert_owner_all_and_tdc();

  for (uint i = 0; i < table_cache_instances; i++)
    m_table_cache[i]->free_all_unused_tables();
}

#ifndef NDEBUG
/**
  Print debug information for the contents of all table cache instances.
*/

void Table_cache_manager::print_tables() {
  puts(
      "DB             Table                            Version  Thread  Open  "
      "Lock");

  for (uint i = 0; i < table_cache_instances; i++)
    m_table_cache[i]->print_tables();
}
#endif
