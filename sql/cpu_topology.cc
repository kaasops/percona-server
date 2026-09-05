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

#include "sql/cpu_topology.h"

/* my_config.h is not force-included by the sql_main build flags, and this
   file otherwise only pulls in <sched.h>/<stdint.h>/<vector> transitively
   (see cpu_topology.h) -- none of which define HAVE_LIBNUMA. Without this
   include the #ifdef HAVE_LIBNUMA branches below are silently compiled out
   and every NUMA node reports mem_allowed=true unconditionally (adaptation
   A1; same defect exists in the source commit this file was ported from). */
#include "my_config.h"

#include <dirent.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifdef HAVE_LIBNUMA
#include <numa.h>
#include <numaif.h>
#endif /* HAVE_LIBNUMA */

namespace {

/**
  Snapshot of sched_getaffinity(getpid(), ...): the set of logical CPUs this process
  is actually allowed to run on (taskset(1), cgroup cpuset, or a container
  runtime's CPU pinning). Every topology/NUMA builder below must intersect
  its sysfs-derived CPU lists with this mask: without it, a consumer
  building a cpuset from an unfiltered snapshot can compute one that reaches
  outside an operator's `taskset` restriction (the kernel does not reject a
  later pthread_setaffinity_np() call that does so), silently violating the
  restriction. cgroup cpusets are already partially handled (the kernel
  itself intersects, or EINVAL's on a fully disjoint mask) but taskset is
  not enforced at the setaffinity call site at all, so this must happen at
  the topology-snapshot level, once, for every consumer.
*/
struct Process_affinity_mask {
  cpu_set_t mask{};
  bool valid{false};

  Process_affinity_mask() noexcept {
    CPU_ZERO(&mask);
    /* getpid() here, not the "0 == calling thread" shorthand: this
    constructor now only ever runs once, from inside sql_machine_topology()'s
    magic-static initializer (see below), and it must report the main
    thread's affinity regardless of which thread happens to *trigger* that
    lazy initialization first. sched_getaffinity(0, ...) would instead report
    whichever thread wins the init race - possibly one already narrowed by
    any in-server thread-affinity mechanism (a thread inherits its creator's
    mask per clone(2)), which would permanently poison this snapshot with a
    self-inflicted restriction. Querying the process's thread-group leader
    (pid == tgid, i.e. getpid()) instead ties the result to "whatever the
    main/option-parsing thread's mask was", independent of caller. A genuine
    external restriction (cgroup cpuset, taskset) is still correctly
    captured, since it constrains the whole process, main thread
    included. */
    valid = ::sched_getaffinity(::getpid(), sizeof(mask), &mask) == 0 &&
            CPU_COUNT(&mask) > 0;
  }
};

/**
  Whether logical CPU cpu_id is allowed by the process affinity mask.

  CPUs at or beyond CPU_SETSIZE cannot be represented in a cpu_set_t at all
  (every cpuset builder in this file already drops them, see
  build_cpuset_from_cpu_list()), so they are reported as "allowed" here:
  this function only decides whether sched_getaffinity() *excludes* a CPU,
  and it has no information either way about CPUs it cannot represent. When
  the mask itself could not be obtained (unsupported platform, syscall
  failure), fail open and treat every CPU as allowed rather than collapsing
  the topology to nothing.
*/
[[nodiscard]] bool cpu_allowed_by_mask(const Process_affinity_mask &affinity,
                                       uint32_t cpu_id) noexcept {
  if (!affinity.valid || cpu_id >= CPU_SETSIZE) {
    return true;
  }

  return CPU_ISSET(static_cast<int>(cpu_id), &affinity.mask) != 0;
}

/** Simple helper to parse uint32_t from a C-string. */
[[nodiscard]] bool parse_uint32(const char *str, uint32_t *value) noexcept {
  if (str == nullptr || value == nullptr || *str == '\0') {
    return false;
  }

  uint64_t parsed = 0;

  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(str);
       *p != '\0'; ++p) {
    if (!std::isdigit(*p)) {
      return false;
    }

    parsed = parsed * 10U + static_cast<uint64_t>(*p - '0');

    if (parsed > UINT32_MAX) {
      return false;
    }
  }

  *value = static_cast<uint32_t>(parsed);
  return true;
}

