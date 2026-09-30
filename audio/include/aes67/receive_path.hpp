#pragma once

#include "aes67/alsa_sink.hpp"
#include "aes67/jitter_buffer.hpp"

#include <cstdint>
#include <span>

namespace aes67 {

class ReceivePath {
public:
    ReceivePath(AudioFormat format, std::uint8_t payload_type = 96,
                std::size_t jitter_capacity_packets = 64);

    // Parse RTP and enqueue one L24 packet. Returns false for a duplicate, late,
    // or over-capacity packet.
    bool ingest(std::span<const std::uint8_t> datagram);
    void prime(std::uint32_t first_timestamp);

    // Write the next timestamp-contiguous packet. Returns false while waiting
    // for the next packet in sequence.
    bool render_next(AlsaSink& sink);
    std::size_t buffered_packets() const noexcept;

private:
    AudioFormat format_;
    std::uint8_t payload_type_;
    JitterBuffer jitter_buffer_;
};

}  // namespace aes67
