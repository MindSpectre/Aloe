#pragma once

#include <aloe/loop>
#include <aloe/net>
#include <aloe/tcp>
#include <cstdint>
#include <format>
#include <string>

#include <run_state.hpp>

namespace aloe::benchmarks {

    /// Fails `state` when the stack dropped, refused or starved anything during the run: a benchmark that measured
    /// garbage must say so instead of reporting it.
    inline void check_stack_counters(RunState& state,
                                     const tcp::TcpCounters& tcp,
                                     const net::Ipv4Counters& ip,
                                     const loop::ShardCounters& shard) {
        std::string problems;
        const auto expect_zero = [&problems](const std::uint64_t value, const char* name) {
            if (value != 0) {
                problems += std::format(" {}={}", name, value);
            }
        };
        expect_zero(tcp.dropped_bad_header, "tcp.dropped_bad_header");
        expect_zero(tcp.dropped_bad_checksum, "tcp.dropped_bad_checksum");
        expect_zero(tcp.dropped_no_connection, "tcp.dropped_no_connection");
        expect_zero(tcp.dropped_closed, "tcp.dropped_closed");
        expect_zero(tcp.dropped_unexpected, "tcp.dropped_unexpected");
        expect_zero(tcp.dropped_duplicate, "tcp.dropped_duplicate");
        expect_zero(tcp.dropped_out_of_order, "tcp.dropped_out_of_order");
        expect_zero(tcp.dropped_out_of_window, "tcp.dropped_out_of_window");
        expect_zero(tcp.dropped_no_slot, "tcp.dropped_no_slot");
        expect_zero(tcp.dropped_no_node, "tcp.dropped_no_node");
        expect_zero(tcp.dropped_table_full, "tcp.dropped_table_full");
        expect_zero(tcp.send_refused, "tcp.send_refused");
        expect_zero(tcp.commits_refused, "tcp.commits_refused");
        expect_zero(tcp.allocation_failures, "tcp.allocation_failures");
        expect_zero(tcp.retransmits, "tcp.retransmits");
        expect_zero(ip.dropped_ethertype, "ip.dropped_ethertype");
        expect_zero(ip.dropped_arp_malformed, "ip.dropped_arp_malformed");
        expect_zero(ip.dropped_arp_conflict, "ip.dropped_arp_conflict");
        expect_zero(ip.dropped_bad_header, "ip.dropped_bad_header");
        expect_zero(ip.dropped_bad_checksum, "ip.dropped_bad_checksum");
        expect_zero(ip.dropped_fragment, "ip.dropped_fragment");
        expect_zero(ip.dropped_protocol, "ip.dropped_protocol");
        expect_zero(ip.dropped_martian, "ip.dropped_martian");
        expect_zero(ip.dropped_tcp_broadcast, "ip.dropped_tcp_broadcast");
        expect_zero(ip.send_no_route, "ip.send_no_route");
        expect_zero(ip.send_oversized, "ip.send_oversized");
        expect_zero(ip.send_refused, "ip.send_refused");
        expect_zero(shard.transmit_refused, "shard.transmit_refused");
        if (!problems.empty()) {
            state.fail("the stack's counters are not clean:" + problems);
        }
    }

}  // namespace aloe::benchmarks
