#include <algorithm>
#include <aloe/core>
#include <array>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <eal.hpp>
#include <ethernet.hpp>
#include <port.hpp>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

namespace aloe::ethdev {

    namespace {

        constexpr std::uint64_t rx_checksum_offloads =
            RTE_ETH_RX_OFFLOAD_IPV4_CKSUM | RTE_ETH_RX_OFFLOAD_UDP_CKSUM | RTE_ETH_RX_OFFLOAD_TCP_CKSUM;
        constexpr std::uint64_t tx_checksum_offloads =
            RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
        constexpr std::uint64_t wanted_rss_types =
            RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV4_UDP;

        /// The largest MTU whose frames fit the data room of one mbuf of the port's pools.
        constexpr std::uint16_t largest_mtu = RTE_MBUF_DEFAULT_DATAROOM - wire::ethernet_header_size;

        /// Toeplitz reads at most this much key for an IPv4 4-tuple.
        constexpr std::uint8_t smallest_usable_key = 16;

        void check(const int result, const std::string_view step) {
            if (result < 0) {
                throw EthdevError{detail::describe(step, result)};
            }
        }

        /// True when a driver answered that it does not implement the operation.
        [[nodiscard]] bool unsupported(const int result) noexcept {
            return result == -ENOTSUP;
        }

        [[nodiscard]] device::RssHashTypes types_of(const std::uint64_t hash_functions) noexcept {
            return device::RssHashTypes{.ipv4     = (hash_functions & RTE_ETH_RSS_IPV4) != 0,
                                        .ipv4_tcp = (hash_functions & RTE_ETH_RSS_NONFRAG_IPV4_TCP) != 0,
                                        .ipv4_udp = (hash_functions & RTE_ETH_RSS_NONFRAG_IPV4_UDP) != 0};
        }

        [[nodiscard]] std::uint64_t hash_functions_of(const device::RssHashTypes& types) noexcept {
            return (types.ipv4 ? RTE_ETH_RSS_IPV4 : 0) | (types.ipv4_tcp ? RTE_ETH_RSS_NONFRAG_IPV4_TCP : 0) |
                   (types.ipv4_udp ? RTE_ETH_RSS_NONFRAG_IPV4_UDP : 0);
        }

        [[nodiscard]] device::Capabilities capabilities_of(const rte_eth_dev_info& info) noexcept {
            const std::uint64_t rss_types = info.flow_type_rss_offloads & wanted_rss_types;
            const bool rss = rss_types != 0 && info.hash_key_size >= smallest_usable_key &&
                             info.hash_key_size <= device::rss_key_capacity &&
                             std::has_single_bit(info.reta_size);  // device::queue_for masks, as cards do
            constexpr std::uint64_t rx_l4 = RTE_ETH_RX_OFFLOAD_UDP_CKSUM | RTE_ETH_RX_OFFLOAD_TCP_CKSUM;
            constexpr std::uint64_t tx_l4 = RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
            return device::Capabilities{
                .max_rx_queues    = info.max_rx_queues,
                .max_tx_queues    = info.max_tx_queues,
                .min_mtu          = info.min_mtu,
                .max_mtu          = std::min<std::uint16_t>(info.max_mtu, largest_mtu),
                .rss              = rss,
                .rss_key_size     = rss ? info.hash_key_size : std::uint8_t{0},
                .rss_table_size   = rss ? info.reta_size : std::uint16_t{0},
                .rss_types        = rss ? types_of(rss_types) : device::RssHashTypes{},
                .rx_ipv4_checksum = (info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM) != 0,
                .rx_l4_checksum   = (info.rx_offload_capa & rx_l4) == rx_l4,
                .tx_ipv4_checksum = (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) != 0,
                .tx_l4_checksum   = (info.tx_offload_capa & tx_l4) == tx_l4,
            };
        }

        [[nodiscard]] std::array<std::uint8_t, device::rss_key_capacity> key_bytes() noexcept {
            std::array<std::uint8_t, device::rss_key_capacity> key{};
            for (std::size_t index = 0; index < key.size(); ++index) {
                key[index] = std::to_integer<std::uint8_t>(device::aloe_rss_key[index]);
            }
            return key;
        }

    }  // namespace