/** Check if a directory name is of the form "cpu<id>". */
[[nodiscard]] bool is_cpu_dir_name(const char *name,
                                   uint32_t *cpu_id) noexcept {
  if (name == nullptr || std::strncmp(name, "cpu", 3) != 0) {
    return false;
  }

  return parse_uint32(name + 3, cpu_id);
}

/** Read a small text file into a std::string. */
[[nodiscard]] bool read_text_file(const char *path, std::string &out) noexcept {
  out.clear();

  FILE *file = std::fopen(path, "r");
  if (file == nullptr) {
    return false;
  }

  char buffer[4096];

  while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
    out.append(buffer);
  }

  std::fclose(file);
  return !out.empty();
}

/** Trim leading and trailing whitespace from a string (in place). */
void trim(std::string &s) noexcept {
  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }

  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }

  if (start == 0 && end == s.size()) {
    return;
  }

  s.assign(s.data() + start, end - start);
}

/**
  Parse Linux-style CPU/ID list, e.g. "0-3,8,10-11" into a vector of ids.

  The function is tolerant to extra whitespace and ignores invalid segments.
*/
[[nodiscard]] bool parse_linux_id_list(const std::string &text,
                                       std::vector<uint32_t> &ids) noexcept {
  ids.clear();

  std::string s = text;
  trim(s);
  if (s.empty()) {
    return false;
  }

  size_t pos = 0;
  bool any_valid = false;

  while (pos < s.size()) {
    /* Extract next segment up to comma. */
    size_t comma = s.find(',', pos);
    const size_t len = (comma == std::string::npos ? s.size() : comma) - pos;

    std::string segment = s.substr(pos, len);
    trim(segment);

    if (!segment.empty()) {
      size_t dash = segment.find('-');

      if (dash == std::string::npos) {
        /* Single id. */
        uint32_t value = 0;
        if (parse_uint32(segment.c_str(), &value)) {
          ids.push_back(value);
          any_valid = true;
        }
      } else {
        /* Range "start-end". */
        std::string start_str = segment.substr(0, dash);
        std::string end_str = segment.substr(dash + 1);
        trim(start_str);
        trim(end_str);

        uint32_t start_id = 0;
        uint32_t end_id = 0;

        if (parse_uint32(start_str.c_str(), &start_id) &&
            parse_uint32(end_str.c_str(), &end_id) && start_id <= end_id) {
          for (uint32_t v = start_id; v <= end_id; ++v) {
            ids.push_back(v);
          }
          any_valid = true;
        }
      }
    }

    if (comma == std::string::npos) {
      break;
    }

    pos = comma + 1;
  }

  if (!any_valid) {
    ids.clear();
    return false;
  }

  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

  return true;
}

/**
  Parse a whitespace-separated list of unsigned integers into a vector.

  Used for NUMA node distance vectors.
*/
[[nodiscard]] bool parse_distance_vector(
    const std::string &text, std::vector<uint32_t> &distance) noexcept {
  distance.clear();

  std::string s = text;
  trim(s);
  if (s.empty()) {
    return false;
  }

  bool any_valid = false;

  size_t pos = 0;
  while (pos < s.size()) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
      ++pos;
    }

    if (pos >= s.size()) {
      break;
    }

    size_t next_pos = pos;
    while (next_pos < s.size() &&
           !std::isspace(static_cast<unsigned char>(s[next_pos]))) {
      ++next_pos;
    }

    std::string token = s.substr(pos, next_pos - pos);
    uint32_t value = 0;

    if (parse_uint32(token.c_str(), &value)) {
      distance.push_back(value);
      any_valid = true;
    }

    pos = next_pos;
  }

  if (!any_valid) {
    distance.clear();
    return false;
  }

  return true;
}

