#pragma once

#include <cstdint>

namespace aloe::tcp {

    /// One per stack, monotonic, read on the owning thread or after it has stopped.
    struct TcpCounters {
        std::uint64_t segments_received     = 0;
        std::uint64_t data_segments_sent    = 0;
        std::uint64_t pure_acks_sent        = 0;
        std::uint64_t control_segments_sent = 0;  ///< SYN, SYN-ACK, FIN, first sends and retransmits alike.
        std::uint64_t resets_sent           = 0;
        std::uint64_t retransmits           = 0;  ///< Timer-driven control retransmits.
        std::uint64_t window_updates        = 0;  ///< Segments that extended the advertised edge.

        std::uint64_t connections_opened    = 0;  ///< Active opens that reached ESTABLISHED.
        std::uint64_t connections_accepted  = 0;
        std::uint64_t connections_closed    = 0;  ///< Normal closes, both FINs acknowledged.
        std::uint64_t connections_reset     = 0;  ///< RST received on a connection we own.
        std::uint64_t connections_timed_out = 0;
        std::uint64_t handshakes_failed     = 0;  ///< A passive open nobody owned yet was reset or timed out.

        std::uint64_t dropped_bad_header    = 0;
        std::uint64_t dropped_bad_checksum  = 0;
        std::uint64_t dropped_no_connection = 0;
        std::uint64_t dropped_closed        = 0;
        std::uint64_t dropped_unexpected    = 0;
        std::uint64_t dropped_duplicate     = 0;
        std::uint64_t dropped_out_of_order  = 0;
        std::uint64_t dropped_out_of_window = 0;
        std::uint64_t dropped_no_slot       = 0;
        std::uint64_t dropped_no_node       = 0;
        std::uint64_t dropped_table_full    = 0;

        std::uint64_t send_refused        = 0;
        std::uint64_t send_unresolved     = 0;
        std::uint64_t commits_refused     = 0;  ///< A commit after the connection left Established and CloseWait.
        /// A packet the pool could not supply for a prepare; the retry hint says when to try again.
        std::uint64_t allocation_failures = 0;

        friend constexpr bool operator==(const TcpCounters&, const TcpCounters&) noexcept = default;
    };

}  // namespace aloe::tcp
