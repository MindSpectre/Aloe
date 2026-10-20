#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <level_bricks.hpp>
#include <round_config.hpp>
#include <run_state.hpp>
#include <stack_sanity.hpp>

namespace {

    using aloe::benchmarks::RoundConfig;
    using aloe::benchmarks::RunState;
    using IpCounters    = aloe::net::Ipv4Counters;
    using TcpCounters   = aloe::tcp::TcpCounters;
    using ShardCounters = aloe::loop::ShardCounters;

    /// The failure `check_stack_counters` records for these counters; empty when the run stays clean.
    [[nodiscard]] std::string failure_for(const TcpCounters& tcp, const IpCounters& ip, const ShardCounters& shard) {
        RunState state{
            RoundConfig{.rounds = 1, .warmup = 0}
        };
        aloe::benchmarks::check_stack_counters(state, tcp, ip, shard);
        return state.failed.load() ? state.failure : std::string{};
    }

    template <typename Counters>
    struct Counter {
        std::uint64_t Counters::* member = nullptr;
        std::string_view name;
    };

}  // namespace

TEST(StackConfig, ArpNeverGoesStaleDuringARun) {
    const aloe::benchmarks::PeerConfig peer{};
    const aloe::net::Ipv4Config config = aloe::benchmarks::stack_ip_config(peer);
    EXPECT_EQ(config.address, peer.stack_ip);
    EXPECT_EQ(config.prefix, 24U);
    EXPECT_GE(config.arp_reachable, std::chrono::hours{24})
        << "a gapped run can last minutes; no refresh on the hot path";
    EXPECT_GT(config.arp_expire, config.arp_reachable);
}

TEST(StackSanity, FailsOnEveryDropAndRefusal) {
    EXPECT_EQ(failure_for({}, {}, {}), "");
    const Counter<TcpCounters> tcp[] = {
        {&TcpCounters::dropped_bad_header,    "tcp.dropped_bad_header"   },
        {&TcpCounters::dropped_bad_checksum,  "tcp.dropped_bad_checksum" },
        {&TcpCounters::dropped_no_connection, "tcp.dropped_no_connection"},
        {&TcpCounters::dropped_closed,        "tcp.dropped_closed"       },
        {&TcpCounters::dropped_unexpected,    "tcp.dropped_unexpected"   },
        {&TcpCounters::dropped_duplicate,     "tcp.dropped_duplicate"    },
        {&TcpCounters::dropped_out_of_order,  "tcp.dropped_out_of_order" },
        {&TcpCounters::dropped_out_of_window, "tcp.dropped_out_of_window"},
        {&TcpCounters::dropped_no_slot,       "tcp.dropped_no_slot"      },
        {&TcpCounters::dropped_no_node,       "tcp.dropped_no_node"      },
        {&TcpCounters::dropped_table_full,    "tcp.dropped_table_full"   },
        {&TcpCounters::send_refused,          "tcp.send_refused"         },
        {&TcpCounters::commits_refused,       "tcp.commits_refused"      },
        {&TcpCounters::allocation_failures,   "tcp.allocation_failures"  },
        {&TcpCounters::retransmits,           "tcp.retransmits"          },
    };
    for (const auto& [member, name] : tcp) {
        TcpCounters counters;
        counters.*member = 1;
        EXPECT_NE(failure_for(counters, {}, {}).find(std::string{name} + "=1"), std::string::npos) << name;
    }
    const Counter<IpCounters> ip[] = {
        {&IpCounters::transmit_refused,      "ip.transmit_refused"     },
        {&IpCounters::dropped_short,         "ip.dropped_short"        },
        {&IpCounters::dropped_not_for_us,    "ip.dropped_not_for_us"   },
        {&IpCounters::dropped_ethertype,     "ip.dropped_ethertype"    },
        {&IpCounters::dropped_arp_malformed, "ip.dropped_arp_malformed"},
        {&IpCounters::dropped_arp_conflict,  "ip.dropped_arp_conflict" },
        {&IpCounters::dropped_bad_header,    "ip.dropped_bad_header"   },
        {&IpCounters::dropped_bad_checksum,  "ip.dropped_bad_checksum" },
        {&IpCounters::dropped_fragment,      "ip.dropped_fragment"     },
        {&IpCounters::dropped_protocol,      "ip.dropped_protocol"     },
        {&IpCounters::dropped_icmp,          "ip.dropped_icmp"         },
        {&IpCounters::dropped_martian,       "ip.dropped_martian"      },
        {&IpCounters::dropped_tcp_broadcast, "ip.dropped_tcp_broadcast"},
        {&IpCounters::send_no_route,         "ip.send_no_route"        },
        {&IpCounters::send_oversized,        "ip.send_oversized"       },
        {&IpCounters::send_refused,          "ip.send_refused"         },
    };
    for (const auto& [member, name] : ip) {
        IpCounters counters;
        counters.*member = 1;
        EXPECT_NE(failure_for({}, counters, {}).find(std::string{name} + "=1"), std::string::npos) << name;
    }
    ShardCounters shard;
    shard.transmit_refused = 1;
    EXPECT_NE(failure_for({}, {}, shard).find("shard.transmit_refused=1"), std::string::npos);
}

TEST(StackSanity, AllowsTheWaitForArp) {
    TcpCounters tcp;
    tcp.send_unresolved = 1;  // the first SYN or SYN-ACK waits for the reply with --arp-on-wire
    IpCounters ip;
    ip.send_unresolved         = 1;
    ip.dropped_arp_unsolicited = 1;  // the brick's rule, not the benchmark's
    EXPECT_EQ(failure_for(tcp, ip, {}), "");
}