/**
  Parse node meminfo to extract MemTotal and MemFree in kB.

  The format of nodeX/meminfo mirrors /proc/meminfo, per Linux ABI.
*/
[[nodiscard]] static bool parse_node_meminfo(const std::string &text,
                                             uint64_t &mem_total_kb,
                                             uint64_t &mem_free_kb) noexcept {
  mem_total_kb = 0;
  mem_free_kb = 0;

  auto trim_inplace = [](std::string &s) {
    size_t begin = 0;
    while (begin < s.size() &&
           std::isspace(static_cast<unsigned char>(s[begin]))) {
      ++begin;
    }

    size_t end = s.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
      --end;
    }

    if (begin != 0 || end != s.size()) {
      s = s.substr(begin, end - begin);
    }
  };

  auto parse_kb_value = [&trim_inplace](const std::string &raw_value,
                                        uint64_t &out) -> bool {
    std::string value = raw_value;
    trim_inplace(value);

    if (value.empty()) {
      return false;
    }

    if (value.size() >= 2) {
      const std::string suffix = value.substr(value.size() - 2);
      if (suffix == "kB" || suffix == "KB" || suffix == "kb") {
        value.resize(value.size() - 2);
        trim_inplace(value);
      }
    }

    if (value.empty()) {
      return false;
    }

    uint64_t parsed = 0;
    for (char ch : value) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        return false;
      }

      const uint64_t digit = static_cast<uint64_t>(ch - '0');
      if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10ULL) {
        return false;
      }

      parsed = parsed * 10ULL + digit;
    }

    out = parsed;
    return true;
  };

  bool found_total = false;
  bool found_free = false;

  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) {
      end = text.size();
    }

    std::string line = text.substr(pos, end - pos);
    trim_inplace(line);

    const std::string total_key = "MemTotal:";
    const std::string free_key = "MemFree:";

    const size_t total_pos = line.find(total_key);
    if (total_pos != std::string::npos) {
      uint64_t parsed = 0;
      if (parse_kb_value(line.substr(total_pos + total_key.length()), parsed)) {
        mem_total_kb = parsed;
        found_total = true;
      }
    }

    const size_t free_pos = line.find(free_key);
    if (free_pos != std::string::npos) {
      uint64_t parsed = 0;
      if (parse_kb_value(line.substr(free_pos + free_key.length()), parsed)) {
        mem_free_kb = parsed;
        found_free = true;
      }
    }

    if (end == text.size()) {
      break;
    }

    pos = end + 1;
  }

  return (found_total && mem_total_kb != 0) || (found_free && mem_free_kb != 0);
}

/** Detect logical CPU count using sysconf. */
[[nodiscard]] uint32_t detect_logical_cpu_count() noexcept {
  const long nprocs = ::sysconf(_SC_NPROCESSORS_ONLN);
  if (nprocs <= 0) {
    return 0;
  }

  return static_cast<uint32_t>(nprocs);
}

/** Ensure cpu_to_socket/core vectors are large enough to store cpu_id. */
void ensure_cpu_mapping_size(Sql_cpu_topology &topology, uint32_t cpu_id) {
  const size_t required = static_cast<size_t>(cpu_id) + 1;

  if (topology.cpu_to_socket.size() < required) {
    topology.cpu_to_socket.resize(required, -1);
  }

  if (topology.cpu_to_core.size() < required) {
    topology.cpu_to_core.resize(required, -1);
  }
}

/**
  Read a uint32_t from a text file (single line).

  This helper is used by the CPU topology builder.
*/
[[nodiscard]] bool read_uint32_file(const char *path,
                                    uint32_t *value) noexcept {
  if (value == nullptr) {
    return false;
  }

  FILE *file = std::fopen(path, "r");
  if (file == nullptr) {
    return false;
  }

  char buffer[64];
  const char *result = std::fgets(buffer, sizeof(buffer), file);
  std::fclose(file);

  if (result == nullptr) {
    return false;
  }

  char *newline = std::strchr(buffer, '\n');
  if (newline != nullptr) {
    *newline = '\0';
  }

  return parse_uint32(buffer, value);
}

/**
  Check whether a logical CPU is online.

  sysfs keeps a "cpuN" directory for every CPU the kernel knows about, online
  or not; an offline CPU loses its "topology/" subtree, which is why it must
  be excluded explicitly rather than inferred from missing topology files
  (those can also be absent for other reasons on an online CPU). CPUs that
  can never be hot-unplugged (typically cpu0) have no "online" file at all;
  treat that as online.

  Not noexcept: the cpu_dir + "/online" concatenation below allocates, and
  its only caller, build_cpu_topology_from_sysfs(), is itself not noexcept
  and runs under sql_build_cpu_topology()'s try/catch, so a failed
  allocation degrades to "topology unavailable" instead of terminating.
*/
[[nodiscard]] bool is_cpu_online(const std::string &cpu_dir) {
  uint32_t online = 1;
  if (!read_uint32_file((cpu_dir + "/online").c_str(), &online)) {
    return true;
  }
  return online != 0;
}

