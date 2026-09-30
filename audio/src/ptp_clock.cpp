#include "aes67/ptp_clock.hpp"

#include <stdexcept>

#ifdef _WIN32
#else
#include <cerrno>
#include <ctime>
#endif

namespace aes67 {

std::uint64_t PtpClock::now_ns() const {
#ifdef _WIN32
    throw std::runtime_error("PTP clock access requires Linux and linuxptp");
#else
    timespec time{};
    if (::clock_gettime(CLOCK_REALTIME, &time) != 0) {
        throw std::runtime_error("could not read PTP-disciplined CLOCK_REALTIME");
    }
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(time.tv_nsec);
#endif
}

void PtpClock::sleep_until_ns(std::uint64_t deadline_ns) const {
#ifdef _WIN32
    (void)deadline_ns;
    throw std::runtime_error("PTP clock access requires Linux and linuxptp");
#else
    timespec deadline{};
    deadline.tv_sec = static_cast<time_t>(deadline_ns / 1'000'000'000ULL);
    deadline.tv_nsec = static_cast<long>(deadline_ns % 1'000'000'000ULL);
    for (;;) {
        const auto result = ::clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &deadline, nullptr);
        if (result == 0) return;
        if (result != EINTR) throw std::runtime_error("could not sleep on PTP clock");
    }
#endif
}

}  // namespace aes67
