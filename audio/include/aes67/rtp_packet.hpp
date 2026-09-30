#pragma once

#include <cstdint>
#include <span>

namespace aes67 {

struct RtpPacket {
    std::uint8_t payload_type = 0;
    std::uint16_t sequence_number = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    bool marker = false;
    std::span<const std::uint8_t> payload;
};

// Parse an RTP v2 datagram, including CSRC and extension headers.
RtpPacket parse_rtp(std::span<const std::uint8_t> datagram);

}  // namespace aes67
