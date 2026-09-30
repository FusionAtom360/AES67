#include "aes67/audio_packet.hpp"
#include "aes67/receive_path.hpp"
#include "aes67/rtp_sender.hpp"
#include "aes67/udp_receiver.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

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
    sender.send(source, 7);

    aes67::ReceivePath path(format, 96);
    assert(receiver.receive_once(path));
    assert(path.last_timestamp().has_value());
    assert(*path.last_timestamp() == source.rtp_timestamp);
    const auto received = path.take_next();
    assert(received.has_value());
    assert(received->rtp_timestamp == source.rtp_timestamp);
    assert(received->samples == source.samples);

    std::cout << "sender/receiver integration test passed\n";
}