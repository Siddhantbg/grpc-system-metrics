// telemetry_server.cpp
// gRPC server that collects real-time system metrics from the Linux
// kernel's pseudo-filesystems (/proc/stat, /proc/meminfo) and the
// POSIX statvfs() call, then exposes them over MetricsService.

#include <grpcpp/grpcpp.h>
#include "metrics.grpc.pb.h"

#include <sys/statvfs.h>   // statvfs() for disk statistics

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using telemetry::MetricsRequest;
using telemetry::MetricsResponse;
using telemetry::MetricsService;

// ═══════════════════════════════════════════════════════════════
// CPU helpers  –  /proc/stat
//
// The first line of /proc/stat has the form:
//   cpu  <user> <nice> <system> <idle> <iowait> <irq> <softirq> <steal> …
// All fields are cumulative ticks since boot.  We take two
// snapshots 500 ms apart and compute the fraction of non-idle ticks.
// ═══════════════════════════════════════════════════════════════

struct CpuSnapshot {
    long long user     = 0;
    long long nice     = 0;
    long long system   = 0;
    long long idle     = 0;
    long long iowait   = 0;
    long long irq      = 0;
    long long softirq  = 0;
    long long steal    = 0;

    // Sum of all tick categories (denominator for usage %)
    long long total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }

    // Ticks the CPU was idle (iowait is counted as idle for our purposes)
    long long idleTotal() const { return idle + iowait; }
};

// Read the aggregate CPU line from /proc/stat into a CpuSnapshot.
// Returns false if the file cannot be opened or parsed.
static bool readCpuSnapshot(CpuSnapshot& snap) {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) {
        std::cerr << "[Server] ERROR: cannot open /proc/stat\n";
        return false;
    }

    std::string line;
    if (!std::getline(file, line)) return false;

    // Expected prefix: "cpu  "
    std::istringstream iss(line);
    std::string label;
    iss >> label  // "cpu"
        >> snap.user >> snap.nice >> snap.system >> snap.idle
        >> snap.iowait >> snap.irq >> snap.softirq >> snap.steal;

    return !iss.fail();
}

// Sample CPU usage over a 500 ms window.
// Returns the busy percentage in [0, 100], or -1.0 on error.
static double getCpuUsagePercent() {
    CpuSnapshot s1, s2;

    if (!readCpuSnapshot(s1)) return -1.0;

    // Sleep briefly so the two snapshots bracket a meaningful interval.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (!readCpuSnapshot(s2)) return -1.0;

    const long long totalDiff = s2.total()     - s1.total();
    const long long idleDiff  = s2.idleTotal() - s1.idleTotal();

    if (totalDiff <= 0) return 0.0;

    return (1.0 - static_cast<double>(idleDiff) / totalDiff) * 100.0;
}

// ═══════════════════════════════════════════════════════════════
// Memory helpers  –  /proc/meminfo
//
// We read MemTotal and MemAvailable (preferred over MemFree because
// Available accounts for reclaimable page-cache and slabs).
// Used = MemTotal − MemAvailable
// ═══════════════════════════════════════════════════════════════

struct MemSnapshot {
    uint64_t totalKb     = 0;
    uint64_t availableKb = 0;   // "MemAvailable" in /proc/meminfo
};

// Parse /proc/meminfo and populate a MemSnapshot.
// Returns false if the file cannot be opened or the required keys
// are missing.
static bool readMemSnapshot(MemSnapshot& snap) {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) {
        std::cerr << "[Server] ERROR: cannot open /proc/meminfo\n";
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string key;
        uint64_t    value = 0;
        std::string unit;          // "kB"

        iss >> key >> value >> unit;

        if      (key == "MemTotal:")     snap.totalKb     = value;
        else if (key == "MemAvailable:") snap.availableKb = value;

        // Early-exit once both fields are found
        if (snap.totalKb && snap.availableKb) break;
    }

    return snap.totalKb > 0;
}

// ═══════════════════════════════════════════════════════════════
// Disk helpers  –  statvfs(3)
//
// We query the root filesystem "/".  f_bfree counts blocks
// available to a superuser; for the actual used space we subtract
// f_bfree (not f_bavail) from f_blocks so that reserved blocks are
// counted as used.
// ═══════════════════════════════════════════════════════════════

struct DiskSnapshot {
    uint64_t totalBytes = 0;
    uint64_t usedBytes  = 0;
    uint64_t freeBytes  = 0;
};

