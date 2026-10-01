#include "aes67/receive_path.hpp"

#include "aes67/audio_packet.hpp"
#include "aes67/rtp_packet.hpp"

#include <limits>
#include <stdexcept>

namespace aes67 {
namespace {

bool is_old_sequence(std::uint16_t sequence, std::uint16_t previous) {
    const auto delta = static_cast<std::uint16_t>(sequence - previous);
    return delta >= 0x8000U;
}

}  // namespace

ReceivePath::ReceivePath(AudioFormat format, std::uint8_t payload_type,
                         std::size_t jitter_capacity_packets)
    : format_(format), payload_type_(payload_type), jitter_buffer_(jitter_capacity_packets) {}

bool ReceivePath::ingest(std::span<const std::uint8_t> datagram) {
    const auto rtp = parse_rtp(datagram);
    if (rtp.payload_type != payload_type_) {
        throw std::invalid_argument("RTP payload type does not match configured L24 stream");
    }
    auto packet = decode_l24(format_, rtp.timestamp, rtp.payload);
    if (source_ssrc_.has_value() && *source_ssrc_ != rtp.ssrc) {
        return false;
    }
    if (last_sequence_.has_value()) {
        const auto delta = static_cast<std::uint16_t>(rtp.sequence_number - *last_sequence_);
        if (delta == 0 || is_old_sequence(rtp.sequence_number, *last_sequence_)) {
            return false;
        }
        if (delta > 1 && sequence_gaps_ <= std::numeric_limits<std::uint64_t>::max() - (delta - 1)) {
            sequence_gaps_ += delta - 1;
        }
    }
    source_ssrc_ = rtp.ssrc;
    last_sequence_ = rtp.sequence_number;
    if (!primed_) {
        jitter_buffer_.prime(rtp.timestamp);
        primed_ = true;
    }
    const auto accepted = jitter_buffer_.push(std::move(packet));
    if (accepted) last_timestamp_ = rtp.timestamp;
    return accepted;
}

void ReceivePath::prime(std::uint32_t first_timestamp) {
    jitter_buffer_.prime(first_timestamp);
    primed_ = true;
    source_ssrc_.reset();
    last_sequence_.reset();
    sequence_gaps_ = 0;
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

std::uint64_t ReceivePath::sequence_gaps() const noexcept {
    return sequence_gaps_;
}

}  // namespace aes67
