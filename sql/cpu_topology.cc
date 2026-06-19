#include "cpu_topology.h"

#include <dirent.h>
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

namespace {

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

/** Build CPU topology (sockets/cores/logical CPUs) from sysfs. */
void build_cpu_topology_from_sysfs(Sql_cpu_topology &topology) noexcept {
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

    const std::string cpu_base =
        std::string(kCpuSysfsPath) + "/" + entry->d_name + "/topology/";

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

/** Build NUMA snapshot from /sys/devices/system/node. */
void build_numa_snapshot_from_sysfs(Sql_numa_snapshot &snapshot) noexcept {
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

    /* Update cpu_to_node mapping. */
    for (uint32_t cpu_id : node.logical_cpu_ids) {
      if (cpu_id >= cpu_to_node.size()) {
        cpu_to_node.resize(static_cast<size_t>(cpu_id) + 1, -1);
      }
      cpu_to_node[cpu_id] = static_cast<int32_t>(node_id);
    }

    snapshot.nodes.push_back(std::move(node));
  }

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

void sql_build_cpu_topology(Sql_cpu_topology &topology) noexcept {
  topology.clear();

  topology.logical_cpus = detect_logical_cpu_count();

  build_cpu_topology_from_sysfs(topology);

  topology.physical_cores = count_physical_cores(topology);

  if (topology.logical_cpus == 0) {
    /* Derive logical_cpus from topology when sysconf failed. */
    uint32_t logical_count = 0;

    for (const auto &socket : topology.sockets) {
      for (const auto &core : socket.cores) {
        logical_count += static_cast<uint32_t>(core.logical_cpu_ids.size());
      }
    }

    topology.logical_cpus = logical_count;
  }

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

    /* SMT is considered enabled when we have more than 1 logical CPU per core.
     */
    topology.smt_enabled = (topology.threads_per_core > 1) &&
                           (topology.logical_cpus > topology.physical_cores);
  }

  topology.initialized = topology.logical_cpus != 0 ||
                         topology.physical_cores != 0 ||
                         topology.sockets_count != 0;
}

void sql_build_numa_snapshot(Sql_numa_snapshot &snapshot) noexcept {
  build_numa_snapshot_from_sysfs(snapshot);
}

void sql_build_machine_topology(Sql_machine_topology &topology) noexcept {
  topology.clear();

  sql_build_cpu_topology(topology.cpu);
  sql_build_numa_snapshot(topology.numa);
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
