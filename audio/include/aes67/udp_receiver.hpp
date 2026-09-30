#pragma once

#include "aes67/receive_path.hpp"

#include <cstdint>
#include <string>

namespace aes67 {

struct UdpReceiverConfig {
    std::uint16_t port = 5004;
    std::string bind_address = "0.0.0.0";
    std::string multicast_group;
    std::string multicast_interface = "0.0.0.0";
    bool mark_dscp_ef = true;
    std::uint32_t receive_timeout_ms = 1;
};

class UdpReceiver {
public:
    UdpReceiver(std::uint16_t port, std::string bind_address = "0.0.0.0");
    explicit UdpReceiver(UdpReceiverConfig config);
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // Block until one UDP datagram arrives, then parse and enqueue it.
    bool receive_once(ReceivePath& path);

private:
    int socket_ = -1;
};

}  // namespace aes67
