#include "aes67/alsa_sink.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

#ifdef AES67_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace aes67 {

struct AlsaSink::Impl {
#ifdef AES67_HAVE_ALSA
    snd_pcm_t* handle = nullptr;
#endif
};

AlsaSink::AlsaSink(std::string device) : impl_(new Impl{}) {
#ifdef AES67_HAVE_ALSA
    const auto error = snd_pcm_open(&impl_->handle, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (error < 0) {
        delete impl_;
        impl_ = nullptr;
        throw std::runtime_error("could not open ALSA playback device: " + std::string(snd_strerror(error)));
    }
    const auto params_error = snd_pcm_set_params(
        impl_->handle, SND_PCM_FORMAT_S24_3LE, SND_PCM_ACCESS_RW_INTERLEAVED,
        2, AudioFormat::sample_rate, 1, 1'000);
    if (params_error < 0) {
        snd_pcm_close(impl_->handle);
        delete impl_;
        impl_ = nullptr;
        throw std::runtime_error("could not configure ALSA playback: " + std::string(snd_strerror(params_error)));
    }
#else
    (void)device;
    delete impl_;
    impl_ = nullptr;
    throw std::runtime_error("ALSA support is not available in this build");
#endif
}

AlsaSink::~AlsaSink() {
#ifdef AES67_HAVE_ALSA
    if (impl_ != nullptr) {
        snd_pcm_drain(impl_->handle);
        snd_pcm_close(impl_->handle);
    }
#endif
    delete impl_;
}

void AlsaSink::write(const AudioPacket& packet) {
#ifdef AES67_HAVE_ALSA
    if (impl_ == nullptr || packet.format.channels != 2) {
        throw std::invalid_argument("ALSA sink requires a stereo packet");
    }
    std::vector<std::uint8_t> pcm;
    pcm.reserve(packet.samples.size() * 3);
    for (const auto sample : packet.samples) {
        if (sample < -(1 << 23) || sample > (1 << 23) - 1) {
            throw std::out_of_range("PCM sample is outside the signed 24-bit range");
        }
        const auto value = static_cast<std::uint32_t>(sample) & 0x00ff'ffffU;
        pcm.push_back(static_cast<std::uint8_t>(value & 0xff));
        pcm.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        pcm.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    }
    auto frames_remaining = static_cast<snd_pcm_sframes_t>(packet.samples.size() / 2);
    auto* data = pcm.data();
    while (frames_remaining > 0) {
        const auto frames_written = snd_pcm_writei(impl_->handle, data, frames_remaining);
        if (frames_written == -EPIPE) {
            snd_pcm_prepare(impl_->handle);
            continue;
        }
        if (frames_written < 0) {
            throw std::runtime_error("ALSA playback failed: " + std::string(snd_strerror(static_cast<int>(frames_written))));
        }
        frames_remaining -= frames_written;
        data += frames_written * 2 * 3;
    }
#else
    (void)packet;
    throw std::runtime_error("ALSA support is not available in this build");
#endif
}

}  // namespace aes67
