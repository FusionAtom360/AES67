#include "aes67/alsa_source.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

#ifdef AES67_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace aes67 {

struct AlsaSource::Impl {
#ifdef AES67_HAVE_ALSA
    snd_pcm_t* handle = nullptr;
#endif
};

AlsaSource::AlsaSource(std::string device) : impl_(new Impl{}) {
#ifdef AES67_HAVE_ALSA
    const auto error = snd_pcm_open(&impl_->handle, device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (error < 0) {
        delete impl_;
        impl_ = nullptr;
        throw std::runtime_error("could not open ALSA capture device: " + std::string(snd_strerror(error)));
    }
    const auto params_error = snd_pcm_set_params(
        impl_->handle, SND_PCM_FORMAT_S24_3LE, SND_PCM_ACCESS_RW_INTERLEAVED,
        2, AudioFormat::sample_rate, 1, 1'000);
    if (params_error < 0) {
        snd_pcm_close(impl_->handle);
        delete impl_;
        impl_ = nullptr;
        throw std::runtime_error("could not configure ALSA capture: " + std::string(snd_strerror(params_error)));
    }
#else
    (void)device;
    delete impl_;
    impl_ = nullptr;
    throw std::runtime_error("ALSA support is not available in this build");
#endif
}

AlsaSource::~AlsaSource() {
#ifdef AES67_HAVE_ALSA
    if (impl_ != nullptr) snd_pcm_close(impl_->handle);
#endif
    delete impl_;
}

AudioPacket AlsaSource::read_packet(std::uint32_t rtp_timestamp) {
#ifdef AES67_HAVE_ALSA
    constexpr std::size_t frames_per_packet = AudioFormat::samples_per_packet;
    constexpr std::size_t channels = 2;
    std::vector<std::uint8_t> pcm(frames_per_packet * channels * 3);
    std::size_t frames_read = 0;
    std::size_t recovery_attempts = 0;
    while (frames_read < frames_per_packet) {
        auto* data = pcm.data() + frames_read * channels * 3;
        const auto frames = snd_pcm_readi(impl_->handle, data, frames_per_packet - frames_read);
        if (frames < 0) {
            if (++recovery_attempts > 3) {
                throw std::runtime_error("ALSA capture recovery failed: " +
                                         std::string(snd_strerror(static_cast<int>(frames))));
            }
            const auto recovered = snd_pcm_recover(impl_->handle, static_cast<int>(frames), 1);
            if (recovered < 0) {
                throw std::runtime_error("ALSA capture failed: " +
                                         std::string(snd_strerror(recovered)));
            }
            continue;
        }
        if (frames == 0) {
            throw std::runtime_error("ALSA capture returned zero frames");
        }
        recovery_attempts = 0;
        frames_read += static_cast<std::size_t>(frames);
    }

    AudioPacket packet{AudioFormat{2}, rtp_timestamp, {}};
    packet.samples.reserve(frames_per_packet * channels);
    for (std::size_t offset = 0; offset < pcm.size(); offset += 3) {
        auto sample = static_cast<std::int32_t>(pcm[offset]) |
                      (static_cast<std::int32_t>(pcm[offset + 1]) << 8) |
                      (static_cast<std::int32_t>(pcm[offset + 2]) << 16);
        if ((sample & 0x0080'0000) != 0) sample |= static_cast<std::int32_t>(0xff00'0000U);
        packet.samples.push_back(sample);
    }
    return packet;
#else
    (void)rtp_timestamp;
    throw std::runtime_error("ALSA support is not available in this build");
#endif
}

}  // namespace aes67