    Port::Port(const PortConfig& config)
        : queues_{config.queues},
          counters_(config.queues) {
        if (config.queues == 0) {
            throw EthdevError{"a port needs at least one queue"};
        }
        check(rte_eth_dev_get_port_by_name(config.name.c_str(), &port_id_),
              std::format("rte_eth_dev_get_port_by_name({})", config.name));
        // Claiming the port first makes a second Port on the same name fail before it touches the device.
        check(rte_eth_dev_owner_new(&owner_), "rte_eth_dev_owner_new");
        rte_eth_dev_owner owner{};
        owner.id = owner_;
        std::format_to_n(owner.name, sizeof(owner.name) - 1, "aloe");
        check(rte_eth_dev_owner_set(port_id_, &owner),
              std::format("rte_eth_dev_owner_set({}), which fails for a port already taken", config.name));
        owned_ = true;
        try {
            rte_eth_dev_info info{};
            check(rte_eth_dev_info_get(port_id_, &info), "rte_eth_dev_info_get");
            driver_       = info.driver_name == nullptr ? std::string{} : std::string{info.driver_name};
            capabilities_ = capabilities_of(info);
            if (config.queues > capabilities_.max_rx_queues || config.queues > capabilities_.max_tx_queues) {
                throw EthdevError{std::format("{} offers {} receive and {} transmit queues, {} were asked for",
                                              config.name,
                                              capabilities_.max_rx_queues,
                                              capabilities_.max_tx_queues,
                                              config.queues)};
            }
            if (config.mtu > capabilities_.max_mtu) {
                throw EthdevError{std::format("{} carries an MTU of at most {} in one mbuf, {} was asked for",
                                              config.name,
                                              capabilities_.max_mtu,
                                              config.mtu)};
            }
            configure(config, info);
            check(rte_eth_dev_start(port_id_), "rte_eth_dev_start");
            started_ = true;
            check(rte_eth_dev_get_mtu(port_id_, &mtu_), "rte_eth_dev_get_mtu");
            rte_ether_addr address{};
            check(rte_eth_macaddr_get(port_id_, &address), "rte_eth_macaddr_get");
            wire::MacAddress::Bytes bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                bytes[index] = std::byte{address.addr_bytes[index]};
            }
            mac_ = wire::MacAddress{bytes};
            if (capabilities_.rss && config.queues > 1) {
                program_rss(info);
            }
        } catch (...) {
            teardown();
            throw;
        }
    }

    void Port::configure(const PortConfig& config, const rte_eth_dev_info& info) {
        const bool rss = capabilities_.rss && config.queues > 1;
        auto key       = key_bytes();
        rte_eth_conf conf{};
        conf.rxmode.mq_mode  = rss ? RTE_ETH_MQ_RX_RSS : RTE_ETH_MQ_RX_NONE;
        conf.rxmode.mtu      = config.mtu;
        conf.rxmode.offloads = info.rx_offload_capa & rx_checksum_offloads;
        conf.txmode.offloads = info.tx_offload_capa & tx_checksum_offloads;
        if (rss) {
            conf.rx_adv_conf.rss_conf.rss_key     = key.data();
            conf.rx_adv_conf.rss_conf.rss_key_len = capabilities_.rss_key_size;
            conf.rx_adv_conf.rss_conf.rss_hf      = hash_functions_of(capabilities_.rss_types);
        }
        check(rte_eth_dev_configure(port_id_, config.queues, config.queues, &conf), "rte_eth_dev_configure");

        const unsigned socket = rte_socket_id();
        const unsigned cache  = std::min<unsigned>(RTE_MEMPOOL_CACHE_MAX_SIZE, config.pool_size / 2);
        pools_.reserve(config.queues);
        for (std::uint16_t queue = 0; queue < config.queues; ++queue) {
            const std::string name = std::format("aloe-{}-{}", port_id_, queue);
            rte_mempool* pool      = rte_pktmbuf_pool_create(
                name.c_str(), config.pool_size, cache, 0, RTE_MBUF_DEFAULT_BUF_SIZE, static_cast<int>(socket));
            if (pool == nullptr) {
                throw EthdevError{detail::describe("rte_pktmbuf_pool_create", rte_errno)};
            }
            pools_.push_back(pool);
        }
        for (std::uint16_t queue = 0; queue < config.queues; ++queue) {
            check(rte_eth_rx_queue_setup(port_id_, queue, config.descriptors, socket, nullptr, pools_[queue]),
                  "rte_eth_rx_queue_setup");
            check(rte_eth_tx_queue_setup(port_id_, queue, config.descriptors, socket, nullptr),
                  "rte_eth_tx_queue_setup");
        }
    }

    void Port::program_rss(const rte_eth_dev_info& info) {
        steering_ = device::round_robin_rss(queues_, info.reta_size, capabilities_.rss_key_size);

        // Some drivers take the key only through this call, not through configure.
        auto key = key_bytes();
        rte_eth_rss_conf wanted{};
        wanted.rss_key     = key.data();
        wanted.rss_key_len = capabilities_.rss_key_size;
        wanted.rss_hf      = hash_functions_of(capabilities_.rss_types);
        if (const int key_update = rte_eth_dev_rss_hash_update(port_id_, &wanted);
            key_update < 0 && !unsupported(key_update)) {
            check(key_update, "rte_eth_dev_rss_hash_update");
        }

        // The table goes in groups of 64 entries; the last group of a table of another size is partial.
        const std::size_t entries_total = info.reta_size;
        std::vector<rte_eth_rss_reta_entry64> entries((entries_total + RTE_ETH_RETA_GROUP_SIZE - 1) /
                                                      RTE_ETH_RETA_GROUP_SIZE);
        const auto mask_of = [&](const std::size_t group) {
            const std::size_t remaining = entries_total - group * RTE_ETH_RETA_GROUP_SIZE;
            return remaining >= RTE_ETH_RETA_GROUP_SIZE ? ~std::uint64_t{0} : (std::uint64_t{1} << remaining) - 1;
        };
        for (std::size_t entry = 0; entry < entries_total; ++entry) {
            entries[entry / RTE_ETH_RETA_GROUP_SIZE].reta[entry % RTE_ETH_RETA_GROUP_SIZE] =
                static_cast<std::uint16_t>(entry % queues_);
        }
        for (std::size_t group = 0; group < entries.size(); ++group) {
            entries[group].mask = mask_of(group);
        }
        if (const int table_update = rte_eth_dev_rss_reta_update(port_id_, entries.data(), info.reta_size);
            table_update < 0 && !unsupported(table_update)) {
            check(table_update, "rte_eth_dev_rss_reta_update");
        }
        for (std::size_t group = 0; group < entries.size(); ++group) {
            entries[group].mask = mask_of(group);
        }
        if (const int table_query = rte_eth_dev_rss_reta_query(port_id_, entries.data(), info.reta_size);
            table_query == 0) {
            for (std::size_t entry = 0; entry < entries_total; ++entry) {
                steering_.table[entry] = entries[entry / RTE_ETH_RETA_GROUP_SIZE].reta[entry % RTE_ETH_RETA_GROUP_SIZE];
            }
        } else if (!unsupported(table_query)) {
            check(table_query, "rte_eth_dev_rss_reta_query");
        }

        std::array<std::uint8_t, device::rss_key_capacity> buffer{};
        rte_eth_rss_conf programmed{};
        programmed.rss_key     = buffer.data();
        programmed.rss_key_len = device::rss_key_capacity;
        if (const int key_query = rte_eth_dev_rss_hash_conf_get(port_id_, &programmed); key_query == 0) {
            for (std::size_t index = 0; index < capabilities_.rss_key_size; ++index) {
                steering_.key[index] = std::byte{buffer[index]};
            }
            steering_.types = types_of(programmed.rss_hf & wanted_rss_types);
        } else if (!unsupported(key_query)) {
            check(key_query, "rte_eth_dev_rss_hash_conf_get");
        }
    }

    Port::~Port() {
        teardown();
    }

    void Port::teardown() noexcept {
        if (started_) {
            std::ignore = rte_eth_dev_stop(port_id_);
            started_    = false;
        }
        bool closed = true;
        if (owned_) {
            // DPDK releases the port even when the driver's own close fails; only a port it refused
            // to close (still started, or no close operation) is still there.
            closed = rte_eth_dev_close(port_id_) == 0 || rte_eth_dev_is_valid_port(port_id_) == 0;
            if (!closed) {
                std::ignore = rte_eth_dev_owner_unset(port_id_, owner_);
            }
            owned_ = false;
        }
        // A device that would not close may still hold mbufs of these pools in its rings.
        if (closed) {
            for (rte_mempool* pool : pools_) {
                rte_mempool_free(pool);
            }
        }
        pools_.clear();
    }

    bool Port::link_up() const noexcept {
        rte_eth_link link{};
        return rte_eth_link_get_nowait(port_id_, &link) == 0 && link.link_status == RTE_ETH_LINK_UP;
    }

    std::optional<Packet> Port::allocate(const std::uint16_t queue) noexcept {
        assert(queue < queues_);
        core::force_non_const(this);  // takes an mbuf from the pool, which a const member could still do
        rte_mbuf* mbuf = rte_pktmbuf_alloc(pools_[queue]);
        if (mbuf == nullptr) {
            return std::nullopt;
        }
        return Packet{mbuf};
    }

    std::size_t Port::receive(const std::uint16_t queue, std::span<Packet> out) noexcept {
        assert(queue < queues_);
        std::array<rte_mbuf*, max_burst> burst{};
        const auto wanted       = static_cast<std::uint16_t>(std::min(out.size(), max_burst));
        const std::uint16_t got = rte_eth_rx_burst(port_id_, queue, burst.data(), wanted);
        const std::size_t limit = static_cast<std::size_t>(mtu_) + wire::ethernet_header_size;
        std::size_t count       = 0;
        for (std::uint16_t index = 0; index < got; ++index) {
            rte_mbuf* mbuf = burst[index];
            if (mbuf->nb_segs != 1 || mbuf->pkt_len > limit || mbuf->pkt_len < wire::ethernet_header_size) {
                rte_pktmbuf_free(mbuf);
                ++counters_[queue].oversized;
                continue;
            }
            assert(out[count].empty());
            // A looped-back or recycled mbuf may still carry transmit requests; a received packet has none.
            mbuf->ol_flags   &= ~RTE_MBUF_F_TX_OFFLOAD_MASK;
            mbuf->tx_offload  = 0;
            out[count]        = Packet{mbuf};
            ++count;
        }
        counters_[queue].received += count;
        return count;
    }

    std::size_t Port::transmit(const std::uint16_t queue, std::span<Packet> in) noexcept {
        assert(queue < queues_);
        std::array<rte_mbuf*, max_burst> burst{};
        std::array<std::size_t, max_burst> slot_of{};
        const std::size_t offered = std::min(in.size(), max_burst);
        const std::size_t limit   = static_cast<std::size_t>(mtu_) + wire::ethernet_header_size;
        std::uint16_t pending     = 0;

        // Hands what is pending to the driver. Returns the first slot left with the caller, if any.
        const auto flush = [&]() noexcept -> std::optional<std::size_t> {
            const std::uint16_t sent = pending == 0 ? 0 : rte_eth_tx_burst(port_id_, queue, burst.data(), pending);
            for (std::uint16_t index = 0; index < sent; ++index) {
                std::ignore = in[slot_of[index]].release();
            }
            counters_[queue].transmitted          += sent;
            const std::optional<std::size_t> rest  = sent == pending ? std::nullopt : std::optional{slot_of[sent]};
            pending                                = 0;
            return rest;
        };

        for (std::size_t slot = 0; slot < offered; ++slot) {
            Packet& packet = in[slot];
            if (packet.empty()) {
                continue;
            }
            if (packet.size() < wire::ethernet_header_size || packet.size() > limit) {
                // Refused and counted, in order: what came before it is sent first, and if the
                // driver leaves some of that with the caller, this packet stays untouched too.
                if (const auto rest = flush()) {
                    return *rest;
                }
                packet = Packet{};
                ++counters_[queue].oversized;
                continue;
            }
            burst[pending]   = packet.get();
            slot_of[pending] = slot;
            ++pending;
        }
        if (const auto rest = flush()) {
            return *rest;
        }
        return offered;
    }

    device::QueueCounters Port::counters(const std::uint16_t queue) const noexcept {
        assert(queue < queues_);
        return counters_[queue];
    }

}  // namespace aloe::ethdev
