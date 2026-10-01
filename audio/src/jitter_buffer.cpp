#include "aes67/jitter_buffer.hpp"

#include <stdexcept>

namespace aes67 {
namespace {

bool is_before(std::uint32_t left, std::uint32_t right) {
    return static_cast<std::uint32_t>(left - right) >= 0x8000'0000U;
}

}  // namespace

JitterBuffer::JitterBuffer(std::size_t capacity_packets)
    : capacity_packets_(capacity_packets) {
    if (capacity_packets == 0) {
        throw std::invalid_argument("jitter buffer capacity must be positive");
    }
}

bool JitterBuffer::push(AudioPacket packet) {
    if (next_timestamp_.has_value() && is_before(packet.rtp_timestamp, *next_timestamp_)) {
        return false;
    }
    if (packets_.size() >= capacity_packets_ && !packets_.contains(packet.rtp_timestamp)) {
        return false;
    }
    return packets_.emplace(packet.rtp_timestamp, std::move(packet)).second;
}

void JitterBuffer::prime(std::uint32_t first_timestamp) {
    next_timestamp_ = first_timestamp;
}

std::optional<std::uint32_t> JitterBuffer::next_timestamp() const noexcept {
    return next_timestamp_;
}

std::optional<AudioPacket> JitterBuffer::pop_next() {
    if (!next_timestamp_.has_value()) {
        return std::nullopt;
    }
    const auto packet = packets_.find(*next_timestamp_);
    if (packet == packets_.end()) {
        return std::nullopt;
    }
    auto result = std::move(packet->second);
    packets_.erase(packet);
    *next_timestamp_ += AudioFormat::samples_per_packet;
    return result;
}

std::size_t JitterBuffer::size() const noexcept {
    return packets_.size();
}

}  // namespace aes67
