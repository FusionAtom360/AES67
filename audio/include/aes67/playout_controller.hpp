#pragma once

#include <cstddef>
#include <cstdint>

namespace aes67 {

struct PlayoutConfig {
    std::uint32_t sample_rate = 48'000;
    std::uint32_t packet_samples = 48;
    std::uint32_t delay_ms = 20;
    std::size_t target_buffer_packets = 20;
    bool absolute_rtp_clock = false;
};

class PlayoutController {
public:
    explicit PlayoutController(PlayoutConfig config = {});

    void observe_packet(std::uint32_t rtp_timestamp, std::uint64_t ptp_time_ns);
    bool mapped() const noexcept;
    std::uint64_t due_time_ns(std::uint32_t rtp_timestamp, std::uint64_t now_ptp_time_ns) const;
    std::size_t output_samples(std::size_t buffered_packets);
    double clock_ratio() const noexcept;

private:
    PlayoutConfig config_;
    bool mapped_ = false;
    std::uint32_t reference_rtp_timestamp_ = 0;
    std::uint64_t reference_ptp_time_ns_ = 0;
    std::uint32_t last_rtp_timestamp_ = 0;
    std::uint64_t last_ptp_time_ns_ = 0;
    double clock_ratio_ = 1.0;
    double frame_remainder_ = 0.0;
};

}  // namespace aes67
