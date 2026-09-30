#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aes67 {

struct SdpSession {
    std::string session_name = "AES67 session";
    std::string origin_username = "-";
    std::string origin_session_id = "1";
    std::string origin_version = "1";
    std::string connection_address = "239.69.0.1";
    std::uint16_t media_port = 5004;
    std::string media_protocol = "RTP/AVP";
    std::uint8_t payload_type = 96;
    std::string encoding_name = "L24";
    std::uint32_t clock_rate = 48000;
    std::uint8_t channels = 2;
    bool ptp_reference_clock = false;
    bool direct_media_clock = false;

    static SdpSession parse(std::string_view text);
    std::string generate() const;
};

}  // namespace aes67
