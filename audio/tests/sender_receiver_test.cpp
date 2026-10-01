#include "aes67/audio_packet.hpp"
#include "aes67/receive_path.hpp"
#include "aes67/rtp_sender.hpp"
#include "aes67/udp_receiver.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

#undef assert
#define assert(condition) do { \
    if (!(condition)) throw std::runtime_error("test assertion failed: " #condition); \
} while (false)

int main() {
    constexpr std::uint16_t port = 46067;
    const aes67::AudioFormat format{2};
    aes67::AudioPacket source{format, 48'000, {}};
    source.samples.resize(format.channels * aes67::AudioFormat::samples_per_packet);
    for (std::size_t index = 0; index < source.samples.size(); ++index) {
        source.samples[index] = static_cast<std::int32_t>(index) - 48;
    }

    aes67::UdpReceiver receiver({port, "127.0.0.1", "", "0.0.0.0", false, 1000});
    aes67::RtpSender sender({"127.0.0.1", port, "0.0.0.0", 96, false});
    const auto send_time = std::chrono::steady_clock::now();
    sender.send(source, 7);

    aes67::ReceivePath path(format, 96);
    assert(receiver.receive_once(path));
    assert(path.last_timestamp().has_value());
    assert(*path.last_timestamp() == source.rtp_timestamp);
    const auto received = path.take_next();
    assert(received.has_value());
    assert(received->rtp_timestamp == source.rtp_timestamp);
    assert(received->samples == source.samples);
    const auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - send_time).count();
    assert(latency_us >= 0);

    std::cout << "sender/receiver integration test passed; latency_us=" << latency_us << "\n";

    if (std::getenv("AES67_TEST_MULTICAST") != nullptr) {
        constexpr std::uint16_t multicast_port = 46068;
        aes67::UdpReceiver multicast_receiver({multicast_port, "0.0.0.0", "239.69.0.2", "0.0.0.0", false, 100});
        aes67::RtpSender multicast_sender({"239.69.0.2", multicast_port, "0.0.0.0", 96, false});
        multicast_sender.send(source, 8);
        aes67::ReceivePath multicast_path(format, 96);
        assert(multicast_receiver.receive_once(multicast_path));
        std::cout << "multicast sender/receiver test passed\n";
    }
}