/**
  Build CPU topology (sockets/cores/logical CPUs) from sysfs.

  Not noexcept: does per-CPU std::string/std::map allocation, and is only
  ever called from sql_build_cpu_topology(), which catches exceptions from
  it and degrades to "topology unavailable" instead of terminating.
*/
void build_cpu_topology_from_sysfs(Sql_cpu_topology &topology,
                                   const Process_affinity_mask &affinity) {
  constexpr const char *kCpuSysfsPath = "/sys/devices/system/cpu";

  DIR *dir = ::opendir(kCpuSysfsPath);
  if (dir == nullptr) {
    return;
  }

  /* socket_id -> (core_id -> logical_cpu_ids) */
  std::vector<Sql_cpu_socket> sockets_tmp;

  /* We do not know socket/core ids in advance, use temporary maps. */
  std::map<uint32_t, std::map<uint32_t, std::vector<uint32_t>>> sockets_map;

  while (dirent *entry = ::readdir(dir)) {
    uint32_t cpu_id = 0;

    if (!is_cpu_dir_name(entry->d_name, &cpu_id)) {
      continue;
    }

    if (!cpu_allowed_by_mask(affinity, cpu_id)) {
      /* Excluded by the process's own affinity mask (taskset/cgroup
      cpuset): treat exactly like an offline CPU below, so it never
      contributes a phantom core/socket nor a cpu_to_socket/cpu_to_core
      entry that later cpuset builders could still hand out. */
      continue;
    }

    const std::string cpu_dir =
        std::string(kCpuSysfsPath) + "/" + entry->d_name;

    if (!is_cpu_online(cpu_dir)) {
      /* Do not count offline CPUs as phantom cores: they have no
      topology/ subtree, so falling through would otherwise place each of
      them as its own core on socket 0 and inflate physical_cores. */
      continue;
    }

    const std::string cpu_base = cpu_dir + "/topology/";

    uint32_t socket_id = 0;
    uint32_t core_id = cpu_id;

    const bool has_socket = read_uint32_file(
        (cpu_base + "physical_package_id").c_str(), &socket_id);
    const bool has_core =
        read_uint32_file((cpu_base + "core_id").c_str(), &core_id);

    if (!has_socket) {
      socket_id = 0;
    }

    if (!has_core) {
      core_id = cpu_id;
    }

    sockets_map[socket_id][core_id].push_back(cpu_id);

    ensure_cpu_mapping_size(topology, cpu_id);
    topology.cpu_to_socket[cpu_id] = static_cast<int32_t>(socket_id);
    topology.cpu_to_core[cpu_id] = static_cast<int32_t>(core_id);
  }

  ::closedir(dir);

  topology.sockets.clear();
  topology.sockets.reserve(sockets_map.size());

  for (auto &socket_entry : sockets_map) {
    Sql_cpu_socket socket;
    socket.socket_id = socket_entry.first;
    socket.cores.reserve(socket_entry.second.size());

    for (auto &core_entry : socket_entry.second) {
      Sql_cpu_core core;
      core.core_id = core_entry.first;

      auto &logical_cpu_ids = core_entry.second;
      std::sort(logical_cpu_ids.begin(), logical_cpu_ids.end());
      logical_cpu_ids.erase(
          std::unique(logical_cpu_ids.begin(), logical_cpu_ids.end()),
          logical_cpu_ids.end());

      core.logical_cpu_ids = std::move(logical_cpu_ids);
      socket.cores.push_back(std::move(core));
    }

    topology.sockets.push_back(std::move(socket));
  }

  topology.sockets_count = static_cast<uint32_t>(topology.sockets.size());
}

/** Count physical cores from sockets/cores topology. */
[[nodiscard]] uint32_t count_physical_cores(
    const Sql_cpu_topology &topology) noexcept {
  uint32_t count = 0;

  for (const auto &socket : topology.sockets) {
    count += static_cast<uint32_t>(socket.cores.size());
  }

  return count;
}

/**
  Build a cpu_set_t from a list of logical CPU ids.

  CPUs beyond CPU_SETSIZE are ignored.
*/
[[nodiscard]] bool build_cpuset_from_cpu_list(const std::vector<uint32_t> &cpus,
                                              cpu_set_t *cpuset) noexcept {
  if (cpuset == nullptr) {
    return false;
  }

  CPU_ZERO(cpuset);

  for (uint32_t cpu : cpus) {
    if (cpu >= CPU_SETSIZE) {
      continue;
    }

    CPU_SET(static_cast<int>(cpu), cpuset);
  }

  return CPU_COUNT(cpuset) > 0;
}

