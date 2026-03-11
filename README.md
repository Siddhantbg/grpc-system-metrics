# gRPC System Metrics — Telemetry Monitoring Microservice

A lightweight Telemetry Monitoring Microservice written in **C++17** that collects real-time system metrics from a Linux machine and exposes them over **gRPC**.

```
┌─────────────────────────┐          gRPC (port 50051)          ┌──────────────────────────┐
│   telemetry_client      │  ──── GetSystemMetrics() ──────►   │   telemetry_server       │
│   (RPC caller)          │  ◄─── MetricsResponse ──────────   │   (reads /proc/*)        │
└─────────────────────────┘                                      └──────────────────────────┘
```

## Features

| Metric | Source |
|---|---|
| CPU usage % | `/proc/stat` (two-sample delta over 500 ms) |
| Memory usage % | `/proc/meminfo` (`MemAvailable` accounting) |
| Disk usage % | `statvfs("/")` — root filesystem |

---

## Project Structure

```
grpc-system-metrics/
├── CMakeLists.txt               # Top-level CMake build definition
├── proto/
│   └── metrics.proto            # gRPC / Protobuf API contract
├── server/
│   └── telemetry_server.cpp     # gRPC server — collects & serves metrics
└── client/
    └── telemetry_client.cpp     # gRPC client — requests & displays metrics
```

Generated C++ bindings (`metrics.pb.{cc,h}` and `metrics.grpc.pb.{cc,h}`) are placed in `<build-dir>/generated/` and never committed to source control.

---

## Prerequisites

Install the following on your Linux machine (Ubuntu/Debian instructions shown):

```bash
# Build tools
sudo apt update
sudo apt install -y build-essential cmake git

# Protocol Buffers compiler
sudo apt install -y protobuf-compiler libprotobuf-dev

# gRPC C++ library and the protoc plugin
sudo apt install -y libgrpc++-dev protobuf-compiler-grpc
```

> **Alternative (vcpkg):**  
> If the apt packages are too old for your distro, build from source following the  
> [gRPC Quick Start](https://grpc.io/docs/languages/cpp/quickstart/) guide.

---

## Build

```bash
# 1. Clone / enter the project directory
cd grpc-system-metrics

# 2. Configure (Debug or Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 3. Compile
cmake --build build --parallel $(nproc)
```

Both executables appear in `build/`:

```
build/telemetry_server
build/telemetry_client
```

---

## Run

### Start the server

```bash
./build/telemetry_server
```

Expected output:

```
[Server] Telemetry server listening on 0.0.0.0:50051
[Server] Press Ctrl+C to stop.
```

### Run the client (in a second terminal)

```bash
./build/telemetry_client
```

Example output:

```
[Client] Connecting to localhost:50051 …

==================================================
        SYSTEM TELEMETRY REPORT
==================================================
 Timestamp  : 2026-03-11 14:22:09
--------------------------------------------------
 CPU Usage  :   4.20%  [#...................]
 Memory     :  61.83%  [############........]   (9893 / 16001 MiB)
 Disk (/)   :  38.50%  [#######.............]   (146 / 380 GiB)
==================================================
```

### Connect to a remote server

```bash
./build/telemetry_client 192.168.1.10:50051
```

---

## How It Works

### CPU Sampling (`/proc/stat`)

The kernel exposes cumulative tick counts since boot:

```
cpu  <user> <nice> <system> <idle> <iowait> <irq> <softirq> <steal>
```

The server takes two snapshots 500 ms apart and computes:

$$\text{CPU\%} = \left(1 - \frac{\Delta\text{idle}}{\Delta\text{total}}\right) \times 100$$

### Memory (`/proc/meminfo`)

```
used = MemTotal − MemAvailable
```

`MemAvailable` is preferred over `MemFree` because it includes memory that can be quickly reclaimed from the page cache.

### Disk (`statvfs`)

```
used  = f_blocks × f_frsize − f_bfree × f_frsize
total = f_blocks × f_frsize
```

`f_bfree` (not `f_bavail`) is used so that space reserved for root is counted as used.

---

## Configuration

| Setting | Default | How to change |
|---|---|---|
| Server listen address | `0.0.0.0:50051` | Edit `RunServer()` in `telemetry_server.cpp` |
| Client target address | `localhost:50051` | Pass as first CLI argument |
| CPU sample window | 500 ms | Change the `sleep_for` duration in `getCpuUsagePercent()` |
| Disk mount point | `/` | Change the `path` argument to `readDiskSnapshot()` |

---

## Security Notes

- The channel uses **InsecureChannelCredentials** (plaintext) for simplicity.  
  For production, replace with `grpc::SslCredentials` on the client and  
  `grpc::SslServerCredentials` on the server.  
- The server reads only from `/proc/stat`, `/proc/meminfo`, and calls `statvfs("/")` — all read-only, no elevated privileges required.
