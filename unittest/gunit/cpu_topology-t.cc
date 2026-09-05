/* Copyright (c) 2026, Alexey Bychko.

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

/*
  Covers CPU/socket/NUMA topology detection and the placement helpers built
  on top of it (sql/cpu_topology.h): synthetic-snapshot unit tests plus
  self-consistency probes against the machine executing the tests.
*/

#include "sql/cpu_topology.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

/*
  A two-socket machine with two cores per socket and SMT: logical CPUs are
  numbered so that siblings of one core are not adjacent, which is what real
  machines look like and what a naive implementation tends to get wrong.
*/
Sql_cpu_topology make_two_socket_topology() {
  Sql_cpu_topology topology;

  topology.initialized = true;
  topology.logical_cpus = 8;
  topology.physical_cores = 4;
  topology.sockets_count = 2;
  topology.smt_enabled = true;
  topology.threads_per_core = 2;

  topology.sockets = {
      Sql_cpu_socket{0, {Sql_cpu_core{0, {0, 4}}, Sql_cpu_core{1, {1, 5}}}},
      Sql_cpu_socket{1, {Sql_cpu_core{2, {2, 6}}, Sql_cpu_core{3, {3, 7}}}},
  };

  topology.cpu_to_socket = {0, 0, 1, 1, 0, 0, 1, 1};
  topology.cpu_to_core = {0, 1, 2, 3, 0, 1, 2, 3};

  return topology;
}

Sql_numa_snapshot make_two_node_snapshot() {
  Sql_numa_snapshot snapshot;

  snapshot.initialized = true;
  snapshot.nodes_count = 2;
  // 5-value aggregate init: the 6th field (mem_allowed) takes its default
  // (true), which is exactly what these tests want -- none of them exercise
  // memory-policy gating.
  snapshot.nodes = {
      Sql_numa_node{0, {0, 1, 4, 5}, {10, 21}, 0, 0},
      Sql_numa_node{1, {2, 3, 6, 7}, {21, 10}, 0, 0},
  };
  snapshot.cpu_to_node = {0, 0, 1, 1, 0, 0, 1, 1};

  return snapshot;
}

// This implementation fills a plain POSIX cpu_set_t; every test below reads
// it back through this helper instead of a vector-valued member.
std::vector<std::uint32_t> cpu_set_to_sorted_ids(const cpu_set_t &cpu_set) {
  std::vector<std::uint32_t> ids;

  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &cpu_set)) {
      ids.push_back(static_cast<std::uint32_t>(cpu));
    }
  }

  return ids;
}

}  // namespace

TEST(CpuTopologyTests, EffectiveCapacityPrefersPhysicalCores) {
  Sql_cpu_topology topology = make_two_socket_topology();
  EXPECT_EQ(topology.effective_cpu_capacity(), 4U);

  // A snapshot that could not determine physical cores must still report a
  // usable capacity rather than zero, otherwise every consumer has to guard
  // against a division by zero of its own.
  topology.physical_cores = 0;
  EXPECT_EQ(topology.effective_cpu_capacity(), 8U);
}

TEST(CpuTopologyTests, ClearResetsEveryField) {
  Sql_cpu_topology topology = make_two_socket_topology();
  topology.clear();

  EXPECT_FALSE(topology.initialized);
  EXPECT_EQ(topology.logical_cpus, 0U);
  EXPECT_EQ(topology.physical_cores, 0U);
  EXPECT_EQ(topology.sockets_count, 0U);
  EXPECT_FALSE(topology.has_smt());
  EXPECT_TRUE(topology.sockets.empty());
  EXPECT_TRUE(topology.cpu_to_socket.empty());
  EXPECT_TRUE(topology.cpu_to_core.empty());
}

TEST(CpuSetForSocketTests, CollectsEveryLogicalCpuOfTheSocket) {
  const Sql_cpu_topology topology = make_two_socket_topology();
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);

  ASSERT_TRUE(sql_build_cpuset_for_socket(topology, 1, &cpu_set));
  EXPECT_EQ(cpu_set_to_sorted_ids(cpu_set),
            (std::vector<std::uint32_t>{2, 3, 6, 7}));
}

TEST(CpuSetForSocketTests, RejectsUninitializedTopologyAndUnknownSocket) {
  Sql_cpu_topology topology = make_two_socket_topology();
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);

  EXPECT_FALSE(sql_build_cpuset_for_socket(topology, 42, &cpu_set));
  EXPECT_EQ(CPU_COUNT(&cpu_set), 0);

  topology.initialized = false;
  EXPECT_FALSE(sql_build_cpuset_for_socket(topology, 0, &cpu_set));
  EXPECT_EQ(CPU_COUNT(&cpu_set), 0);
}

