#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>
#include <level_bricks.hpp>
#include <round_config.hpp>
#include <run_state.hpp>
#include <stack_sanity.hpp>

namespace {

    using aloe::benchmarks::RoundConfig;
    using aloe::benchmarks::RunState;
    using IpCounters         = aloe::net::Ipv4Counters;
    using TcpCounters        = aloe::tcp::TcpCounters;
    using ShardQueueCounters = aloe::loop::ShardQueueCounters;

    /// The failure `check_stack_counters` records for these counters; empty when the run stays clean.
    [[nodiscard]] std::string
    failure_for(const TcpCounters& tcp, const IpCounters& ip, const ShardQueueCounters& shard) {
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

TEST(StackConfig, ALoneOrderIsProbedNoEarlierThanTheRetransmissionTimeout) {
    const aloe::tcp::TcpConfig config = aloe::benchmarks::stack_tcp_config();
    EXPECT_EQ(config.tail_probe_ack_delay, std::chrono::milliseconds{500})
        << "the venue acknowledges an order only with its next tick";
    EXPECT_GE(config.tail_probe_ack_delay, config.rto_min) << "the probe of a lone order falls at the RTO's moment";
    EXPECT_NO_THROW(std::ignore = config.validate(1480, std::chrono::milliseconds{1}));
}

TEST(StackSanity, FailsOnEveryDropAndRefusal) {
    EXPECT_EQ(failure_for({}, {}, {}), "");
    const Counter<TcpCounters> tcp[] = {
        {&TcpCounters::dropped_bad_header,       "tcp.dropped_bad_header"      },
        {&TcpCounters::dropped_bad_checksum,     "tcp.dropped_bad_checksum"    },
        {&TcpCounters::dropped_no_connection,    "tcp.dropped_no_connection"   },
        {&TcpCounters::dropped_closed,           "tcp.dropped_closed"          },
        {&TcpCounters::dropped_unexpected,       "tcp.dropped_unexpected"      },
        {&TcpCounters::dropped_duplicate,        "tcp.dropped_duplicate"       },
        {&TcpCounters::dropped_out_of_order,     "tcp.dropped_out_of_order"    },
        {&TcpCounters::dropped_out_of_window,    "tcp.dropped_out_of_window"   },
        {&TcpCounters::dropped_no_slot,          "tcp.dropped_no_slot"         },
        {&TcpCounters::dropped_no_node,          "tcp.dropped_no_node"         },
        {&TcpCounters::dropped_table_full,       "tcp.dropped_table_full"      },
        {&TcpCounters::send_refused,             "tcp.send_refused"            },
        {&TcpCounters::commits_refused,          "tcp.commits_refused"         },
        {&TcpCounters::allocation_failures,      "tcp.allocation_failures"     },
        {&TcpCounters::retransmits,              "tcp.retransmits"             },
        {&TcpCounters::fast_retransmits,         "tcp.fast_retransmits"        },
        {&TcpCounters::retransmits_deferred,     "tcp.retransmits_deferred"    },
        {&TcpCounters::retransmits_copied,       "tcp.retransmits_copied"      },
        {&TcpCounters::zero_window_probes,       "tcp.zero_window_probes"      },
        {&TcpCounters::dropped_paws,             "tcp.dropped_paws"            },
        {&TcpCounters::out_of_order_held,        "tcp.out_of_order_held"       },
        {&TcpCounters::out_of_order_released,    "tcp.out_of_order_released"   },
        {&TcpCounters::sack_acks_sent,           "tcp.sack_acks_sent"          },
        {&TcpCounters::duplicate_sacks_sent,     "tcp.duplicate_sacks_sent"    },
        {&TcpCounters::delayed_acks_sent,        "tcp.delayed_acks_sent"       },
        {&TcpCounters::duplicate_sacks_received, "tcp.duplicate_sacks_received"},
        {&TcpCounters::rack_marked,              "tcp.rack_marked"             },
        {&TcpCounters::lost_retransmits,         "tcp.lost_retransmits"        },
        {&TcpCounters::reorder_timeouts,         "tcp.reorder_timeouts"        },
        {&TcpCounters::reordering_seen,          "tcp.reordering_seen"         },
        {&TcpCounters::sack_reneging,            "tcp.sack_reneging"           },
        {&TcpCounters::tail_probes,              "tcp.tail_probes"             },
        {&TcpCounters::tail_probes_needless,     "tcp.tail_probes_needless"    },
        {&TcpCounters::tail_probe_recoveries,    "tcp.tail_probe_recoveries"   },
        {&TcpCounters::tail_probes_early,        "tcp.tail_probes_early"       },
        {&TcpCounters::undos_duplicate_sack,     "tcp.undos_duplicate_sack"    },
        {&TcpCounters::undos_original_ack,       "tcp.undos_original_ack"      },
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
    ShardQueueCounters shard;
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