/**
  Build NUMA snapshot from /sys/devices/system/node.

  Not noexcept: does per-node std::string/std::vector allocation, and is
  only ever called from sql_build_numa_snapshot(), which catches exceptions
  from it and degrades to "NUMA info unavailable" instead of terminating.
*/
void build_numa_snapshot_from_sysfs(Sql_numa_snapshot &snapshot,
                                    const Process_affinity_mask &affinity) {
  snapshot.clear();

  constexpr const char *kNodeSysfsPath = "/sys/devices/system/node";

  DIR *dir = ::opendir(kNodeSysfsPath);
  if (dir == nullptr) {
    return;
  }

  /* Read online node list if present; otherwise fall back to node* dirs. */
  std::string online_text;

  std::vector<uint32_t> online_nodes;
  if (read_text_file("/sys/devices/system/node/online", online_text) &&
      parse_linux_id_list(online_text, online_nodes)) {
    /* online_nodes is filled. */
  } else {
    /* Discover nodes by scanning nodeX directories. */
    std::vector<uint32_t> discovered_nodes;

    while (dirent *entry = ::readdir(dir)) {
      const char *name = entry->d_name;

      if (name == nullptr || std::strncmp(name, "node", 4) != 0) {
        continue;
      }

      uint32_t node_id = 0;
      if (parse_uint32(name + 4, &node_id)) {
        discovered_nodes.push_back(node_id);
      }
    }

    std::sort(discovered_nodes.begin(), discovered_nodes.end());
    discovered_nodes.erase(
        std::unique(discovered_nodes.begin(), discovered_nodes.end()),
        discovered_nodes.end());

    online_nodes = std::move(discovered_nodes);
  }

  ::closedir(dir);

  if (online_nodes.empty()) {
    /* No NUMA information available. */
    snapshot.initialized = false;
    return;
  }

  /* Build per-node data. */
  std::vector<int32_t> cpu_to_node;
  cpu_to_node.assign(static_cast<size_t>(detect_logical_cpu_count()), -1);

  snapshot.nodes.clear();
  snapshot.nodes.reserve(online_nodes.size());

  for (uint32_t node_id : online_nodes) {
    Sql_numa_node node;
    node.node_id = node_id;

    bool have_any_cpu = false;

    /* cpulist */
    {
      std::string cpulist_path = std::string(kNodeSysfsPath) + "/node" +
                                 std::to_string(node_id) + "/cpulist";
      std::string cpulist_text;

      if (read_text_file(cpulist_path.c_str(), cpulist_text)) {
        if (!parse_linux_id_list(cpulist_text, node.logical_cpu_ids)) {
          node.logical_cpu_ids.clear();
        }
      }
    }

    have_any_cpu = !node.logical_cpu_ids.empty();

    /* Drop CPUs this process isn't allowed to run on (taskset/cgroup
    cpuset). Only a node that *had* CPUs before this filter and lost all of
    them is dropped below (see have_any_cpu use further down): a node that
    was already CPU-less in sysfs (e.g. a CXL memory-only node) is left
    alone here - that is a distinct, deliberate case handled by the
    memory-gate logic elsewhere, not an affinity artifact. */
    node.logical_cpu_ids.erase(
        std::remove_if(node.logical_cpu_ids.begin(),
                       node.logical_cpu_ids.end(),
                       [&affinity](uint32_t cpu_id) {
                         return !cpu_allowed_by_mask(affinity, cpu_id);
                       }),
        node.logical_cpu_ids.end());

    /* distance */
    {
      std::string distance_path = std::string(kNodeSysfsPath) + "/node" +
                                  std::to_string(node_id) + "/distance";
      std::string distance_text;

      if (read_text_file(distance_path.c_str(), distance_text)) {
        if (!parse_distance_vector(distance_text, node.distance)) {
          node.distance.clear();
        }
      }
    }

    /* meminfo */
    {
      std::string meminfo_path = std::string(kNodeSysfsPath) + "/node" +
                                 std::to_string(node_id) + "/meminfo";
      std::string meminfo_text;

      if (read_text_file(meminfo_path.c_str(), meminfo_text)) {
        if (!parse_node_meminfo(meminfo_text, node.mem_total_kb,
                                node.mem_free_kb)) {
          node.mem_total_kb = 0;
          node.mem_free_kb = 0;
        }
      }
    }

    if (have_any_cpu && node.logical_cpu_ids.empty()) {
      /* Every CPU this node had was excluded by the process affinity mask:
      for placement purposes this node does not exist (a container confined
      to node 0 of a 2-node host is, effectively, a single-node machine).
      Skip it entirely rather than keeping a cpu-less entry that
      instance_no % nodes_count could still select. */
      continue;
    }

    /* Update cpu_to_node mapping. */
    for (uint32_t cpu_id : node.logical_cpu_ids) {
      if (cpu_id >= cpu_to_node.size()) {
        cpu_to_node.resize(static_cast<size_t>(cpu_id) + 1, -1);
      }
      cpu_to_node[cpu_id] = static_cast<int32_t>(node_id);
    }

    snapshot.nodes.push_back(std::move(node));
  }

#ifdef HAVE_LIBNUMA
  /* Record the memory-policy-allowed node set (cgroup cpuset.mems / mbind()
  restriction) as advisory metadata for callers that gate memory placement
  (e.g. the NUMA topology auto mode's per-node share check). This is
  independent of the CPU affinity filtering above: a node can be
  CPU-allowed but memory-disallowed, or vice versa. Only queried when at
  least one node survived above; numa_get_mems_allowed() itself is safe to
  call whenever the sysfs node tree exists, matching the guard already used
  by the interleave policy setup in buf0buf.cc. */
  if (!snapshot.nodes.empty()) {
    struct bitmask *mems_allowed = ::numa_get_mems_allowed();
    if (mems_allowed != nullptr) {
      for (auto &node : snapshot.nodes) {
        node.mem_allowed =
            node.node_id < mems_allowed->size &&
            ::numa_bitmask_isbitset(mems_allowed,
                                    static_cast<unsigned int>(node.node_id));
      }
      ::numa_bitmask_free(mems_allowed);
    }
  }
#endif /* HAVE_LIBNUMA */

  snapshot.nodes_count = static_cast<uint32_t>(snapshot.nodes.size());
  snapshot.cpu_to_node = std::move(cpu_to_node);

  snapshot.initialized = snapshot.nodes_count > 0;
}

}  // namespace

