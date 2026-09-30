#include "aes67/audio_packet.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace aes67 {
namespace {

constexpr std::int32_t min_l24 = -(1 << 23);
constexpr std::int32_t max_l24 = (1 << 23) - 1;

void append_sample(std::vector<std::uint8_t>& output, std::int32_t sample) {
    if (sample < min_l24 || sample > max_l24) {
        throw std::out_of_range("L24 sample is outside the signed 24-bit range");
    }
    const auto value = static_cast<std::uint32_t>(sample) & 0x00ff'ffffU;
    output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    output.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::int32_t read_sample(const std::uint8_t* bytes) {
    auto value = (static_cast<std::int32_t>(bytes[0]) << 16) |
                 (static_cast<std::int32_t>(bytes[1]) << 8) |
                 static_cast<std::int32_t>(bytes[2]);
    if ((value & 0x0080'0000) != 0) {
        value |= static_cast<std::int32_t>(0xff00'0000U);
    }
    return value;
}

void validate_format(const AudioFormat& format) {
    if (format.channels == 0) {
        throw std::invalid_argument("audio format must have at least one channel");
    }
}

}  // namespace

std::vector<std::uint8_t> encode_l24(const AudioPacket& packet) {
    validate_format(packet.format);
    const auto expected_samples = static_cast<std::size_t>(packet.format.channels) *
                                  AudioFormat::samples_per_packet;
    if (packet.samples.size() != expected_samples) {
        throw std::invalid_argument("packet must contain exactly 1 ms of audio");
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(packet.samples.size() * 3);
    for (const auto sample : packet.samples) {
        append_sample(payload, sample);
    }
    return payload;
}

AudioPacket decode_l24(const AudioFormat& format, std::uint32_t rtp_timestamp,
                       std::span<const std::uint8_t> payload) {
    validate_format(format);
    const auto expected_samples = static_cast<std::size_t>(format.channels) *
                                  AudioFormat::samples_per_packet;
    if (payload.size() != expected_samples * 3) {
        throw std::invalid_argument("payload must contain exactly 1 ms of L24 audio");
    }

    AudioPacket packet{format, rtp_timestamp, {}};
    packet.samples.reserve(expected_samples);
    for (std::size_t offset = 0; offset < payload.size(); offset += 3) {
        packet.samples.push_back(read_sample(payload.data() + offset));
    }
    return packet;
}

AudioPacket resample_linear(const AudioPacket& packet, std::size_t output_samples_per_channel) {
    validate_format(packet.format);
    const auto input_samples_per_channel = packet.samples.size() / packet.format.channels;
    if (input_samples_per_channel == 0 || packet.samples.size() % packet.format.channels != 0 ||
        output_samples_per_channel == 0) {
        throw std::invalid_argument("cannot resample an empty or malformed audio packet");
    }
    if (input_samples_per_channel == output_samples_per_channel) return packet;

    AudioPacket result{packet.format, packet.rtp_timestamp, {}};
    result.samples.resize(output_samples_per_channel * packet.format.channels);
    for (std::size_t frame = 0; frame < output_samples_per_channel; ++frame) {
        const auto position = output_samples_per_channel == 1
                                  ? 0.0
                                  : static_cast<double>(frame) * (input_samples_per_channel - 1) /
                                        (output_samples_per_channel - 1);
        const auto left = static_cast<std::size_t>(position);
        const auto right = std::min(left + 1, input_samples_per_channel - 1);
        const auto fraction = position - static_cast<double>(left);
        for (std::size_t channel = 0; channel < packet.format.channels; ++channel) {
            const auto first = packet.samples[left * packet.format.channels + channel];
            const auto second = packet.samples[right * packet.format.channels + channel];
            result.samples[frame * packet.format.channels + channel] = static_cast<std::int32_t>(
                std::llround(static_cast<double>(first) + (static_cast<double>(second - first) * fraction)));
        }
    }
    return result;
}

}  // namespace aes67
