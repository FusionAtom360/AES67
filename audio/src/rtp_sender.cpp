#include "aes67/rtp_sender.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aes67 {

struct RtpSender::Impl {
#ifndef _WIN32
    int socket = -1;
    sockaddr_in destination{};
    std::string interface_address = "0.0.0.0";
    bool mark_dscp_ef = true;
    bool multicast = false;
#endif
    std::uint8_t payload_type = 96;
    std::uint32_t ssrc = 0x41455336;
};

RtpSender::RtpSender(RtpSenderConfig config) : impl_(new Impl{}) {
#ifdef _WIN32
    (void)config;
    delete impl_;
    impl_ = nullptr;
    throw std::runtime_error("RTP sender is not implemented for Windows yet");
#else
    impl_->payload_type = config.payload_type;
    impl_->interface_address = config.interface_address;
    impl_->mark_dscp_ef = config.mark_dscp_ef;
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->socket < 0) throw std::runtime_error(std::string("could not create RTP socket: ") + std::strerror(errno));

    if (config.mark_dscp_ef) {
        const int dscp_ef = 46 << 2;
        if (::setsockopt(impl_->socket, IPPROTO_IP, IP_TOS, &dscp_ef, sizeof(dscp_ef)) < 0) {
            throw std::runtime_error(std::string("could not set RTP DSCP EF: ") + std::strerror(errno));
        }
    }
    impl_->destination.sin_family = AF_INET;
    impl_->destination.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.destination_address.c_str(), &impl_->destination.sin_addr) != 1) {
        throw std::invalid_argument("RTP destination is not IPv4: " + config.destination_address);
    }
    impl_->multicast = IN_MULTICAST(ntohl(impl_->destination.sin_addr.s_addr));
    if (impl_->multicast && config.interface_address != "0.0.0.0") {
        in_addr interface_address{};
        if (inet_pton(AF_INET, config.interface_address.c_str(), &interface_address) != 1 ||
            ::setsockopt(impl_->socket, IPPROTO_IP, IP_MULTICAST_IF,
                         &interface_address, sizeof(interface_address)) < 0) {
            throw std::invalid_argument("invalid multicast interface: " + config.interface_address);
        }
    }
#endif
}

RtpSender::~RtpSender() {
#ifndef _WIN32
    if (impl_ != nullptr && impl_->socket >= 0) ::close(impl_->socket);
#endif
    delete impl_;
}

void RtpSender::send(const AudioPacket& packet, std::uint16_t sequence_number) {
#ifdef _WIN32
    (void)packet;
    (void)sequence_number;
    throw std::runtime_error("RTP sender is not implemented for Windows yet");
#else
    const auto payload = encode_l24(packet);
    std::vector<std::uint8_t> datagram(12 + payload.size());
    datagram[0] = 0x80;
    datagram[1] = impl_->payload_type;
    datagram[2] = static_cast<std::uint8_t>(sequence_number >> 8);
    datagram[3] = static_cast<std::uint8_t>(sequence_number);
    datagram[4] = static_cast<std::uint8_t>(packet.rtp_timestamp >> 24);
    datagram[5] = static_cast<std::uint8_t>(packet.rtp_timestamp >> 16);
    datagram[6] = static_cast<std::uint8_t>(packet.rtp_timestamp >> 8);
    datagram[7] = static_cast<std::uint8_t>(packet.rtp_timestamp);
    datagram[8] = static_cast<std::uint8_t>(impl_->ssrc >> 24);
    datagram[9] = static_cast<std::uint8_t>(impl_->ssrc >> 16);
    datagram[10] = static_cast<std::uint8_t>(impl_->ssrc >> 8);
    datagram[11] = static_cast<std::uint8_t>(impl_->ssrc);
    std::copy(payload.begin(), payload.end(), datagram.begin() + 12);
    const auto send_datagram = [&]() {
        return ::sendto(impl_->socket, datagram.data(), datagram.size(), 0,
                        reinterpret_cast<const sockaddr*>(&impl_->destination), sizeof(impl_->destination));
    };
    auto sent = send_datagram();
    const auto recoverable_error = [](int error) {
        return error == EINTR || error == ENETDOWN || error == ENETUNREACH ||
               error == EHOSTUNREACH || error == ENODEV || error == ENOBUFS || error == EIO;
    };
    if (sent != static_cast<ssize_t>(datagram.size()) && recoverable_error(errno)) {
        ::close(impl_->socket);
        impl_->socket = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (impl_->socket >= 0) {
            if (impl_->mark_dscp_ef) {
                const int dscp_ef = 46 << 2;
                (void)::setsockopt(impl_->socket, IPPROTO_IP, IP_TOS, &dscp_ef, sizeof(dscp_ef));
            }
            if (impl_->multicast && impl_->interface_address != "0.0.0.0") {
                in_addr interface_address{};
                if (inet_pton(AF_INET, impl_->interface_address.c_str(), &interface_address) == 1) {
                    (void)::setsockopt(impl_->socket, IPPROTO_IP, IP_MULTICAST_IF,
                                       &interface_address, sizeof(interface_address));
                }
            }
            sent = send_datagram();
        }
    }
    if (sent != static_cast<ssize_t>(datagram.size())) {
        throw std::runtime_error(std::string("RTP send failed: ") + std::strerror(errno));
    }
#endif
}

}  // namespace aes67
