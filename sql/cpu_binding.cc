#include "cpu_binding.h"
#include "cpu_topology.h"
#include "log.h"

#include <pthread.h>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

struct CpuBindingParsedMap {
  std::map<ThreadRole, CpuBindingEntry> entries;
  CpuBindingParsedMap() = default;
};

static CpuBindingParsedMap g_cpu_binding_parsed;

struct RoleBindingPlan {
  bool initialized;
  cpu_set_t cpuset;  // used for ANY/FIXED
  unsigned long bound_threads;
  int last_socket;  // last socket used for SPARSE

  RoleBindingPlan() : initialized(false), bound_threads(0), last_socket(-1) {
    CPU_ZERO(&cpuset);
  }
};

static std::map<ThreadRole, RoleBindingPlan> g_role_plan;

static std::vector<cpu_set_t> g_socket_cpusets;
static bool g_socket_cpusets_initialized = false;

/** Local cached CPU topology snapshot. */
struct TopologyCache {
  Sql_cpu_topology topology{};
  bool initialized{false};
  bool valid{false};
};

static TopologyCache g_topology_cache;

/** Obtain cached CPU topology, initializing it on first use. */
static const Sql_cpu_topology *get_cpu_topology() {
#ifdef __linux__
  if (!g_topology_cache.initialized) {
    sql_build_cpu_topology(g_topology_cache.topology);
    g_topology_cache.initialized = true;
    g_topology_cache.valid = g_topology_cache.topology.initialized;
  }

  return g_topology_cache.valid ? &g_topology_cache.topology : nullptr;
#else
  return nullptr;
#endif
}

static bool build_cpuset_for_socket(int socket, cpu_set_t &cpuset) {
#ifdef __linux__
  const Sql_cpu_topology *topology = get_cpu_topology();
  if (topology == nullptr) {
    sql_print_warning("cpu_binding: CPU topology is not available");
    return false;
  }

  const auto &sockets = topology->sockets;
  if (socket < 0 || socket >= static_cast<int>(sockets.size())) {
    sql_print_warning(
        "cpu_binding: requested socket %d out of range (sockets=%zu)", socket,
        sockets.size());
    return false;
  }

  CPU_ZERO(&cpuset);

  const Sql_cpu_socket &sock = sockets[static_cast<size_t>(socket)];
  for (const Sql_cpu_core &core : sock.cores) {
    for (uint32_t cpu_id : core.logical_cpu_ids) {
      if (cpu_id < static_cast<uint32_t>(CPU_SETSIZE)) {
        CPU_SET(static_cast<int>(cpu_id), &cpuset);
      }
    }
  }

  if (CPU_COUNT(&cpuset) == 0) {
    sql_print_warning("cpu_binding: socket %d has no logical CPUs in topology",
                      socket);
    return false;
  }

  return true;
#else
  (void)socket;
  (void)cpuset;
  return false;
#endif
}

static bool init_socket_cpusets() {
#ifdef __linux__
  if (g_socket_cpusets_initialized) {
    return !g_socket_cpusets.empty();
  }

  const Sql_cpu_topology *topology = get_cpu_topology();
  if (topology == nullptr) {
    sql_print_warning("cpu_binding: CPU topology is not available");
    g_socket_cpusets_initialized = true;
    return false;
  }

  const auto &sockets = topology->sockets;
  g_socket_cpusets.clear();

  if (sockets.empty()) {
    sql_print_warning("cpu_binding: no sockets in topology");
    g_socket_cpusets_initialized = true;
    return false;
  }

  g_socket_cpusets.resize(sockets.size());
  for (size_t i = 0; i < sockets.size(); ++i) {
    cpu_set_t set;
    if (!build_cpuset_for_socket(static_cast<int>(i), set)) {
      CPU_ZERO(&g_socket_cpusets[i]);
      continue;
    }
    g_socket_cpusets[i] = set;
  }

  g_socket_cpusets_initialized = true;
  return true;
#else
  return false;
#endif
}

static std::string cpu_binding_trim_spaces(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
    ++start;

  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    --end;

  return s.substr(start, end - start);
}

static const char *cpu_binding_mode_name(BindingMode mode) {
  switch (mode) {
    case BindingMode::ANY:
      return "ANY";
    case BindingMode::SPARSE:
      return "SPARSE";
    case BindingMode::FIXED:
      return "FIXED";
    default:
      return "UNKNOWN_MODE";
  }
}

