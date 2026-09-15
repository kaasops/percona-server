#include "cpu_binding.h"
#include "cpu_topology.h"
#include "log.h"

#include <pthread.h>
#include <map>
#include <array>
#include <mutex>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <atomic>

struct CpuBindingParsedMap
  {
  std::map<ThreadRole, CpuBindingEntry> entries;
  CpuBindingParsedMap() = default;
  };

static CpuBindingParsedMap g_cpu_binding_parsed;

struct RoleBindingPlan
  {
  std::once_flag once;
  bool valid = false;        // plan was built successfully
  cpu_set_t cpuset;          // used for ANY/FIXED; read-only after `once`
  std::atomic<unsigned long> sparse_ticket{0}; // SPARSE round-robin position

  RoleBindingPlan()
  {
    CPU_ZERO(&cpuset);
  }
  };

static std::array<RoleBindingPlan,
                   static_cast<size_t>(ThreadRole::ROLE_COUNT)> g_role_plans;
//
static std::vector<cpu_set_t> g_socket_cpusets;
static std::once_flag g_socket_cpusets_once;
static bool g_socket_cpusets_ok = false;
//
static bool
build_cpuset_for_socket(int socket, cpu_set_t &cpuset) {
#ifdef __linux__

  if (!sql_cpu_topology.initialized) {
    sql_cpu_topology_init(&sql_cpu_topology);
  }

  const auto &sockets = sql_cpu_topology.sockets;
  if (socket < 0 ||
      socket >= static_cast<int>(sockets.size())) {
    sql_print_warning(
      "cpu_binding: requested socket %d out of range (sockets=%zu)",
      socket, sockets.size());
    return false;
  }

  CPU_ZERO(&cpuset);

  const Sql_cpu_socket &sock = sockets[static_cast<size_t>(socket)];
  for (const Sql_cpu_core &core : sock.cores) {
    for (const Sql_cpu_thread &t : core.threads) {
      CPU_SET(t.cpu_id, &cpuset);
    }
  }

  if (CPU_COUNT(&cpuset) == 0) {
    sql_print_warning(
      "cpu_binding: socket %d has no logical CPUs in topology",
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

static bool
init_socket_cpusets() {
#ifdef __linux__
  std::call_once(g_socket_cpusets_once, [] {
    if (!sql_cpu_topology.initialized) {
      sql_cpu_topology_init(&sql_cpu_topology);
    }

    const auto &sockets = sql_cpu_topology.sockets;
    g_socket_cpusets.clear();

    if (sockets.empty()) {
      sql_print_warning("cpu_binding: no sockets in topology");
      g_socket_cpusets_ok = false;
      return;
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

    g_socket_cpusets_ok = true;
  });

  return g_socket_cpusets_ok;
#else
  return false;
#endif
}

static std::string cpu_binding_trim_spaces(const std::string &s) {
  size_t start = 0;
  while (start < s.size() &&
    std::isspace(static_cast<unsigned char>(s[start])))
    ++start;

  size_t end = s.size();
  while (end > start &&
    std::isspace(static_cast<unsigned char>(s[end - 1])))
    --end;

  return s.substr(start, end - start);
  }


static const char *cpu_binding_mode_name(BindingMode mode) {
  switch (mode) {
    case BindingMode::ANY:    return "ANY";
    case BindingMode::SPARSE: return "SPARSE";
    case BindingMode::FIXED:  return "FIXED";
    default:                  return "UNKNOWN_MODE";
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
    case ThreadRole::BUFPOOL_LRU_T:
      return "BUFPOOL_LRU_T";
    default:
      return "UNKNOWN_THREAD_ROLE";
    }
  }


using CpuFilter = void(*)(cpu_set_t &cpuset,
const CpuBindingEntry &entry,
ThreadRole role);

static bool cpu_binding_build_cpuset_chain(ThreadRole role,
const CpuBindingEntry &entry,
cpu_set_t &cpuset,
unsigned long planned_threads) {

  sql_print_information(
    "cpu_binding: start role=%s planned_threads=%lu, mode=%s",
    cpu_binding_role_name(role),
    planned_threads, cpu_binding_mode_name(entry.mode));

#ifdef __linux__
  // Every binding mode reads g_socket_cpusets.
  init_socket_cpusets();

  if (!sql_cpu_topology.initialized) {
    sql_cpu_topology_init(&sql_cpu_topology);
    }
  if (sql_cpu_topology.sockets.empty()) {
    return false;
    }

// -- BindingMode::ANY
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
      sql_print_warning(
        "cpu_binding: role=%s ANY produced empty cpuset",
        cpu_binding_role_name(role));
      return false;
    }

    cpuset = new_set;
    return true;
  }
// -- BindingMode::ANY


// -- BindingMode::FIXED
  if (entry.mode == BindingMode::FIXED) {
    int idx = entry.socket;
    if (idx < 0 ||
        idx >= static_cast<int>(g_socket_cpusets.size())) {
      sql_print_warning(
        "cpu_binding: role=%s FIXED socket=%d is out of range (sockets=%zu)",
        cpu_binding_role_name(role),
        idx,
        g_socket_cpusets.size());
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
// -- BindingMode::FIXED


// -- BindingMode::SPARSE
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
      sql_print_warning(
        "cpu_binding: role=%s SPARSE produced empty cpuset",
        cpu_binding_role_name(role));
      return false;
    }

    cpuset = new_set;
    return true;
  }
// -- BindingMode::SPARSE

  return false;
#else
  (void)role;
  (void)entry;
  (void)cpuset;
  return false;
#endif
  }


// Public API: apply binding for role; planned_threads is for SPARSE logic.
void cpu_binding_apply_for_role(ThreadRole role,
pthread_t thread,
unsigned long planned_threads) {

  sql_print_information("cpu_binding_apply_for_role(raw): role=%s, planned_threads=%lu",
    cpu_binding_role_name(role), planned_threads);

#ifdef __linux__
  auto it = g_cpu_binding_parsed.entries.find(role);
  if (it == g_cpu_binding_parsed.entries.end()) {
    sql_print_information("cpu_binding_apply_for_role(g_cpu_binding_parsed): g_cpu_binding_parsed.entries.end() has reached, role=%s",
      cpu_binding_role_name(role));
    return;
  }

  const CpuBindingEntry &entry = it->second;
  RoleBindingPlan &plan = g_role_plans[static_cast<size_t>(role)];

  if (entry.mode != BindingMode::SPARSE) {
    std::call_once(plan.once, [&] {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);

      unsigned long logical_cpus = sql_cpu_get_logical();
      for (unsigned long cpu = 0; cpu < logical_cpus; ++cpu) {
        CPU_SET(static_cast<int>(cpu), &cpuset);
      }

      if (!cpu_binding_build_cpuset_chain(role, entry, cpuset,
        planned_threads)) {
        sql_print_warning(
          "cpu_binding: empty cpuset for role=%s, skipping affinity",
          cpu_binding_role_name(role));
        plan.valid = false;
        return;
      }

      plan.cpuset = cpuset;
      plan.valid = true;
    });

    if (!plan.valid) {
      // Already warned; the topology is fixed, so do not retry.
      return;
    }

    int err = pthread_setaffinity_np(thread,
      sizeof(cpu_set_t),
      &plan.cpuset);
    if (err != 0) {
      sql_print_warning(
        "Failed to set CPU affinity (cached) for thread role=%d "
        "(mode=%s, socket=%d), error=%d",
        static_cast<int>(role),
        cpu_binding_mode_name(entry.mode),
        entry.socket,
        err);
    }
    return;
  }

  // SPARSE: round-robin over sockets using cached per-socket cpusets.
  if (!init_socket_cpusets()) {
    sql_print_warning(
      "cpu_binding: topology not initialized for SPARSE role=%s",
      cpu_binding_role_name(role));
    return;
  }
  if (g_socket_cpusets.empty()) {
    return;
  }

  size_t num_sockets = g_socket_cpusets.size();
  size_t idx = static_cast<size_t>(
    plan.sparse_ticket.fetch_add(1, std::memory_order_relaxed) % num_sockets);

  const cpu_set_t *cpuset = nullptr;
  for (size_t attempt = 0; attempt < num_sockets; ++attempt) {
    size_t candidate = (idx + attempt) % num_sockets;
    if (CPU_COUNT(&g_socket_cpusets[candidate]) > 0) {
      cpuset = &g_socket_cpusets[candidate];
      idx = candidate;
      break;
    }
  }

  if (cpuset == nullptr) {
    sql_print_warning(
      "cpu_binding: SPARSE socket=%zu has empty cpuset",
      idx);
    return;
  }

  int err = pthread_setaffinity_np(thread,
    sizeof(cpu_set_t),
    cpuset);
  if (err != 0) {
    sql_print_warning(
      "Failed to set CPU affinity (SPARSE) for thread role=%d "
      "(socket=%zu), error=%d",
      static_cast<int>(role),
      idx,
      err);
    return;
  }
#else
  (void)role;
  (void)thread;
  (void)planned_threads;
#endif
}

bool parse_cpu_binding_string(const std::string &raw,
                              CpuBindingEntry &out) {
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
    case ThreadRole::BUFPOOL_LRU_T:
      return "thread_affinity_bp_lru";
    default:
      return "thread_affinity_unknown";
    }
  }


void cpu_binding_register_option(ThreadRole role, const char *raw_value) {

  sql_print_warning("cpu_binding_register_option(): role=%s, raw_value=%s",
    cpu_binding_role_name(role), raw_value);

  if (raw_value == nullptr)
    return;

  const char *option_name = cpu_binding_thread_role_option_name(role);

  CpuBindingEntry entry;
  std::string raw(raw_value);

  if (!parse_cpu_binding_string(raw, entry)) {
    sql_print_warning(
      "Invalid value for %s: '%s', "
      "expected 'socket,core,thread' with numbers or 'any' or 'sparse'.",
      option_name, raw_value);
    return;
    }

  g_cpu_binding_parsed.entries[role] = entry;

  sql_print_information(
    "Parsed %s='%s' as mode=%s socket=%d",
    option_name, raw.c_str(),
    cpu_binding_mode_name(entry.mode), entry.socket);

  }
