#pragma once

#include "aes67/receive_path.hpp"

#include <cstdint>
#include <string>

namespace aes67 {

class UdpReceiver {
public:
    UdpReceiver(std::uint16_t port, std::string bind_address = "0.0.0.0");
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // Block until one UDP datagram arrives, then parse and enqueue it.
    bool receive_once(ReceivePath& path);

private:
    int socket_ = -1;
};

}  // namespace aes67