void Sql_cpu_core::clear() noexcept {
  core_id = 0;
  logical_cpu_ids.clear();
}

void Sql_cpu_socket::clear() noexcept {
  socket_id = 0;
  cores.clear();
}

void Sql_cpu_topology::clear() noexcept {
  initialized = false;

  logical_cpus = 0;
  physical_cores = 0;
  sockets_count = 0;

  smt_enabled = false;
  threads_per_core = 1;

  sockets.clear();

  cpu_to_socket.clear();
  cpu_to_core.clear();
}

void Sql_numa_node::clear() noexcept {
  node_id = 0;
  logical_cpu_ids.clear();
  distance.clear();
  mem_total_kb = 0;
  mem_free_kb = 0;
  mem_allowed = true;
}

void Sql_numa_snapshot::clear() noexcept {
  initialized = false;
  nodes_count = 0;
  nodes.clear();
  cpu_to_node.clear();
}

void Sql_machine_topology::clear() noexcept {
  cpu.clear();
  numa.clear();
}

/**
  Build CPU topology (sockets/cores/logical CPUs) for the process.

  Internal to this file: the only supported entry point for consumers is
  sql_machine_topology(), which builds this (and the NUMA snapshot below)
  exactly once and hands out an immutable process-wide result.
*/
static void sql_build_cpu_topology(Sql_cpu_topology &topology) noexcept {
  topology.clear();

  try {
    const Process_affinity_mask affinity;

    topology.logical_cpus = detect_logical_cpu_count();

    build_cpu_topology_from_sysfs(topology, affinity);

    topology.physical_cores = count_physical_cores(topology);

    /* Prefer the topology-derived count over the raw sysconf() one
    whenever the sysfs walk produced anything: sysconf(_SC_NPROCESSORS_ONLN)
    counts every online CPU regardless of this process's own affinity mask,
    while the topology sum above already excludes CPUs the affinity mask
    filtered out. Only fall back to the raw count when the topology is
    empty (sysfs unavailable, or - pathologically - the mask excluded every
    online CPU), to avoid collapsing logical_cpus to 0. */
    uint32_t logical_count = 0;

    for (const auto &socket : topology.sockets) {
      for (const auto &core : socket.cores) {
        logical_count += static_cast<uint32_t>(core.logical_cpu_ids.size());
      }
    }

    if (logical_count != 0) {
      topology.logical_cpus = logical_count;
    }
    /* else: keep the raw sysconf() value already assigned above. */

    if (topology.physical_cores == 0 && topology.logical_cpus != 0) {
      /* Fallback: assume 1 thread per core. */
      topology.physical_cores = topology.logical_cpus;
    }

    /* Derive SMT/threads-per-core information. */
    topology.threads_per_core = 1;
    topology.smt_enabled = false;

    if (topology.physical_cores != 0) {
      uint32_t max_threads_per_core = 1;

      for (const auto &socket : topology.sockets) {
        for (const auto &core : socket.cores) {
          const uint32_t threads_here =
              static_cast<uint32_t>(core.logical_cpu_ids.size());
          if (threads_here > max_threads_per_core) {
            max_threads_per_core = threads_here;
          }
        }
      }

      if (max_threads_per_core == 0) {
        max_threads_per_core = 1;
      }

      topology.threads_per_core = max_threads_per_core;

      /* SMT is considered enabled when we have more than 1 logical CPU per
      core. */
      topology.smt_enabled = (topology.threads_per_core > 1) &&
                             (topology.logical_cpus > topology.physical_cores);
    }

    topology.initialized = topology.logical_cpus != 0 ||
                           topology.physical_cores != 0 ||
                           topology.sockets_count != 0;
  } catch (...) {
    /* Catches allocation failures and unexpected exceptions from code that
    runs directly in this try block and in build_cpu_topology_from_sysfs()
    itself (sysfs scanning, per-CPU string/map bookkeeping) -- degrading to
    "topology unavailable" instead of terminating, since this function is
    noexcept and runs on server startup and connection-handling paths.

    This does NOT cover std::bad_alloc thrown from inside a `noexcept`-marked
    helper elsewhere in this file (e.g. read_text_file()/
    parse_linux_id_list()/trim(), used by the NUMA path below): a noexcept
    function that throws internally calls std::terminate() before the
    exception can unwind to any try/catch above it, this one included. That
    path is reachable only under extreme memory exhaustion at server
    startup -- a state this function's degradation logic could not recover
    from either way. */
    topology.clear();
  }
}

