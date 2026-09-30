#include "aes67/audio_packet.hpp"
#include "aes67/jitter_buffer.hpp"
#include "aes67/receive_path.hpp"
#include "aes67/rtp_packet.hpp"

#include <cassert>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

std::vector<std::uint8_t> make_rtp(std::uint16_t sequence, std::uint32_t timestamp,
                                   std::uint32_t ssrc, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> datagram(12 + payload.size());
    datagram[0] = 0x80;
    datagram[1] = 96;
    datagram[2] = static_cast<std::uint8_t>(sequence >> 8);
    datagram[3] = static_cast<std::uint8_t>(sequence);
    datagram[4] = static_cast<std::uint8_t>(timestamp >> 24);
    datagram[5] = static_cast<std::uint8_t>(timestamp >> 16);
    datagram[6] = static_cast<std::uint8_t>(timestamp >> 8);
    datagram[7] = static_cast<std::uint8_t>(timestamp);
    datagram[8] = static_cast<std::uint8_t>(ssrc >> 24);
    datagram[9] = static_cast<std::uint8_t>(ssrc >> 16);
    datagram[10] = static_cast<std::uint8_t>(ssrc >> 8);
    datagram[11] = static_cast<std::uint8_t>(ssrc);
    std::copy(payload.begin(), payload.end(), datagram.begin() + 12);
    return datagram;
}

int main() {
    using aes67::AudioFormat;
    using aes67::AudioPacket;

    const AudioFormat format{2};
    AudioPacket source{format, 96'000, {}};
    source.samples.resize(format.channels * AudioFormat::samples_per_packet);
    source.samples.front() = -(1 << 23);
    source.samples[1] = -1;
    source.samples[2] = 0;
    source.samples[3] = (1 << 23) - 1;

    const auto payload = aes67::encode_l24(source);
    assert(payload.size() == source.samples.size() * 3);
    assert(payload[0] == 0x80 && payload[1] == 0x00 && payload[2] == 0x00);
    assert(payload[3] == 0xff && payload[4] == 0xff && payload[5] == 0xff);

    const auto decoded = aes67::decode_l24(format, source.rtp_timestamp, payload);
    assert(decoded.rtp_timestamp == source.rtp_timestamp);
    assert(decoded.samples == source.samples);

    bool rejected_bad_size = false;
    try {
        aes67::decode_l24(format, 0, std::vector<std::uint8_t>(3));
    } catch (const std::invalid_argument&) {
        rejected_bad_size = true;
    }
    assert(rejected_bad_size);

    const auto first_payload = aes67::encode_l24(AudioPacket{format, 48'000, source.samples});
    const auto second_payload = aes67::encode_l24(AudioPacket{format, 48'048, source.samples});
    const auto first_datagram = make_rtp(7, 48'000, 0x12345678, first_payload);
    const auto second_datagram = make_rtp(8, 48'048, 0x12345678, second_payload);
    const auto parsed = aes67::parse_rtp(second_datagram);
    assert(parsed.sequence_number == 8);
    assert(parsed.timestamp == 48'048);
    assert(parsed.ssrc == 0x12345678);
    assert(parsed.payload.size() == second_payload.size());

    aes67::ReceivePath receive_path(format);
    receive_path.prime(48'000);
    assert(receive_path.ingest(second_datagram));
    assert(receive_path.ingest(first_datagram));
    assert(receive_path.buffered_packets() == 2);
    assert(!receive_path.ingest(first_datagram));

    bool rejected_short_rtp = false;
    try {
        aes67::parse_rtp(std::vector<std::uint8_t>(11));
    } catch (const std::invalid_argument&) {
        rejected_short_rtp = true;
    }
    assert(rejected_short_rtp);

    std::cout << "audio packet tests passed\n";
}
