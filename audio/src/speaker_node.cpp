#include "aes67/alsa_sink.hpp"
#include "aes67/receive_path.hpp"
#include "aes67/udp_receiver.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
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
              << " [--bind ADDRESS] [--port PORT] [--payload-type TYPE]"
              << " [--first-timestamp TIMESTAMP] [--device ALSA_DEVICE]\n";
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

}  // namespace

int main(int argc, char** argv) {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 5004;
    std::uint8_t payload_type = 96;
    std::uint32_t first_timestamp = 0;
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
            if (option == "--bind") {
                bind_address = value;
            } else if (option == "--port") {
                port = static_cast<std::uint16_t>(parse_unsigned(value, "--port", 65535));
            } else if (option == "--payload-type") {
                payload_type = static_cast<std::uint8_t>(parse_unsigned(value, "--payload-type", 127));
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

        const aes67::AudioFormat format{2};
        aes67::ReceivePath path(format, payload_type);
        path.prime(first_timestamp);
        aes67::UdpReceiver receiver(port, bind_address);
        aes67::AlsaSink sink(alsa_device);

        std::cout << "Listening on " << bind_address << ":" << port
                  << ", writing stereo L24 to ALSA device " << alsa_device << "\n";
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
