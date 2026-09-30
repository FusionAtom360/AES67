#include "aes67/receive_path.hpp"

#include "aes67/audio_packet.hpp"
#include "aes67/rtp_packet.hpp"

#include <stdexcept>

namespace aes67 {

ReceivePath::ReceivePath(AudioFormat format, std::uint8_t payload_type,
                         std::size_t jitter_capacity_packets)
    : format_(format), payload_type_(payload_type), jitter_buffer_(jitter_capacity_packets) {}

bool ReceivePath::ingest(std::span<const std::uint8_t> datagram) {
    const auto rtp = parse_rtp(datagram);
    last_timestamp_ = rtp.timestamp;
    if (rtp.payload_type != payload_type_) {
        throw std::invalid_argument("RTP payload type does not match configured L24 stream");
    }
    if (!primed_) {
        jitter_buffer_.prime(rtp.timestamp);
        primed_ = true;
    }
    return jitter_buffer_.push(decode_l24(format_, rtp.timestamp, rtp.payload));
}

void ReceivePath::prime(std::uint32_t first_timestamp) {
    jitter_buffer_.prime(first_timestamp);
    primed_ = true;
}

bool ReceivePath::render_next(AlsaSink& sink) {
    const auto packet = take_next();
    if (!packet.has_value()) {
        return false;
    }
    sink.write(*packet);
    return true;
}

std::optional<AudioPacket> ReceivePath::take_next() {
    return jitter_buffer_.pop_next();
}

std::optional<std::uint32_t> ReceivePath::next_timestamp() const noexcept {
    return jitter_buffer_.next_timestamp();
}

std::optional<std::uint32_t> ReceivePath::last_timestamp() const noexcept {
    return last_timestamp_;
}

std::size_t ReceivePath::buffered_packets() const noexcept {
    return jitter_buffer_.size();
}

}  // namespace aes67
