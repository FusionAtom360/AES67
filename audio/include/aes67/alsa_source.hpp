#pragma once

#include "aes67/audio_packet.hpp"

#include <string>

namespace aes67 {

class AlsaSource {
public:
    explicit AlsaSource(std::string device = "default");
    ~AlsaSource();

    AlsaSource(const AlsaSource&) = delete;
    AlsaSource& operator=(const AlsaSource&) = delete;

    AudioPacket read_packet(std::uint32_t rtp_timestamp);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace aes67
