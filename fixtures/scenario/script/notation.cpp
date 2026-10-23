#include "notation.hpp"

#include <aloe/core>
#include <aloe/wire>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <script.hpp>

namespace aloe::scenario {

    namespace {

        constexpr std::uint8_t kind_eol          = 0;
        constexpr std::uint8_t kind_nop          = 1;
        constexpr std::uint8_t kind_mss          = 2;
        constexpr std::uint8_t kind_window_scale = 3;
        constexpr std::uint8_t kind_sack_permit  = 4;
        constexpr std::uint8_t kind_sack         = 5;
        constexpr std::uint8_t kind_timestamps   = 8;
        constexpr std::size_t sack_block_size    = 8;
        constexpr std::size_t timestamps_size    = 10;

        void put(std::vector<std::byte>& out, const std::uint8_t value) {
            out.push_back(std::byte{value});
        }

        void put32(std::vector<std::byte>& out, const std::uint32_t value) {
            const std::size_t at = out.size();
            out.resize(at + 4);
            wire::store_be32(std::span<std::byte>{out}.subspan(at, 4), value);
        }

        [[nodiscard]] std::string describe_flags(const wire::TcpFlags flags) {
            std::string out;
            if (flags.has(wire::TcpFlag::Syn)) {
                out += 'S';
            }
            if (flags.has(wire::TcpFlag::Fin)) {
                out += 'F';
            }
            if (flags.has(wire::TcpFlag::Rst)) {
                out += 'R';
            }
            if (flags.has(wire::TcpFlag::Psh)) {
                out += 'P';
            }
            if (flags.has(wire::TcpFlag::Urg)) {
                out += 'U';
            }
            if (flags.has(wire::TcpFlag::Ack)) {
                out += '.';
            }
            return out.empty() ? "-" : out;
        }

        [[nodiscard]] std::string
        describe_range(const std::uint32_t first, const std::uint32_t last, const std::uint32_t length) {
            return std::format("{}:{}({})", first, last, length);
        }

    }  // namespace

    std::vector<std::byte> encode_options(const Options& options, const wire::TcpSequence sack_base) {
        assert(!options.any);
        std::vector<std::byte> out;
        for (const TcpOption& option : options.list) {
            switch (option.kind) {
                case TcpOption::Kind::Nop:
                    put(out, kind_nop);
                    break;
                case TcpOption::Kind::Eol:
                    put(out, kind_eol);
                    break;
                case TcpOption::Kind::Mss:
                    put(out, kind_mss);
                    put(out, 4);
                    put(out, static_cast<std::uint8_t>(option.mss >> 8U));
                    put(out, static_cast<std::uint8_t>(option.mss));
                    break;
                case TcpOption::Kind::WindowScale:
                    put(out, kind_window_scale);
                    put(out, 3);
                    put(out, option.shift);
                    break;
                case TcpOption::Kind::SackPermitted:
                    put(out, kind_sack_permit);
                    put(out, 2);
                    break;
                case TcpOption::Kind::Sack:
                    put(out, kind_sack);
                    put(out, static_cast<std::uint8_t>(2 + (sack_block_size * option.blocks.size())));
                    for (const SackBlock& block : option.blocks) {
                        put32(out, block.first + sack_base.value);
                        put32(out, block.last + sack_base.value);
                    }
                    break;
                case TcpOption::Kind::Timestamps:
                    put(out, kind_timestamps);
                    put(out, static_cast<std::uint8_t>(timestamps_size));
                    put32(out, option.value);
                    put32(out, option.echo);
                    break;
            }
        }
        return out;
    }

    std::string describe_options(const std::span<const std::byte> options, const wire::TcpSequence sack_base) {
        std::string out;
        const auto add = [&out](const std::string& word) {
            if (!out.empty()) {
                out += ',';
            }
            out += word;
        };
        std::size_t at = 0;
        while (at < options.size()) {
            const auto kind = std::to_integer<std::uint8_t>(options[at]);
            if (kind == kind_eol) {
                add("eol");
                break;
            }
            if (kind == kind_nop) {
                add("nop");
                ++at;
                continue;
            }
            if (at + 1 >= options.size()) {
                return "bad options";
            }
            const auto length = std::to_integer<std::size_t>(options[at + 1]);
            if (length < 2 || at + length > options.size()) {
                return "bad options";
            }
            const std::span<const std::byte> body = options.subspan(at + 2, length - 2);
            if (kind == kind_mss) {
                if (length != 4) {
                    return "bad options";
                }
                add(std::format("mss {}", wire::load_be16(body)));
            } else if (kind == kind_window_scale) {
                if (length != 3) {
                    return "bad options";
                }
                add(std::format("wscale {}", std::to_integer<unsigned>(body[0])));
            } else if (kind == kind_sack_permit) {
                if (length != 2) {
                    return "bad options";
                }
                add("sackOK");
            } else if (kind == kind_timestamps) {
                if (length != timestamps_size) {
                    return "bad options";
                }
                add(std::format(
                    "TS val {} ecr {}", wire::load_be32(body.first(4)), wire::load_be32(body.subspan(4, 4))));
            } else if (kind == kind_sack) {
                if (body.empty() || body.size() % sack_block_size != 0) {
                    return "bad options";
                }
                std::string blocks;
                for (std::size_t block = 0; block < body.size(); block += sack_block_size) {
                    if (!blocks.empty()) {
                        blocks += ',';
                    }
                    blocks += std::format("{}:{}",
                                          wire::load_be32(body.subspan(block, 4)) - sack_base.value,
                                          wire::load_be32(body.subspan(block + 4, 4)) - sack_base.value);
                }
                add("sack " + blocks);
            } else {
                add(std::format("kind {}", kind));
            }
            at += length;
        }
        return out;
    }

    std::string describe(const PacketLine& line, const wire::TcpSequence sack_base) {
        std::string out = std::format("{} {} {}",
                                      line.direction == Direction::Inbound ? '<' : '>',
                                      describe_flags(line.flags),
                                      describe_range(line.first, line.last, line.length));
        if (line.ack) {
            out += std::format(" ack {}", *line.ack);
        }
        if (line.window) {
            out += std::format(" win {}", *line.window);
        }
        if (line.urgent) {
            out += std::format(" urg {}", *line.urgent);
        }
        if (line.options.any) {
            out += " <...>";
        } else if (!line.options.list.empty()) {
            out += " <" + describe_options(encode_options(line.options, sack_base), sack_base) + ">";
        }
        return out;
    }

    std::string describe(const ObservedSegment& segment, const wire::TcpSequence sack_base) {
        std::string out = std::format(
            "> {} {}", describe_flags(segment.flags), describe_range(segment.first, segment.last, segment.length));
        if (segment.flags.has(wire::TcpFlag::Ack)) {
            out += std::format(" ack {}", segment.ack);
        }
        out += std::format(" win {}", segment.window);
        if (segment.urgent != 0) {
            out += std::format(" urg {}", segment.urgent);
        }
        if (!segment.options.empty()) {
            out += " <" + describe_options(segment.options, sack_base) + ">";
        }
        return out;
    }

    std::string format_time(const core::Duration since_start) {
        const auto micros            = std::chrono::duration_cast<std::chrono::microseconds>(since_start).count();
        const std::int64_t magnitude = micros < 0 ? -micros : micros;
        return std::format("{}{}.{:06}", micros < 0 ? '-' : '+', magnitude / 1'000'000, magnitude % 1'000'000);
    }

}  // namespace aloe::scenario
