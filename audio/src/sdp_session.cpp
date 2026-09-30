#include "aes67/sdp_session.hpp"

#include <charconv>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace aes67 {
namespace {

std::uint32_t parse_number(std::string_view value, const char* field, std::uint32_t maximum) {
    std::uint32_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result > maximum) {
        throw std::invalid_argument(std::string("invalid SDP ") + field);
    }
    return result;
}

std::vector<std::string_view> split(std::string_view value, char delimiter) {
    std::vector<std::string_view> parts;
    while (true) {
        const auto position = value.find(delimiter);
        parts.push_back(value.substr(0, position));
        if (position == std::string_view::npos) return parts;
        value.remove_prefix(position + 1);
    }
}

}  // namespace

SdpSession SdpSession::parse(std::string_view text) {
    SdpSession session;
    bool has_version = false;
    bool has_origin = false;
    bool has_name = false;
    bool has_connection = false;
    bool has_media = false;
    bool has_format = false;

    while (!text.empty()) {
        const auto newline = text.find('\n');
        auto line = text.substr(0, newline);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() < 2 || line[1] != '=') throw std::invalid_argument("invalid SDP line");
        const auto value = line.substr(2);
        switch (line[0]) {
        case 'v':
            if (value != "0") throw std::invalid_argument("unsupported SDP version");
            has_version = true;
            break;
        case 'o': {
            const auto fields = split(value, ' ');
            if (fields.size() != 6) throw std::invalid_argument("invalid SDP origin");
            session.origin_username = std::string(fields[0]);
            session.origin_session_id = std::string(fields[1]);
            session.origin_version = std::string(fields[2]);
            has_origin = true;
            break;
        }
        case 's':
            session.session_name = std::string(value);
            has_name = true;
            break;
        case 'c': {
            const auto fields = split(value, ' ');
            if (fields.size() != 3 || fields[0] != "IN" || fields[1] != "IP4") {
                throw std::invalid_argument("invalid SDP connection");
            }
            session.connection_address = std::string(fields[2]);
            has_connection = true;
            break;
        }
        case 'm': {
            const auto fields = split(value, ' ');
            if (fields.size() != 4 || fields[0] != "audio") throw std::invalid_argument("invalid SDP audio media");
            session.media_port = static_cast<std::uint16_t>(parse_number(fields[1], "media port", 65535));
            session.media_protocol = std::string(fields[2]);
            session.payload_type = static_cast<std::uint8_t>(parse_number(fields[3], "payload type", 127));
            has_media = true;
            break;
        }
        case 'a':
            if (value.starts_with("rtpmap:")) {
                const auto fields = split(value.substr(7), ' ');
                if (fields.size() != 2) throw std::invalid_argument("invalid SDP rtpmap");
                const auto codec = split(fields[1], '/');
                if (parse_number(fields[0], "rtpmap payload type", 127) != session.payload_type || codec.size() < 2) {
                    throw std::invalid_argument("SDP rtpmap does not match media");
                }
                session.encoding_name = std::string(codec[0]);
                session.clock_rate = parse_number(codec[1], "clock rate", 0xffffffffU);
                if (codec.size() == 3) session.channels = static_cast<std::uint8_t>(parse_number(codec[2], "channel count", 255));
                has_format = true;
            }
            break;
        default:
            break;
        }
        if (newline == std::string_view::npos) break;
        text.remove_prefix(newline + 1);
    }

    if (!has_version || !has_origin || !has_name || !has_connection || !has_media || !has_format) {
        throw std::invalid_argument("incomplete SDP session");
    }
    if (session.media_protocol != "RTP/AVP" || session.encoding_name != "L24" ||
        session.clock_rate != 48000 || session.channels == 0) {
        throw std::invalid_argument("SDP is not an AES67 L24/48000 stream");
    }
    return session;
}

std::string SdpSession::generate() const {
    std::ostringstream output;
    output << "v=0\r\n"
           << "o=" << origin_username << ' ' << origin_session_id << ' ' << origin_version << " IN IP4 0.0.0.0\r\n"
           << "s=" << session_name << "\r\n"
           << "c=IN IP4 " << connection_address << "\r\n"
           << "t=0 0\r\n"
           << "m=audio " << media_port << ' ' << media_protocol << ' ' << static_cast<unsigned>(payload_type) << "\r\n"
           << "a=rtpmap:" << static_cast<unsigned>(payload_type) << ' ' << encoding_name << '/' << clock_rate;
    if (channels != 1) output << '/' << static_cast<unsigned>(channels);
    output << "\r\n";
    return output.str();
}

}  // namespace aes67