TEST(CpuSetForNumaNodeTests, ReturnsNodeCpusAndRejectsUnknownNode) {
  const Sql_numa_snapshot snapshot = make_two_node_snapshot();
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);

  ASSERT_TRUE(sql_build_cpuset_for_numa_node(snapshot, 0, &cpu_set));
  EXPECT_EQ(cpu_set_to_sorted_ids(cpu_set),
            (std::vector<std::uint32_t>{0, 1, 4, 5}));

  EXPECT_FALSE(sql_build_cpuset_for_numa_node(snapshot, 7, &cpu_set));
  EXPECT_EQ(CPU_COUNT(&cpu_set), 0);
}

TEST(CpuSetForInstanceTests, PrefersNumaNodesAndCyclesThroughThem) {
  Sql_machine_topology topology;
  topology.cpu = make_two_socket_topology();
  topology.numa = make_two_node_snapshot();

  cpu_set_t first;
  cpu_set_t third;
  CPU_ZERO(&first);
  CPU_ZERO(&third);

  ASSERT_TRUE(sql_build_cpuset_for_instance(topology, 0, &first));
  ASSERT_TRUE(sql_build_cpuset_for_instance(topology, 2, &third));

  // Instance 2 wraps around to node 0, so it must land on the same CPUs as
  // instance 0 -- placement is expected to repeat, not to drift.
  EXPECT_EQ(cpu_set_to_sorted_ids(first),
            (std::vector<std::uint32_t>{0, 1, 4, 5}));
  EXPECT_EQ(cpu_set_to_sorted_ids(third), cpu_set_to_sorted_ids(first));

  cpu_set_t second;
  CPU_ZERO(&second);
  ASSERT_TRUE(sql_build_cpuset_for_instance(topology, 1, &second));
  EXPECT_EQ(cpu_set_to_sorted_ids(second),
            (std::vector<std::uint32_t>{2, 3, 6, 7}));
}

TEST(CpuSetForInstanceTests, FallsBackToSocketsWhenNumaIsAbsent) {
  Sql_machine_topology topology;
  topology.cpu = make_two_socket_topology();
  // NUMA snapshot left uninitialized on purpose.

  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  ASSERT_TRUE(sql_build_cpuset_for_instance(topology, 1, &cpu_set));

  EXPECT_EQ(cpu_set_to_sorted_ids(cpu_set),
            (std::vector<std::uint32_t>{2, 3, 6, 7}));
}

TEST(CpuSetForInstanceTests, FallsBackToASingleCpuWhenOnlyCountIsKnown) {
  Sql_machine_topology topology;
  topology.cpu.initialized = true;
  topology.cpu.logical_cpus = 4;
  // No sockets and no NUMA: the planner knows how many CPUs exist and
  // nothing else, which is what a degraded snapshot looks like.

  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  ASSERT_TRUE(sql_build_cpuset_for_instance(topology, 5, &cpu_set));
  EXPECT_EQ(cpu_set_to_sorted_ids(cpu_set), (std::vector<std::uint32_t>{1}));
}

// This implementation's flat-fallback branch calls
// detect_logical_cpu_count() when the topology carries no socket/NUMA/count
// information at all (cpu_topology.cc:1104-1105), so on a live machine an
// empty Sql_machine_topology still yields a usable single-CPU set instead
// of failing outright.
TEST(CpuSetForInstanceTests, DegradesToSingleCpuWhenNothingIsKnown) {
  const Sql_machine_topology topology;
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);

  EXPECT_TRUE(sql_build_cpuset_for_instance(topology, 0, &cpu_set));
  EXPECT_EQ(CPU_COUNT(&cpu_set), 1);
}

TEST(NumaAccessorTests, ReportCountsAndMapCpusOnlyWhenInitialized) {
  Sql_numa_snapshot snapshot = make_two_node_snapshot();

  EXPECT_EQ(sql_numa_node_count(snapshot), 2U);
  ASSERT_NE(sql_numa_node_by_index(snapshot, 1), nullptr);
  EXPECT_EQ(sql_numa_node_by_index(snapshot, 1)->node_id, 1U);
  EXPECT_EQ(sql_numa_node_by_index(snapshot, 5), nullptr);
  EXPECT_EQ(sql_numa_cpu_to_node(snapshot, 6), 1);
  EXPECT_EQ(sql_numa_cpu_to_node(snapshot, 99), -1);

  snapshot.initialized = false;
  EXPECT_EQ(sql_numa_node_count(snapshot), 0U);
  EXPECT_EQ(sql_numa_node_by_index(snapshot, 0), nullptr);
  EXPECT_EQ(sql_numa_cpu_to_node(snapshot, 0), -1);
}

