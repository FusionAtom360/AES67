#include "aes67/rtp_packet.hpp"

#include <stdexcept>

namespace aes67 {
namespace {

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

}  // namespace

RtpPacket parse_rtp(std::span<const std::uint8_t> datagram) {
    if (datagram.size() < 12) {
        throw std::invalid_argument("RTP datagram is shorter than the fixed header");
    }
    const auto* bytes = datagram.data();
    if ((bytes[0] >> 6) != 2) {
        throw std::invalid_argument("RTP version is not 2");
    }

    const auto has_padding = (bytes[0] & 0x20) != 0;
    const auto has_extension = (bytes[0] & 0x10) != 0;
    const auto csrc_count = static_cast<std::size_t>(bytes[0] & 0x0f);
    std::size_t header_size = 12 + csrc_count * 4;
    if (header_size > datagram.size()) {
        throw std::invalid_argument("RTP CSRC list exceeds datagram");
    }
    if (has_extension) {
        if (header_size + 4 > datagram.size()) {
            throw std::invalid_argument("RTP extension header is truncated");
        }
        const auto extension_words = read_u16(bytes + header_size + 2);
        header_size += 4 + static_cast<std::size_t>(extension_words) * 4;
        if (header_size > datagram.size()) {
            throw std::invalid_argument("RTP extension exceeds datagram");
        }
    }

    std::size_t payload_size = datagram.size() - header_size;
    if (has_padding) {
        const auto padding = datagram.back();
        if (padding == 0 || padding > payload_size) {
            throw std::invalid_argument("RTP padding is invalid");
        }
        payload_size -= padding;
    }

    return RtpPacket{
        static_cast<std::uint8_t>(bytes[1] & 0x7f),
        read_u16(bytes + 2),
        read_u32(bytes + 4),
        read_u32(bytes + 8),
        (bytes[1] & 0x80) != 0,
        datagram.subspan(header_size, payload_size),
    };
}

}  // namespace aes67
