/*
* Audacity: A Digital Audio Editor
*/

#include "playbackjitterlogger.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>

namespace au::playback {
namespace {
const char* eventName(uint32_t t)
{
    switch (static_cast<PlaybackJitterLogger::EventType>(t)) {
    case PlaybackJitterLogger::EventType::PositionUpdate:     return "POSITION_UPDATE";
    case PlaybackJitterLogger::EventType::StateRunning:       return "STATE_RUNNING";
    case PlaybackJitterLogger::EventType::StatePaused:        return "STATE_PAUSED";
    case PlaybackJitterLogger::EventType::StateStopped:       return "STATE_STOPPED";
    case PlaybackJitterLogger::EventType::TimerStopInactive:  return "TIMER_STOP_INACTIVE";
    case PlaybackJitterLogger::EventType::TimerStopRecording: return "TIMER_STOP_RECORDING";
    }
    return "UNKNOWN";
}
}

PlaybackJitterLogger& PlaybackJitterLogger::instance()
{
    static PlaybackJitterLogger s;
    return s;
}

PlaybackJitterLogger::PlaybackJitterLogger()
{
    m_buffer.resize(kCapacity);
    m_steadyStartNs = nowSteadyNs();
    const auto sys = std::chrono::system_clock::now().time_since_epoch();
    m_systemStartEpochNs = std::chrono::duration_cast<std::chrono::nanoseconds>(sys).count();
}

int64_t PlaybackJitterLogger::nowSteadyNs()
{
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

void PlaybackJitterLogger::logTick(int64_t dacNs, uint64_t consumedSamples)
{
    LogEntry& slot = m_buffer[m_writeIdx & kMask];
    slot.wallNs = nowSteadyNs();
    slot.dacNs = dacNs;
    slot.consumedSamples = consumedSamples;
    slot.eventType = static_cast<uint32_t>(EventType::PositionUpdate);
    slot.pad = 0;
    ++m_writeIdx;
}

void PlaybackJitterLogger::logEvent(EventType type)
{
    LogEntry& slot = m_buffer[m_writeIdx & kMask];
    slot.wallNs = nowSteadyNs();
    slot.dacNs = kDacSentinel;
    slot.consumedSamples = 0;
    slot.eventType = static_cast<uint32_t>(type);
    slot.pad = 0;
    ++m_writeIdx;
}

void PlaybackJitterLogger::setSampleRate(double hz)
{
    m_sampleRate = hz;
}

muse::Ret PlaybackJitterLogger::flush(const muse::io::path_t& outPath) const
{
    std::ofstream f(outPath.toStdString());
    if (!f) {
        return muse::make_ret(muse::Ret::Code::InternalError);
    }

    const bool overflowed = m_writeIdx > kCapacity;
    const size_t count    = std::min(m_writeIdx, kCapacity);
    const size_t startIdx = overflowed ? (m_writeIdx - kCapacity) : 0;

    char isoBuf[64] = { 0 };
    const std::time_t t = static_cast<std::time_t>(m_systemStartEpochNs / 1000000000LL);
    std::tm tmBuf {};
#if defined(_WIN32)
    gmtime_s(&tmBuf, &t);
#else
    gmtime_r(&t, &tmBuf);
#endif
    std::strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%SZ", &tmBuf);

    f << "# playback_jitter v1\n";
    f << "# steady_start_ns=" << m_steadyStartNs
      << "  system_start_iso=" << isoBuf
      << "  sample_rate=" << m_sampleRate
      << "  capacity=" << kCapacity
      << "  overflowed=" << (overflowed ? 1 : 0)
      << "  total_entries=" << m_writeIdx
      << "\n";
    f << "# wall_ns,dac_ns,event_type,consumed_samples\n";

    for (size_t i = 0; i < count; ++i) {
        const size_t logicalIdx = startIdx + i;
        const LogEntry& e = m_buffer[logicalIdx & kMask];
        f << e.wallNs << ",";
        if (e.dacNs == kDacSentinel) {
            f << ",";
        } else {
            f << e.dacNs << ",";
        }
        f << eventName(e.eventType) << ",";
        f << e.consumedSamples << "\n";
    }

    return muse::make_ok();
}
} // namespace au::playback
