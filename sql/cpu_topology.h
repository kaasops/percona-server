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

void sql_build_cpu_topology(Sql_cpu_topology &topology) noexcept;

void sql_build_numa_snapshot(Sql_numa_snapshot &snapshot) noexcept;

void sql_build_machine_topology(Sql_machine_topology &topology) noexcept;

[[nodiscard]] bool sql_build_cpuset_for_socket(const Sql_cpu_topology &topology,
                                               uint32_t socket_id,
                                               cpu_set_t *cpuset) noexcept;

[[nodiscard]] bool sql_build_cpuset_for_numa_node(
    const Sql_numa_snapshot &snapshot, uint32_t node_id,
    cpu_set_t *cpuset) noexcept;

[[nodiscard]] bool sql_build_cpuset_for_instance(
    const Sql_machine_topology &topology, uint32_t instance_no,
    cpu_set_t *cpuset) noexcept;

[[nodiscard]] uint32_t sql_numa_node_count(
    const Sql_numa_snapshot &snapshot) noexcept;

[[nodiscard]] const Sql_numa_node *sql_numa_node_by_index(
    const Sql_numa_snapshot &snapshot, uint32_t index) noexcept;

[[nodiscard]] int32_t sql_numa_cpu_to_node(const Sql_numa_snapshot &snapshot,
                                           uint32_t cpu_id) noexcept;

#endif /* SQL_CPU_TOPOLOGY_H */
