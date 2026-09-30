#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aes67 {

struct AudioFormat {
    static constexpr std::uint32_t sample_rate = 48'000;
    static constexpr std::uint16_t bits_per_sample = 24;
    static constexpr std::uint16_t packet_duration_ms = 1;
    static constexpr std::uint32_t samples_per_packet = sample_rate / 1'000;

    std::uint16_t channels = 2;
};

struct AudioPacket {
    AudioFormat format;
    std::uint32_t rtp_timestamp = 0;
    std::vector<std::int32_t> samples;
};

// Encode interleaved signed PCM samples as network-order L24 payload bytes.
std::vector<std::uint8_t> encode_l24(const AudioPacket& packet);

// Decode a complete L24 payload into interleaved signed PCM samples.
AudioPacket decode_l24(const AudioFormat& format, std::uint32_t rtp_timestamp,
                       std::span<const std::uint8_t> payload);

}  // namespace aes67
