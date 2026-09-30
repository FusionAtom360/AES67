#pragma once

#include <cstdint>

namespace aes67 {

// linuxptp disciplines CLOCK_REALTIME through ptp4l and phc2sys.
class PtpClock {
public:
    std::uint64_t now_ns() const;
    void sleep_until_ns(std::uint64_t deadline_ns) const;
};

}  // namespace aes67
