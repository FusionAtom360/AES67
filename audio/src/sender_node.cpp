#include "aes67/alsa_source.hpp"
#include "aes67/ptp_clock.hpp"
#include "aes67/rtp_sender.hpp"
#include "aes67/sdp_session.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#ifndef _WIN32
#include <csignal>
#endif

namespace {

std::atomic_bool running{true};

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " [--destination ADDRESS] [--port PORT] [--interface ADDRESS]"
              << " [--payload-type TYPE] [--device ALSA_DEVICE] [--sdp FILE]\n";
}

std::uint32_t parse_unsigned(const std::string& value, const char* option,
                             std::uint32_t maximum) {
    std::size_t parsed = 0;
    const auto number = std::stoull(value, &parsed, 10);
    if (parsed != value.size() || number > maximum) {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    return static_cast<std::uint32_t>(number);
}

void write_sdp(const std::string& path, const aes67::SdpSession& session) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) throw std::runtime_error("could not open SDP output file: " + path);
    output << session.generate();
    if (!output) throw std::runtime_error("could not write SDP output file: " + path);
}

}  // namespace

int main(int argc, char** argv) {
    std::string destination = "239.69.0.1";
    std::uint16_t port = 5004;
    std::string interface_address = "0.0.0.0";
    std::uint8_t payload_type = 96;
    std::string alsa_device = "default";
    std::string sdp_path = "stream.sdp";

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help" || option == "-h") {
                print_usage(argv[0]);
                return 0;
            }
            if (index + 1 >= argc) throw std::invalid_argument("missing value for " + option);
            const std::string value = argv[++index];
            if (option == "--destination") {
                destination = value;
            } else if (option == "--port") {
                port = static_cast<std::uint16_t>(parse_unsigned(value, "--port", 65535));
            } else if (option == "--interface") {
                interface_address = value;
            } else if (option == "--payload-type") {
                payload_type = static_cast<std::uint8_t>(parse_unsigned(value, "--payload-type", 127));
            } else if (option == "--device") {
                alsa_device = value;
            } else if (option == "--sdp") {
                sdp_path = value;
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }

#ifndef _WIN32
        std::signal(SIGINT, [](int) { running = false; });
        std::signal(SIGTERM, [](int) { running = false; });
#endif

        aes67::SdpSession session;
        session.connection_address = destination;
        session.media_port = port;
        session.payload_type = payload_type;
        session.ptp_reference_clock = true;
        session.direct_media_clock = true;
        write_sdp(sdp_path, session);

        aes67::AlsaSource source(alsa_device);
        aes67::RtpSender sender({destination, port, interface_address, payload_type, true});
        aes67::PtpClock ptp_clock;
        auto rtp_timestamp = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(static_cast<long double>(ptp_clock.now_ns()) *
                                       aes67::AudioFormat::sample_rate / 1'000'000'000.0L));
        std::uint16_t sequence_number = 0;

        std::cout << "Publishing " << destination << ":" << port
                  << " as " << sdp_path << " from ALSA device " << alsa_device << "\n";
        while (running) {
            auto packet = source.read_packet(rtp_timestamp);
            sender.send(packet, sequence_number++);
            rtp_timestamp += aes67::AudioFormat::samples_per_packet;
        }
    } catch (const std::exception& error) {
        std::cerr << "sender node failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
