#include "aes67/alsa_sink.hpp"
#include "aes67/receive_path.hpp"
#include "aes67/sdp_session.hpp"
#include "aes67/udp_receiver.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <csignal>
#endif

namespace {

std::atomic_bool running{true};

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " [--sdp FILE | [--bind ADDRESS] [--port PORT] [--multicast ADDRESS] [--interface ADDRESS]"
              << " [--payload-type TYPE]"
              << "] [--first-timestamp TIMESTAMP] [--device ALSA_DEVICE]\n";
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

aes67::SdpSession read_sdp(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::invalid_argument("could not open SDP file: " + path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return aes67::SdpSession::parse(contents.str());
}

#ifndef _WIN32
bool is_multicast_address(const std::string& address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
        throw std::invalid_argument("SDP connection address is not IPv4: " + address);
    }
    return IN_MULTICAST(ntohl(parsed.s_addr));
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 5004;
    std::string multicast_group;
    std::string multicast_interface = "0.0.0.0";
    std::uint8_t payload_type = 96;
    std::optional<std::uint32_t> first_timestamp;
    std::string sdp_path;
    bool manual_network_options = false;
    std::string alsa_device = "default";

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help" || option == "-h") {
                print_usage(argv[0]);
                return 0;
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("missing value for " + option);
            }
            const std::string value = argv[++index];
            if (option == "--sdp") {
                sdp_path = value;
            } else if (option == "--bind") {
                bind_address = value;
                manual_network_options = true;
            } else if (option == "--port") {
                port = static_cast<std::uint16_t>(parse_unsigned(value, "--port", 65535));
                manual_network_options = true;
            } else if (option == "--multicast") {
                multicast_group = value;
                manual_network_options = true;
            } else if (option == "--interface") {
                multicast_interface = value;
                manual_network_options = true;
            } else if (option == "--payload-type") {
                payload_type = static_cast<std::uint8_t>(parse_unsigned(value, "--payload-type", 127));
                manual_network_options = true;
            } else if (option == "--first-timestamp") {
                first_timestamp = parse_unsigned(value, "--first-timestamp", std::numeric_limits<std::uint32_t>::max());
            } else if (option == "--device") {
                alsa_device = value;
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }

#ifndef _WIN32
        std::signal(SIGINT, [](int) { running = false; });
        std::signal(SIGTERM, [](int) { running = false; });
#endif

        if (!sdp_path.empty() && manual_network_options) {
            throw std::invalid_argument("--sdp cannot be combined with manual network options");
        }
        if (sdp_path.empty() && !first_timestamp.has_value()) {
            first_timestamp = 0;
        }

        std::optional<aes67::SdpSession> sdp;
        if (!sdp_path.empty()) {
            sdp = read_sdp(sdp_path);
            port = sdp->media_port;
            payload_type = sdp->payload_type;
            if (sdp->connection_address == "0.0.0.0") {
                throw std::invalid_argument("SDP connection address cannot be 0.0.0.0");
            }
#ifndef _WIN32
            if (is_multicast_address(sdp->connection_address)) multicast_group = sdp->connection_address;
#endif
        }

        const auto channels = sdp ? static_cast<std::uint16_t>(sdp->channels) : std::uint16_t{2};
        const aes67::AudioFormat format{channels};
        aes67::ReceivePath path(format, payload_type);
        if (first_timestamp.has_value()) path.prime(*first_timestamp);
        aes67::UdpReceiver receiver(aes67::UdpReceiverConfig{
            port, bind_address, multicast_group, multicast_interface, true});
        aes67::AlsaSink sink(alsa_device);

        std::cout << "Listening on " << bind_address << ":" << port
                  << (multicast_group.empty() ? "" : " (multicast " + multicast_group + ")")
              << ", writing L24 to ALSA device " << alsa_device << "\n";
        while (running) {
            receiver.receive_once(path);
            while (path.render_next(sink)) {
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "speaker node failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
