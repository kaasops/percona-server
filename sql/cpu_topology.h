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

#ifndef SQL_CPU_TOPOLOGY_H
#define SQL_CPU_TOPOLOGY_H

#include <sched.h>
#include <stdint.h>

#include <vector>

struct Sql_cpu_core {
  uint32_t core_id{0};
  std::vector<uint32_t> logical_cpu_ids{};

  void clear() noexcept;
};

struct Sql_cpu_socket {
  uint32_t socket_id{0};
  std::vector<Sql_cpu_core> cores{};

  void clear() noexcept;
};

struct Sql_cpu_topology {
  bool initialized{false};

  uint32_t logical_cpus{0};
  uint32_t physical_cores{0};
  uint32_t sockets_count{0};

  bool smt_enabled{false};
  uint32_t threads_per_core{1};

  std::vector<Sql_cpu_socket> sockets{};

  std::vector<int32_t> cpu_to_socket{};
  std::vector<int32_t> cpu_to_core{};

  [[nodiscard]] uint32_t effective_cpu_capacity() const noexcept {
    return physical_cores != 0 ? physical_cores : logical_cpus;
  }

  [[nodiscard]] bool has_smt() const noexcept { return smt_enabled; }

  void clear() noexcept;
};

struct Sql_numa_node {
  uint32_t node_id{0};
  std::vector<uint32_t> logical_cpu_ids{};
  std::vector<uint32_t> distance{};

  uint64_t mem_total_kb{0};
  uint64_t mem_free_kb{0};

  /**
    Whether this node is a valid memory-policy target for the current
    process, per numa_get_mems_allowed() (cgroup cpuset.mems / mbind()
    cpuset restriction). Not derived from sched_getaffinity(): a node can be
    CPU-allowed but memory-disallowed, or vice versa. Defaults to true
    (unrestricted) on non-Linux/non-HAVE_LIBNUMA builds, or when the
    process-wide mask could not be queried. Advisory metadata only, for use
    by memory-placement gates; nodes are never dropped from the snapshot on
    account of this flag.
  */
  bool mem_allowed{true};

  void clear() noexcept;
};

struct Sql_numa_snapshot {
  bool initialized{false};

  uint32_t nodes_count{0};

  std::vector<Sql_numa_node> nodes{};
  std::vector<int32_t> cpu_to_node{};

  void clear() noexcept;
};

struct Sql_machine_topology {
  Sql_cpu_topology cpu{};
  Sql_numa_snapshot numa{};

  void clear() noexcept;
};

/** Process-wide immutable machine topology snapshot, built exactly once
    (C++11 magic static, thread-safe): lazy thread-safe init at first
    access; currently the first access is the table cache instance count
    resolver (Table_cache_manager::resolve_instance_count(), called from
    init_server_components() ahead of table_def_init()) - earlier in
    startup than storage-engine init, so also ahead of the buffer pool
    instance count cap in innodb_buffer_pool_size_init().
    CPU hotplug and runtime cgroup cpuset changes are NOT reflected (known
    limitation). */
[[nodiscard]] const Sql_machine_topology &sql_machine_topology() noexcept;

[[nodiscard]] bool sql_build_cpuset_for_socket(const Sql_cpu_topology &topology,
                                               uint32_t socket_id,
                                               cpu_set_t *cpuset) noexcept;

[[nodiscard]] bool sql_build_cpuset_for_numa_node(
    const Sql_numa_snapshot &snapshot, uint32_t node_id,
    cpu_set_t *cpuset) noexcept;

[[nodiscard]] bool sql_build_cpuset_for_instance(
    const Sql_machine_topology &topology, uint32_t instance_no,
    cpu_set_t *cpuset) noexcept;

/**
  Resolve the NUMA node id a logical instance index should be placed on,
  using the same instance_no % nodes_count mapping as the NUMA branch of
  sql_build_cpuset_for_instance(). Returns false when no multi-node NUMA
  topology is available (single-node or no NUMA info at all): callers
  should then fall back to a non-NUMA-specific memory policy rather than
  binding to a node index that carries no real meaning.
*/
[[nodiscard]] bool sql_numa_node_for_instance(const Sql_numa_snapshot &numa,
                                              uint32_t instance_no,
                                              uint32_t *node_id) noexcept;

[[nodiscard]] uint32_t sql_numa_node_count(
    const Sql_numa_snapshot &snapshot) noexcept;

[[nodiscard]] const Sql_numa_node *sql_numa_node_by_index(
    const Sql_numa_snapshot &snapshot, uint32_t index) noexcept;

[[nodiscard]] int32_t sql_numa_cpu_to_node(const Sql_numa_snapshot &snapshot,
                                           uint32_t cpu_id) noexcept;

#endif /* SQL_CPU_TOPOLOGY_H */
