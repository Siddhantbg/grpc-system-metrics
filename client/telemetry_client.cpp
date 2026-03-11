// telemetry_client.cpp
// gRPC client that connects to the Telemetry Monitoring Microservice,
// calls GetSystemMetrics, and prints the returned metrics in a
// human-readable format.

#include <grpcpp/grpcpp.h>
#include "metrics.grpc.pb.h"

#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using telemetry::MetricsRequest;
using telemetry::MetricsResponse;
using telemetry::MetricsService;

// ═══════════════════════════════════════════════════════════════
// ANSI colour helpers for a coloured terminal output.
// Falls back gracefully on terminals that don't support ANSI codes.
// ═══════════════════════════════════════════════════════════════

namespace colour {
    constexpr const char* RESET  = "\033[0m";
    constexpr const char* BOLD   = "\033[1m";
    constexpr const char* CYAN   = "\033[36m";
    constexpr const char* GREEN  = "\033[32m";
    constexpr const char* YELLOW = "\033[33m";
    constexpr const char* RED    = "\033[31m";
    constexpr const char* BLUE   = "\033[34m";
}

// ═══════════════════════════════════════════════════════════════
// MetricsClient
// Wraps the generated gRPC stub and provides a clean API for
// requesting and displaying system metrics.
// ═══════════════════════════════════════════════════════════════

class MetricsClient {
public:
    // Construct the client from an existing channel.
    // Keeping the channel an explicit parameter makes it easy to
    // swap credentials (e.g. TLS) without changing business logic.
    explicit MetricsClient(std::shared_ptr<Channel> channel)
        : stub_(MetricsService::NewStub(std::move(channel))) {}

    // Send one GetSystemMetrics request to the server and print the
    // result.  Returns true on success, false on RPC failure.
    bool FetchAndPrint(const std::string& clientId = "cpp-telemetry-client") {
        // Build the request
        MetricsRequest request;
        request.set_client_id(clientId);

        MetricsResponse response;
        ClientContext   context;

        // Invoke the RPC (blocking call)
        const Status status = stub_->GetSystemMetrics(&context, request, &response);

        if (!status.ok()) {
            std::cerr << colour::RED << "[Client] RPC failed"  << colour::RESET
                      << "  code="    << status.error_code()
                      << "  message=" << status.error_message() << "\n";
            return false;
        }

        printReport(response);
        return true;
    }

private:
    std::unique_ptr<MetricsService::Stub> stub_;

    // ── Formatting helpers ───────────────────────────────────────

    // Return a colour code depending on the usage percentage:
    //   < 60 %  → green   (healthy)
    //   < 85 %  → yellow  (moderate)
    // ≥ 85 %  → red     (high)
    static const char* usageColour(double pct) {
        if (pct < 60.0) return colour::GREEN;
        if (pct < 85.0) return colour::YELLOW;
        return colour::RED;
    }

    // Render `pct` followed by a 20-character ASCII progress bar.
    // Example:  " 42.30%  [########............]"
    static std::string progressBar(double pct) {
        constexpr int WIDTH = 20;
        const int filled = static_cast<int>(pct / 100.0 * WIDTH);

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << std::setw(6) << pct << "%  [";
        for (int i = 0; i < WIDTH; ++i)
            oss << (i < filled ? '#' : '.');
        oss << "]";
        return oss.str();
    }

    // Print a complete formatted report from the MetricsResponse.
    static void printReport(const MetricsResponse& r) {
        const std::string SEP(50, '=');
        const std::string DIV(50, '-');

        std::cout << "\n"
                  << colour::BOLD << colour::CYAN << SEP << colour::RESET << "\n"
                  << colour::BOLD << colour::CYAN
                  << "        SYSTEM TELEMETRY REPORT"
                  << colour::RESET << "\n"
                  << colour::BOLD << colour::CYAN << SEP << colour::RESET << "\n";

        std::cout << colour::BOLD << " Timestamp  : " << colour::RESET
                  << r.timestamp() << "\n"
                  << DIV << "\n";

        // CPU
        const double cpu = r.cpu_usage_percent();
        std::cout << colour::BOLD << " CPU Usage  : " << colour::RESET
                  << usageColour(cpu)
                  << progressBar(cpu)
                  << colour::RESET << "\n";

        // Memory
        const double mem = r.memory_usage_percent();
        std::cout << colour::BOLD << " Memory     : " << colour::RESET
                  << usageColour(mem)
                  << progressBar(mem)
                  << colour::RESET
                  << "   (" << r.used_memory_mb() << " / " << r.total_memory_mb() << " MiB)\n";

        // Disk
        const double dsk = r.disk_usage_percent();
        std::cout << colour::BOLD << " Disk (/)   : " << colour::RESET
                  << usageColour(dsk)
                  << progressBar(dsk)
                  << colour::RESET
                  << "   (" << r.used_disk_gb() << " / " << r.total_disk_gb() << " GiB)\n";

        std::cout << colour::BOLD << colour::CYAN << SEP << colour::RESET << "\n\n";
    }
};

// ═══════════════════════════════════════════════════════════════
// Entry point
// ═══════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    // Default server address; can be overridden via the first argument.
    // E.g.:  ./telemetry_client 192.168.1.10:50051
    std::string serverAddress = "localhost:50051";
    if (argc > 1) {
        serverAddress = argv[1];
    }

    std::cout << colour::BLUE << "[Client] " << colour::RESET
              << "Connecting to " << serverAddress << " …\n";

    // Create an insecure plaintext channel.
    // For production, replace InsecureChannelCredentials() with
    // grpc::SslCredentials(grpc::SslCredentialsOptions()).
    auto channel = grpc::CreateChannel(serverAddress,
                                       grpc::InsecureChannelCredentials());

    MetricsClient client(channel);

    // Perform the RPC and print results
    const bool ok = client.FetchAndPrint("cpp-telemetry-client");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