static const char *cpu_binding_role_name(ThreadRole role) {
  switch (role) {
    case ThreadRole::MAIN_THREAD:
      return "MAIN_THREAD";
    case ThreadRole::REPLICA_IO:
      return "REPLICA_IO";
    case ThreadRole::REPLICA_APPLIER:
      return "REPLICA_APPLIER";
    case ThreadRole::REPLICA_WORKER:
      return "REPLICA_WORKER";
    case ThreadRole::CLIENT_THREAD:
      return "CLIENT_THREAD";
    default:
      return "UNKNOWN_THREAD_ROLE";
  }
}

/**
  Build cpuset for a given role and binding entry.

  ANY:    union of all sockets.
  FIXED:  cpuset of specified socket.
  SPARSE: union of all sockets (selection is done in apply).
*/
static bool cpu_binding_build_cpuset_chain(ThreadRole role,
                                           const CpuBindingEntry &entry,
                                           cpu_set_t &cpuset,
                                           unsigned long planned_threads) {
#ifdef __linux__
  (void)planned_threads;

  const Sql_cpu_topology *topology = get_cpu_topology();
  if (topology == nullptr) {
    return false;
  }
  if (topology->sockets.empty()) {
    return false;
  }

  if (!init_socket_cpusets()) {
    sql_print_warning(
        "cpu_binding: no socket CPU sets available, skipping binding for "
        "role=%s",
        cpu_binding_role_name(role));
    return false;
  }

  if (entry.mode == BindingMode::ANY) {
    cpu_set_t new_set;
    CPU_ZERO(&new_set);

    for (const cpu_set_t &s : g_socket_cpusets) {
      for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &s)) {
          CPU_SET(cpu, &new_set);
        }
      }
    }

    if (CPU_COUNT(&new_set) == 0) {
      sql_print_warning("cpu_binding: role=%s ANY produced empty cpuset",
                        cpu_binding_role_name(role));
      return false;
    }

    cpuset = new_set;
    return true;
  }

  if (entry.mode == BindingMode::FIXED) {
    int idx = entry.socket;
    if (idx < 0 || idx >= static_cast<int>(g_socket_cpusets.size())) {
      sql_print_warning(
          "cpu_binding: role=%s FIXED socket=%d is out of range (sockets=%zu)",
          cpu_binding_role_name(role), idx, g_socket_cpusets.size());
      return false;
    }

    const cpu_set_t &socket_set = g_socket_cpusets[static_cast<size_t>(idx)];
    if (CPU_COUNT(&socket_set) == 0) {
      sql_print_warning(
          "cpu_binding: role=%s FIXED socket=%d produced empty cpuset",
          cpu_binding_role_name(role), idx);
      return false;
    }

    cpuset = socket_set;
    return true;
  }

  if (entry.mode == BindingMode::SPARSE) {
    if (role == ThreadRole::CLIENT_THREAD) {
      return CPU_COUNT(&cpuset) > 0;
    }

    cpu_set_t new_set;
    CPU_ZERO(&new_set);

    for (const cpu_set_t &s : g_socket_cpusets) {
      for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &s)) {
          CPU_SET(cpu, &new_set);
        }
      }
    }

    if (CPU_COUNT(&new_set) == 0) {
      sql_print_warning("cpu_binding: role=%s SPARSE produced empty cpuset",
                        cpu_binding_role_name(role));
      return false;
    }

    cpuset = new_set;
    return true;
  }

  return false;
#else
  (void)role;
  (void)entry;
  (void)cpuset;
  (void)planned_threads;
  return false;
#endif
}

