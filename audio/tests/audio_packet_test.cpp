#include "aes67/audio_packet.hpp"
#include "aes67/jitter_buffer.hpp"
#include "aes67/playout_controller.hpp"
#include "aes67/receive_path.hpp"
#include "aes67/rtp_packet.hpp"
#include "aes67/sdp_session.hpp"

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

    aes67::ReceivePath auto_primed_path(format);
    assert(auto_primed_path.ingest(first_datagram));
    assert(auto_primed_path.buffered_packets() == 1);

    const auto resampled = aes67::resample_linear(source, 24);
    assert(resampled.samples.size() == format.channels * 24);
    assert(resampled.samples.front() == source.samples.front());

    aes67::PlayoutController playout({48'000, 48, 20, 20});
    playout.observe_packet(48'000, 1'000'000'000);
    assert(playout.mapped());
    assert(playout.due_time_ns(48'000, 1'000'000'000) == 1'020'000'000);
    assert(playout.output_samples(20) == 48);
    playout.observe_packet(48'048, 1'001'000'000);
    assert(playout.clock_ratio() > 0.99 && playout.clock_ratio() < 1.01);

    aes67::PlayoutController absolute_playout({48'000, 48, 20, 20, true});
    absolute_playout.observe_packet(48'000, 1'000'000'000);
    assert(absolute_playout.due_time_ns(48'000, 1'000'000'000) == 1'020'000'000);

    bool rejected_short_rtp = false;
    try {
        aes67::parse_rtp(std::vector<std::uint8_t>(11));
    } catch (const std::invalid_argument&) {
        rejected_short_rtp = true;
    }
    assert(rejected_short_rtp);

    aes67::SdpSession session;
    session.session_name = "Studio monitor";
    session.connection_address = "239.69.0.1";
    session.media_port = 5004;
    session.payload_type = 97;
    session.channels = 2;
    const auto sdp = session.generate();
    const auto parsed_sdp = aes67::SdpSession::parse(sdp);
    assert(parsed_sdp.session_name == session.session_name);
    assert(parsed_sdp.connection_address == session.connection_address);
    assert(parsed_sdp.media_port == session.media_port);
    assert(parsed_sdp.payload_type == session.payload_type);
    assert(parsed_sdp.encoding_name == "L24");
    assert(parsed_sdp.clock_rate == 48000);
    assert(parsed_sdp.channels == 2);

    bool rejected_incomplete_sdp = false;
    try {
        aes67::SdpSession::parse("v=0\r\ns=missing fields\r\n");
    } catch (const std::invalid_argument&) {
        rejected_incomplete_sdp = true;
    }
    assert(rejected_incomplete_sdp);

    std::cout << "audio packet tests passed\n";
}
