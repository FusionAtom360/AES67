#pragma once

#include "aes67/audio_packet.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace aes67 {

class JitterBuffer {
public:
    explicit JitterBuffer(std::size_t capacity_packets = 64);

    // Packets are keyed by RTP timestamp; duplicates and late packets are dropped
    // using RTP serial-number arithmetic so the 32-bit timestamp can wrap.
    bool push(AudioPacket packet);
    void prime(std::uint32_t first_timestamp);
    std::optional<std::uint32_t> next_timestamp() const noexcept;
    std::optional<AudioPacket> pop_next();
    std::size_t size() const noexcept;

private:
    std::size_t capacity_packets_;
    std::optional<std::uint32_t> next_timestamp_;
    std::map<std::uint32_t, AudioPacket> packets_;
};

}  // namespace aes67