/* Public API: apply binding for role; planned_threads is for SPARSE logic. */
void cpu_binding_apply_for_role(ThreadRole role, pthread_t thread,
                                unsigned long planned_threads) {
#ifdef __linux__
  auto it = g_cpu_binding_parsed.entries.find(role);
  if (it == g_cpu_binding_parsed.entries.end()) {
    return;
  }

  const CpuBindingEntry &entry = it->second;
  RoleBindingPlan &plan = g_role_plan[role];

  if (entry.mode != BindingMode::SPARSE) {
    if (!plan.initialized) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);

      if (!cpu_binding_build_cpuset_chain(role, entry, cpuset,
                                          planned_threads)) {
        sql_print_warning(
            "cpu_binding: empty cpuset for role=%s, skipping affinity",
            cpu_binding_role_name(role));
        return;
      }

      plan.cpuset = cpuset;
      plan.initialized = true;

      sql_print_information(
          "cpu_binding: prepared role=%s mode=%s socket=%d cpu_threads=%d",
          cpu_binding_role_name(role), cpu_binding_mode_name(entry.mode),
          entry.socket, CPU_COUNT(&plan.cpuset));
    }

    int err = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &plan.cpuset);
    if (err != 0) {
      sql_print_warning(
          "Failed to set CPU affinity (cached) for thread role=%d "
          "(mode=%s, socket=%d), error=%d",
          static_cast<int>(role), cpu_binding_mode_name(entry.mode),
          entry.socket, err);
    }
    return;
  }

  if (!init_socket_cpusets()) {
    sql_print_warning(
        "cpu_binding: topology not initialized for SPARSE role=%s",
        cpu_binding_role_name(role));
    return;
  }
  if (g_socket_cpusets.empty()) {
    return;
  }

  if (!plan.initialized) {
    plan.initialized = true;
    plan.bound_threads = 0;
    plan.last_socket = -1;
  }

  int num_sockets = static_cast<int>(g_socket_cpusets.size());
  int next_socket =
      (plan.last_socket < 0) ? 0 : (plan.last_socket + 1) % num_sockets;

  const cpu_set_t &cpuset = g_socket_cpusets[static_cast<size_t>(next_socket)];
  if (CPU_COUNT(&cpuset) == 0) {
    sql_print_warning("cpu_binding: SPARSE socket=%d has empty cpuset",
                      next_socket);
    return;
  }

  int err = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  if (err != 0) {
    sql_print_warning(
        "Failed to set CPU affinity (SPARSE) for thread role=%d "
        "(socket=%d), error=%d",
        static_cast<int>(role), next_socket, err);
    return;
  }

  sql_print_information(
      "cpu_binding: applied role=%s mode=%s socket=%d cpus=%d",
      cpu_binding_role_name(role), cpu_binding_mode_name(entry.mode),
      next_socket, CPU_COUNT(&cpuset));

  plan.bound_threads++;
  plan.last_socket = next_socket;
#else
  (void)role;
  (void)thread;
  (void)planned_threads;
#endif
}

bool parse_cpu_binding_string(const std::string &raw, CpuBindingEntry &out) {
  std::string s = cpu_binding_trim_spaces(raw);

  if (s.empty() || s == "any") {
    out.mode = BindingMode::ANY;
    out.socket = -1;
    return true;
  }

  if (s == "sparse") {
    out.mode = BindingMode::SPARSE;
    out.socket = -1;
    return true;
  }

  char *end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0' || v < 0) {
    return false;
  }

  out.mode = BindingMode::FIXED;
  out.socket = static_cast<int>(v);
  return true;
}

static const char *cpu_binding_thread_role_option_name(ThreadRole role) {
  switch (role) {
    case ThreadRole::MAIN_THREAD:
      return "thread_affinity_main";
    case ThreadRole::REPLICA_IO:
      return "thread_affinity_rpl";
    case ThreadRole::REPLICA_APPLIER:
      return "thread_affinity_rpl";
    case ThreadRole::REPLICA_WORKER:
      return "thread_affinity_rpl";
    case ThreadRole::CLIENT_THREAD:
      return "thread_affinity_client";
    default:
      return "thread_affinity_unknown";
  }
}

void cpu_binding_register_option(ThreadRole role, const char *raw_value) {
  if (raw_value == nullptr) {
    return;
  }

  const char *option_name = cpu_binding_thread_role_option_name(role);

  CpuBindingEntry entry;
  std::string raw(raw_value);

  if (!parse_cpu_binding_string(raw, entry)) {
    sql_print_warning(
        "Invalid value for %s: '%s', "
        "expected numeric socket id, 'any' or 'sparse'.",
        option_name, raw_value);
    return;
  }

  g_cpu_binding_parsed.entries[role] = entry;

  sql_print_information("cpu_binding: parsed %s='%s' as mode=%s socket=%d",
                        option_name, raw.c_str(),
                        cpu_binding_mode_name(entry.mode), entry.socket);
}

int cpu_binding_get_role_socket(ThreadRole role) {
  auto it = g_cpu_binding_parsed.entries.find(role);
  if (it == g_cpu_binding_parsed.entries.end()) {
    return -1;
  }

  const CpuBindingEntry &entry = it->second;
  if (entry.mode != BindingMode::FIXED) {
    return -1;
  }

  return entry.socket;
}

int cpu_binding_get_socket_cores(int socket_id) {
#ifdef __linux__
  const Sql_cpu_topology *topology = get_cpu_topology();
  if (topology == nullptr) {
    return -1;
  }

  const auto &sockets = topology->sockets;
  if (socket_id < 0 || static_cast<size_t>(socket_id) >= sockets.size()) {
    return -1;
  }

  return static_cast<int>(sockets[static_cast<size_t>(socket_id)].cores.size());
#else
  (void)socket_id;
  return -1;
#endif
}
