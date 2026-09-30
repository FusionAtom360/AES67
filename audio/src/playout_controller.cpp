#include "aes67/playout_controller.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace aes67 {
namespace {

std::int32_t timestamp_delta(std::uint32_t newer, std::uint32_t older) {
    return static_cast<std::int32_t>(newer - older);
}

double clamp_ratio(double ratio) {
    return std::clamp(ratio, 0.98, 1.02);
}

}  // namespace

PlayoutController::PlayoutController(PlayoutConfig config) : config_(config) {
    if (config_.sample_rate == 0 || config_.packet_samples == 0 || config_.target_buffer_packets == 0) {
        throw std::invalid_argument("playout configuration values must be positive");
    }
}

void PlayoutController::observe_packet(std::uint32_t rtp_timestamp, std::uint64_t ptp_time_ns) {
    if (!mapped_) {
        mapped_ = true;
        reference_rtp_timestamp_ = rtp_timestamp;
        reference_ptp_time_ns_ = ptp_time_ns;
        last_rtp_timestamp_ = rtp_timestamp;
        last_ptp_time_ns_ = ptp_time_ns;
        return;
    }

    const auto rtp_delta = timestamp_delta(rtp_timestamp, last_rtp_timestamp_);
    const auto ptp_delta = ptp_time_ns - last_ptp_time_ns_;
    if (rtp_delta > 0 && ptp_delta > 0) {
        const auto expected_ns = static_cast<double>(rtp_delta) * 1'000'000'000.0 / config_.sample_rate;
        const auto observed_ratio = expected_ns / static_cast<double>(ptp_delta);
        // Smooth network jitter while tracking the sender/receiver clock offset.
        clock_ratio_ = clamp_ratio(clock_ratio_ * 0.995 + observed_ratio * 0.005);
    }
    last_rtp_timestamp_ = rtp_timestamp;
    last_ptp_time_ns_ = ptp_time_ns;
}

bool PlayoutController::mapped() const noexcept {
    return mapped_;
}

std::uint64_t PlayoutController::due_time_ns(std::uint32_t rtp_timestamp,
                                             std::uint64_t now_ptp_time_ns) const {
    if (!mapped_) return std::numeric_limits<std::uint64_t>::max();
    if (config_.absolute_rtp_clock) {
        const auto now_ticks = static_cast<std::uint64_t>(
            static_cast<long double>(now_ptp_time_ns) * config_.sample_rate / 1'000'000'000.0L);
        const auto delta = static_cast<std::int64_t>(static_cast<std::int32_t>(
            rtp_timestamp - static_cast<std::uint32_t>(now_ticks)));
        const auto target_ticks = static_cast<std::int64_t>(now_ticks) + delta;
        const auto target_ns = target_ticks * 1'000'000'000LL /
                               static_cast<std::int64_t>(config_.sample_rate);
        const auto due = target_ns + static_cast<std::int64_t>(config_.delay_ms) * 1'000'000LL;
        return due > 0 ? static_cast<std::uint64_t>(due) : 0;
    }
    const auto delta = static_cast<std::int64_t>(timestamp_delta(rtp_timestamp, reference_rtp_timestamp_));
    const auto media_ns = delta * 1'000'000'000LL / static_cast<std::int64_t>(config_.sample_rate);
    const auto delay_ns = static_cast<std::int64_t>(config_.delay_ms) * 1'000'000LL;
    const auto due = static_cast<std::int64_t>(reference_ptp_time_ns_) + delay_ns + media_ns;
    return due > 0 ? static_cast<std::uint64_t>(due) : 0;
}

std::size_t PlayoutController::output_samples(std::size_t buffered_packets) {
    const auto queue_error = static_cast<double>(buffered_packets) -
                              static_cast<double>(config_.target_buffer_packets);
    // Positive queue error consumes the buffer faster; clock_ratio_ handles source drift.
    const auto queue_ratio = std::clamp(1.0 + queue_error * 0.001, 0.98, 1.02);
    const auto desired = static_cast<double>(config_.packet_samples) / (clock_ratio_ * queue_ratio);
    const auto with_remainder = desired + frame_remainder_;
    const auto output = static_cast<std::size_t>(std::max(1.0, std::floor(with_remainder)));
    frame_remainder_ = with_remainder - static_cast<double>(output);
    return output;
}

double PlayoutController::clock_ratio() const noexcept {
    return clock_ratio_;
}

}  // namespace aes67