/*
  The probes below run against the machine executing the tests. They assert
  internal consistency rather than particular numbers, since those differ
  between machines -- but an inconsistent snapshot means the reader is
  wrong, and every consumer of it will be wrong in the same way.

  build_cpu_topology() is `static` in cpu_topology.cc in this
  implementation, so the only entry point available to a consumer -- and to
  these tests -- is the sql_machine_topology() singleton.
*/
TEST(CpuTopologyProbeTests, SnapshotOfThisMachineIsSelfConsistent) {
  const Sql_cpu_topology &topology = sql_machine_topology().cpu;

  ASSERT_TRUE(topology.initialized);
  ASSERT_GT(topology.logical_cpus, 0U);

  EXPECT_LE(topology.physical_cores, topology.logical_cpus);
  EXPECT_EQ(topology.sockets.size(), topology.sockets_count);

  // cpu_to_socket/cpu_to_core are indexed by physical CPU ID, not by the
  // ordinal position among visible CPUs. Under a restricted mask (taskset,
  // cgroup cpuset.cpus) logical_cpus counts only the CPUs actually visible,
  // while physical IDs can run well past that count (e.g. `taskset -c 8-11`
  // reports logical_cpus == 4 but physical IDs up to 11) -- an equality
  // against logical_cpus would reject an otherwise-correct snapshot. Find
  // the highest physical CPU ID the topology actually reports and size-guard
  // against that instead, using the same iteration the cross-check loop
  // below uses.
  std::uint32_t max_cpu_id = 0;
  for (const auto &socket : topology.sockets) {
    for (const auto &core : socket.cores) {
      for (std::uint32_t cpu : core.logical_cpu_ids) {
        if (cpu > max_cpu_id) max_cpu_id = cpu;
      }
    }
  }

  // Fatal size guard ahead of the cross-check loop below: a mutation
  // that skips the cpu_to_socket/cpu_to_core assignments entirely (rather
  // than corrupting their values) would leave these vectors too short to
  // hold max_cpu_id, and the loop's operator[] would then read out of
  // bounds (UB, not a diagnosable test failure) instead of tripping this
  // assert.
  ASSERT_GT(topology.cpu_to_socket.size(), max_cpu_id)
      << "cpu_to_socket is undersized for the cross-check loop below";
  ASSERT_GT(topology.cpu_to_core.size(), max_cpu_id)
      << "cpu_to_core is undersized for the cross-check loop below";

  std::size_t cpus_in_sockets = 0;
  for (const auto &socket : topology.sockets) {
    for (const auto &core : socket.cores) {
      cpus_in_sockets += core.logical_cpu_ids.size();

      // Cross-check values, not just sizes above: ensure_cpu_mapping_size()
      // (cpu_topology.cc:559) resizes cpu_to_socket/cpu_to_core independently
      // of the assignments that follow it (cpu_topology.cc:560-561), so a
      // size-only check would miss a mutation that drops the assignments
      // while leaving the resize intact.
      for (std::uint32_t cpu : core.logical_cpu_ids) {
        // Redundant with the ASSERT_GT guard above under normal execution,
        // but kept per-access: it documents the invariant the loop relies on
        // at the point where operator[] is actually called, and stays
        // correct if the guard above is ever edited independently of this
        // loop.
        ASSERT_LT(cpu, topology.cpu_to_socket.size());
        ASSERT_LT(cpu, topology.cpu_to_core.size());

        // static_cast: cpu_to_socket/cpu_to_core hold int32_t, socket_id/
        // core_id are uint32_t -- a bare EXPECT_EQ here would trip
        // -Wsign-compare under -Werror (MYSQL_MAINTAINER_MODE=ON).
        EXPECT_EQ(topology.cpu_to_socket[cpu],
                  static_cast<std::int32_t>(socket.socket_id));
        EXPECT_EQ(topology.cpu_to_core[cpu],
                  static_cast<std::int32_t>(core.core_id));
      }
    }
  }
  EXPECT_EQ(cpus_in_sockets, topology.logical_cpus);

  if (topology.smt_enabled) {
    EXPECT_GT(topology.threads_per_core, 1U);
  } else {
    EXPECT_EQ(topology.threads_per_core, 1U);
  }
}

TEST(CpuTopologyProbeTests, EverySocketOfThisMachineYieldsACpuSet) {
  const Sql_cpu_topology &topology = sql_machine_topology().cpu;
  ASSERT_TRUE(topology.initialized);

  for (const auto &socket : topology.sockets) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    EXPECT_TRUE(
        sql_build_cpuset_for_socket(topology, socket.socket_id, &cpu_set))
        << "socket " << socket.socket_id << " reported no CPUs";
  }
}
