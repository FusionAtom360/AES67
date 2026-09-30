#pragma once

#include "aes67/audio_packet.hpp"

#include <cstdint>
#include <string>

namespace aes67 {

struct RtpSenderConfig {
    std::string destination_address = "239.69.0.1";
    std::uint16_t port = 5004;
    std::string interface_address = "0.0.0.0";
    std::uint8_t payload_type = 96;
    bool mark_dscp_ef = true;
};

class RtpSender {
public:
    explicit RtpSender(RtpSenderConfig config);
    ~RtpSender();

    RtpSender(const RtpSender&) = delete;
    RtpSender& operator=(const RtpSender&) = delete;

    void send(const AudioPacket& packet, std::uint16_t sequence_number);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace aes67
