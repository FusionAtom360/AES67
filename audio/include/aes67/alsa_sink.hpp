#pragma once

#include "aes67/audio_packet.hpp"

#include <string>

namespace aes67 {

class AlsaSink {
public:
    explicit AlsaSink(std::string device = "default");
    ~AlsaSink();

    AlsaSink(const AlsaSink&) = delete;
    AlsaSink& operator=(const AlsaSink&) = delete;

    void write(const AudioPacket& packet);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace aes67