// Populate a DiskSnapshot for the filesystem mounted at `path`.
// Returns false if statvfs() fails.
static bool readDiskSnapshot(DiskSnapshot& snap, const char* path = "/") {
    struct statvfs vfs{};
    if (::statvfs(path, &vfs) != 0) {
        std::cerr << "[Server] ERROR: statvfs() failed for path: " << path << "\n";
        return false;
    }

    snap.totalBytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    snap.freeBytes  = static_cast<uint64_t>(vfs.f_bfree)  * vfs.f_frsize;
    snap.usedBytes  = snap.totalBytes - snap.freeBytes;

    return snap.totalBytes > 0;
}

// ═══════════════════════════════════════════════════════════════
// Timestamp helper
// ═══════════════════════════════════════════════════════════════

static std::string currentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    // std::localtime is not thread-safe on all platforms; use a mutex
    // in a high-throughput server.  For this project it is acceptable.
    oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════
// gRPC Service Implementation
// ═══════════════════════════════════════════════════════════════

class MetricsServiceImpl final : public MetricsService::Service {
public:
    // Handle a single unary GetSystemMetrics call.
    Status GetSystemMetrics(ServerContext*        context,
                            const MetricsRequest* request,
                            MetricsResponse*      response) override
    {
        // Log the caller's identifier when provided
        if (!request->client_id().empty()) {
            std::cout << "[Server] Request from client: \""
                      << request->client_id() << "\"\n";
        }

        // ── 1. CPU ──────────────────────────────────────────────
        const double cpuPct = getCpuUsagePercent();
        if (cpuPct < 0.0) {
            return Status(grpc::StatusCode::INTERNAL,
                          "Failed to read CPU statistics from /proc/stat");
        }
        response->set_cpu_usage_percent(cpuPct);

        // ── 2. Memory ───────────────────────────────────────────
        MemSnapshot mem;
        if (!readMemSnapshot(mem)) {
            return Status(grpc::StatusCode::INTERNAL,
                          "Failed to read memory statistics from /proc/meminfo");
        }
        const uint64_t usedKb = mem.totalKb - mem.availableKb;
        const double memPct   = static_cast<double>(usedKb) /
                                static_cast<double>(mem.totalKb) * 100.0;

        response->set_memory_usage_percent(memPct);
        response->set_total_memory_mb(mem.totalKb / 1024ULL);
        response->set_used_memory_mb(usedKb       / 1024ULL);

        // ── 3. Disk ─────────────────────────────────────────────
        DiskSnapshot disk;
        if (!readDiskSnapshot(disk)) {
            return Status(grpc::StatusCode::INTERNAL,
                          "Failed to read disk statistics via statvfs()");
        }
        const double diskPct = static_cast<double>(disk.usedBytes) /
                               static_cast<double>(disk.totalBytes) * 100.0;

        response->set_disk_usage_percent(diskPct);
        response->set_total_disk_gb(disk.totalBytes / (1024ULL * 1024 * 1024));
        response->set_used_disk_gb (disk.usedBytes  / (1024ULL * 1024 * 1024));

        // ── 4. Timestamp ────────────────────────────────────────
        const std::string ts = currentTimestamp();
        response->set_timestamp(ts);

        std::cout << "[Server] Metrics sampled at " << ts
                  << "  CPU=" << std::fixed << std::setprecision(1) << cpuPct << "%"
                  << "  MEM=" << memPct << "%"
                  << "  DSK=" << diskPct << "%\n";

        return Status::OK;
    }
};

// ═══════════════════════════════════════════════════════════════
// Entry point
// ═══════════════════════════════════════════════════════════════

static void RunServer() {
    const std::string address("0.0.0.0:50051");

    MetricsServiceImpl service;

    ServerBuilder builder;

    // Bind to the specified address.
    // InsecureServerCredentials() means plaintext (no TLS).
    // For production deployments replace this with SslServerCredentials().
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());

    // Register our service implementation
    builder.RegisterService(&service);

    // Assemble and start the server
    std::unique_ptr<Server> server(builder.BuildAndStart());
    if (!server) {
        std::cerr << "[Server] FATAL: failed to start server on " << address << "\n";
        return;
    }

    std::cout << "[Server] Telemetry server listening on " << address << "\n";
    std::cout << "[Server] Press Ctrl+C to stop.\n";

    // Block the main thread until the server shuts down (e.g. via SIGINT)
    server->Wait();
}

int main(int /*argc*/, char** /*argv*/) {
    RunServer();
    return 0;
}