/** Internal to this file: see sql_build_cpu_topology()'s comment. */
static void sql_build_numa_snapshot(Sql_numa_snapshot &snapshot) noexcept {
  try {
    const Process_affinity_mask affinity;
    build_numa_snapshot_from_sysfs(snapshot, affinity);
  } catch (...) {
    /* See sql_build_cpu_topology()'s catch block above for the exact scope
    of what this protects: allocation failures in this function's own code
    and in build_numa_snapshot_from_sysfs(), not std::bad_alloc from inside
    the noexcept-marked parsing helpers it calls (read_text_file(),
    parse_linux_id_list(), parse_distance_vector(), parse_node_meminfo(),
    trim()) -- those terminate the process instead of unwinding to this
    catch. */
    snapshot.clear();
  }
}

/** Internal to this file: see sql_build_cpu_topology()'s comment. */
static void sql_build_machine_topology(Sql_machine_topology &topology) noexcept {
  topology.clear();

  sql_build_cpu_topology(topology.cpu);
  sql_build_numa_snapshot(topology.numa);
}

const Sql_machine_topology &sql_machine_topology() noexcept {
  static const Sql_machine_topology topology = [] {
    Sql_machine_topology t{};
    sql_build_machine_topology(t);
    return t;
  }();
  return topology;
}

uint32_t sql_numa_node_count(const Sql_numa_snapshot &snapshot) noexcept {
  if (!snapshot.initialized) {
    return 0;
  }

  return snapshot.nodes_count;
}

const Sql_numa_node *sql_numa_node_by_index(const Sql_numa_snapshot &snapshot,
                                            uint32_t index) noexcept {
  if (!snapshot.initialized) {
    return nullptr;
  }

  if (index >= snapshot.nodes.size()) {
    return nullptr;
  }

  return &snapshot.nodes[index];
}

int32_t sql_numa_cpu_to_node(const Sql_numa_snapshot &snapshot,
                             uint32_t cpu_id) noexcept {
  if (!snapshot.initialized) {
    return -1;
  }

  if (cpu_id >= snapshot.cpu_to_node.size()) {
    return -1;
  }

  return snapshot.cpu_to_node[cpu_id];
}

