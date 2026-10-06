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
#include <limits>
#include <stdexcept>
#include <vector>

#undef assert
#define assert(condition) do { \
    if (!(condition)) throw std::runtime_error("test assertion failed: " #condition); \
} while (false)

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
    assert(receive_path.ingest(first_datagram));
    assert(receive_path.ingest(second_datagram));
    assert(receive_path.buffered_packets() == 2);
    assert(!receive_path.ingest(first_datagram));
    assert(receive_path.sequence_gaps() == 0);

    auto gap_datagram = make_rtp(10, 48'096, 0x12345678, second_payload);
    assert(receive_path.ingest(gap_datagram));
    assert(receive_path.sequence_gaps() == 1);

    auto other_source = make_rtp(11, 48'144, 0x87654321, second_payload);
    assert(!receive_path.ingest(other_source));
    assert(receive_path.sequence_gaps() == 1);

    aes67::JitterBuffer wrapped_buffer;
    const auto wrapped_timestamp = std::numeric_limits<std::uint32_t>::max() - 15;
    wrapped_buffer.prime(wrapped_timestamp);
    assert(wrapped_buffer.push(AudioPacket{format, wrapped_timestamp, source.samples}));
    assert(wrapped_buffer.push(AudioPacket{format, 32, source.samples}));
    assert(wrapped_buffer.pop_next().has_value());
    assert(wrapped_buffer.pop_next().has_value());

    aes67::JitterBuffer gap_buffer;
    gap_buffer.prime(48'000);
    assert(gap_buffer.push(AudioPacket{format, 48'000, source.samples}));
    assert(gap_buffer.push(AudioPacket{format, 48'096, source.samples}));
    assert(gap_buffer.pop_next()->rtp_timestamp == 48'000);
    assert(gap_buffer.pop_next()->rtp_timestamp == 48'096);

    aes67::ReceivePath malformed_path(format);
    auto malformed_datagram = first_datagram;
    malformed_datagram.resize(12 + 3);
    bool rejected_malformed_payload = false;
    try {
        malformed_path.ingest(malformed_datagram);
    } catch (const std::invalid_argument&) {
        rejected_malformed_payload = true;
    }
    assert(rejected_malformed_payload);
    assert(!malformed_path.next_timestamp().has_value());
    assert(malformed_path.ingest(first_datagram));

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
    playout.observe_packet(48'096, 900'000'000);
    assert(playout.due_time_ns(48'096, 900'000'000) == 920'000'000);

    aes67::PlayoutController low_buffer_playout({48'000, 48, 20, 20});
    assert(low_buffer_playout.output_samples(1) < 48);
    aes67::PlayoutController high_buffer_playout({48'000, 100, 20, 20});
    assert(high_buffer_playout.output_samples(40) > 100);

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

    auto wrong_payload_type = first_datagram;
    wrong_payload_type[1] = 95;
    bool rejected_wrong_payload_type = false;
    try {
        aes67::ReceivePath(format, 96).ingest(wrong_payload_type);
    } catch (const std::invalid_argument&) {
        rejected_wrong_payload_type = true;
    }
    assert(rejected_wrong_payload_type);

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

    auto mono_sdp = sdp;
    const auto channel_map = mono_sdp.find("L24/48000/2");
    mono_sdp.replace(channel_map, std::string("L24/48000/1").size(), "L24/48000/1");
    bool rejected_mono_sdp = false;
    try {
        aes67::SdpSession::parse(mono_sdp);
    } catch (const std::invalid_argument&) {
        rejected_mono_sdp = true;
    }
    assert(rejected_mono_sdp);

    auto unsynchronized_sdp = sdp;
    const auto clock_attribute = unsynchronized_sdp.find("a=ts-refclk");
    unsynchronized_sdp.erase(clock_attribute, unsynchronized_sdp.find("\r\n", clock_attribute) - clock_attribute + 2);
    bool rejected_unsynchronized_sdp = false;
    try {
        aes67::SdpSession::parse(unsynchronized_sdp);
    } catch (const std::invalid_argument&) {
        rejected_unsynchronized_sdp = true;
    }
    assert(rejected_unsynchronized_sdp);

    bool rejected_incomplete_sdp = false;
    try {
        aes67::SdpSession::parse("v=0\r\ns=missing fields\r\n");
    } catch (const std::invalid_argument&) {
        rejected_incomplete_sdp = true;
    }
    assert(rejected_incomplete_sdp);

    std::cout << "audio packet tests passed\n";
}
