#include "aes67/alsa_sink.hpp"
#include "aes67/audio_packet.hpp"
#include "aes67/playout_controller.hpp"
#include "aes67/ptp_clock.hpp"
#include "aes67/receive_path.hpp"
#include "aes67/sdp_session.hpp"
#include "aes67/udp_receiver.hpp"

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

std::atomic_bool running{true};

#ifndef _WIN32
class LocalControl {
public:
    explicit LocalControl(const std::string& path) : path_(path) {
        if (path_.empty()) return;
        if (path_.size() >= sizeof(sockaddr_un::sun_path)) {
            throw std::invalid_argument("control socket path is too long");
        }
        socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket_ < 0) throw std::runtime_error("could not create control socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::copy(path_.begin(), path_.end(), address.sun_path);
        ::unlink(path_.c_str());
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
            ::listen(socket_, 4) < 0 || ::fcntl(socket_, F_SETFL, O_NONBLOCK) < 0) {
            close_socket();
            throw std::runtime_error("could not bind control socket: " + std::string(std::strerror(errno)));
        }
    }

    ~LocalControl() {
        close_socket();
        if (!path_.empty()) ::unlink(path_.c_str());
    }

    std::optional<double> poll_volume() const {
        if (socket_ < 0) return std::nullopt;
        const auto client = ::accept(socket_, nullptr, nullptr);
        if (client < 0) return std::nullopt;
        char buffer[128]{};
        const auto received = ::read(client, buffer, sizeof(buffer) - 1);
        ::close(client);
        if (received <= 0) return std::nullopt;
        std::string command(buffer, static_cast<std::size_t>(received));
        if (!command.starts_with("volume ")) return std::nullopt;
        try {
            const auto value = std::stod(command.substr(7));
            if (value < 0.0 || value > 1.0) return std::nullopt;
            return value;
        } catch (...) {
            return std::nullopt;
        }
    }

private:
    void close_socket() {
        if (socket_ >= 0) ::close(socket_);
        socket_ = -1;
    }

    int socket_ = -1;
    std::string path_;
};
#else
class LocalControl {
public:
    explicit LocalControl(const std::string&) {}
    std::optional<double> poll_volume() const { return std::nullopt; }
};
#endif

void apply_volume(aes67::AudioPacket& packet, double volume) {
    for (auto& sample : packet.samples) {
        const auto scaled = static_cast<std::int64_t>(std::llround(sample * volume));
        sample = static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, -(1 << 23), (1 << 23) - 1));
    }
}

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " [--sdp FILE | [--bind ADDRESS] [--port PORT] [--multicast ADDRESS] [--interface ADDRESS]"
              << " [--payload-type TYPE]"
              << "] [--first-timestamp TIMESTAMP] [--playout-delay-ms MS] [--device ALSA_DEVICE]"
              << " [--control-socket PATH]\n";
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
    std::uint32_t playout_delay_ms = 20;
    std::string sdp_path;
    bool manual_network_options = false;
    std::string alsa_device = "default";
    std::string control_socket;

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
            } else if (option == "--playout-delay-ms") {
                playout_delay_ms = parse_unsigned(value, "--playout-delay-ms", 10'000);
            } else if (option == "--device") {
                alsa_device = value;
            } else if (option == "--control-socket") {
                control_socket = value;
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
        LocalControl control(control_socket);
        double volume = 1.0;
        aes67::PtpClock ptp_clock;
        aes67::PlayoutController playout({aes67::AudioFormat::sample_rate, aes67::AudioFormat::samples_per_packet,
                          playout_delay_ms, std::max<std::size_t>(1, playout_delay_ms), sdp.has_value()});

        std::cout << "Listening on " << bind_address << ":" << port
                  << (multicast_group.empty() ? "" : " (multicast " + multicast_group + ")")
              << ", writing L24 to ALSA device " << alsa_device << "\n";
        while (running) {
            if (const auto requested_volume = control.poll_volume(); requested_volume.has_value()) {
                volume = *requested_volume;
            }
            if (receiver.receive_once(path)) {
                if (const auto timestamp = path.last_timestamp(); timestamp.has_value()) {
                    playout.observe_packet(*timestamp, ptp_clock.now_ns());
                }
            }
            const auto next_timestamp = path.next_timestamp();
            const auto now_ptp_time_ns = ptp_clock.now_ns();
            if (!next_timestamp.has_value() || !playout.mapped() ||
                now_ptp_time_ns < playout.due_time_ns(*next_timestamp, now_ptp_time_ns)) {
                if (next_timestamp.has_value() && playout.mapped() && path.buffered_packets() > 0) {
                    const auto deadline = playout.due_time_ns(*next_timestamp, now_ptp_time_ns);
                    if (deadline > now_ptp_time_ns) ptp_clock.sleep_until_ns(deadline);
                }
                continue;
            }
            if (const auto packet = path.take_next(); packet.has_value()) {
                const auto output_samples = playout.output_samples(path.buffered_packets() + 1);
                auto output = aes67::resample_linear(*packet, output_samples);
                apply_volume(output, volume);
                sink.write(output);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "speaker node failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