bool sql_build_cpuset_for_socket(const Sql_cpu_topology &topology,
                                 uint32_t socket_id,
                                 cpu_set_t *cpuset) noexcept {
  if (!topology.initialized || cpuset == nullptr) {
    return false;
  }

  const Sql_cpu_socket *socket_ptr = nullptr;

  for (const auto &socket : topology.sockets) {
    if (socket.socket_id == socket_id) {
      socket_ptr = &socket;
      break;
    }
  }

  if (socket_ptr == nullptr) {
    CPU_ZERO(cpuset);
    return false;
  }

  std::vector<uint32_t> cpus;

  for (const auto &core : socket_ptr->cores) {
    cpus.insert(cpus.end(), core.logical_cpu_ids.begin(),
                core.logical_cpu_ids.end());
  }

  return build_cpuset_from_cpu_list(cpus, cpuset);
}

bool sql_build_cpuset_for_numa_node(const Sql_numa_snapshot &snapshot,
                                    uint32_t node_id,
                                    cpu_set_t *cpuset) noexcept {
  if (!snapshot.initialized || cpuset == nullptr) {
    return false;
  }

  const Sql_numa_node *node_ptr = nullptr;

  for (const auto &node : snapshot.nodes) {
    if (node.node_id == node_id) {
      node_ptr = &node;
      break;
    }
  }

  if (node_ptr == nullptr) {
    CPU_ZERO(cpuset);
    return false;
  }

  return build_cpuset_from_cpu_list(node_ptr->logical_cpu_ids, cpuset);
}

/**
  Build a CPU set for a logical instance index.

  The mapping prefers NUMA node, then socket, then flat logical CPU fallback.
*/
bool sql_build_cpuset_for_instance(const Sql_machine_topology &topology,
                                   uint32_t instance_no,
                                   cpu_set_t *cpuset) noexcept {
  if (cpuset == nullptr) {
    return false;
  }

  CPU_ZERO(cpuset);

  const Sql_numa_snapshot &numa = topology.numa;
  const Sql_cpu_topology &cpu = topology.cpu;

  /* Prefer NUMA node if available. */
  if (numa.initialized && numa.nodes_count > 0) {
    const uint32_t index = instance_no % numa.nodes_count;
    const Sql_numa_node *node = sql_numa_node_by_index(numa, index);
    if (node != nullptr &&
        build_cpuset_from_cpu_list(node->logical_cpu_ids, cpuset)) {
      return true;
    }
    /* If NUMA mapping failed, fall through to socket/flat mapping. */
    CPU_ZERO(cpuset);
  }

  /* Next prefer CPU socket if available. */
  if (cpu.initialized && cpu.sockets_count > 0) {
    const uint32_t socket_index = instance_no % cpu.sockets_count;
    const Sql_cpu_socket &socket = cpu.sockets[socket_index];

    std::vector<uint32_t> cpus;
    for (const auto &core : socket.cores) {
      cpus.insert(cpus.end(), core.logical_cpu_ids.begin(),
                  core.logical_cpu_ids.end());
    }

    if (build_cpuset_from_cpu_list(cpus, cpuset)) {
      return true;
    }
    CPU_ZERO(cpuset);
  }

  /* Flat logical CPU fallback. */
  const uint32_t logical_cpus =
      cpu.logical_cpus != 0 ? cpu.logical_cpus : detect_logical_cpu_count();

  if (logical_cpus == 0) {
    return false;
  }

  const uint32_t cpu_id = instance_no % logical_cpus;
  if (cpu_id >= CPU_SETSIZE) {
    return false;
  }

  CPU_SET(static_cast<int>(cpu_id), cpuset);
  return CPU_COUNT(cpuset) > 0;
}

bool sql_numa_node_for_instance(const Sql_numa_snapshot &numa,
                                uint32_t instance_no,
                                uint32_t *node_id) noexcept {
  if (node_id == nullptr || !numa.initialized || numa.nodes_count == 0) {
    return false;
  }

  const uint32_t index = instance_no % numa.nodes_count;
  const Sql_numa_node *node = sql_numa_node_by_index(numa, index);
  if (node == nullptr) {
    return false;
  }

  *node_id = node->node_id;
  return true;
}
