#include "aes67/udp_receiver.hpp"

#include <array>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aes67 {

UdpReceiver::UdpReceiver(std::uint16_t port, std::string bind_address) {
#ifdef _WIN32
    (void)port;
    (void)bind_address;
    throw std::runtime_error("UDP receiver is not implemented for Windows yet");
#else
    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) {
        throw std::runtime_error(std::string("could not create UDP socket: ") + std::strerror(errno));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1 ||
        ::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(socket_);
        socket_ = -1;
        throw std::runtime_error(std::string("could not bind UDP socket: ") + std::strerror(errno));
    }
#endif
}

UdpReceiver::~UdpReceiver() {
#ifndef _WIN32
    if (socket_ >= 0) {
        ::close(socket_);
    }
#endif
}

bool UdpReceiver::receive_once(ReceivePath& path) {
#ifdef _WIN32
    (void)path;
    throw std::runtime_error("UDP receiver is not implemented for Windows yet");
#else
    std::array<std::uint8_t, 2048> datagram{};
    const auto received = ::recv(socket_, datagram.data(), datagram.size(), 0);
    if (received < 0) {
        throw std::runtime_error(std::string("UDP receive failed: ") + std::strerror(errno));
    }
    return path.ingest(std::span<const std::uint8_t>(datagram.data(), static_cast<std::size_t>(received)));
#endif
}

}  // namespace aes67
