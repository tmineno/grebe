#pragma once

#include <nlohmann/json.hpp>

// BM-H: UDP loopback throughput benchmark.
// Measures UdpProducer → UdpConsumer throughput on 127.0.0.1.
// channels: number of channels for rate scenarios (1-8).
// max_datagram_size: max bytes per UDP datagram (default 1400 for WSL2 safety).
// burst_size: sendmmsg/recvmmsg batch size (1 = no batching, Linux only).
// Returns JSON array of per-scenario results.
nlohmann::json run_bench_udp(int duration_seconds, uint32_t channels = 1,
                             size_t max_datagram_size = 1400,
                             uint32_t burst_size = 1);
