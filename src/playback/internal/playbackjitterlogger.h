/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "framework/global/io/path.h"
#include "framework/global/types/ret.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace au::playback {
// Diagnostic in-memory ring-buffer logger for playback-timer jitter
// investigation. Captures each updatePlaybackPosition() tick and each
// transport state transition with a steady_clock timestamp; flushed as CSV
// at application shutdown.
//
// Single-threaded by design: all call sites (the 16 ms QTimer callback and
// the transport actions) run on the Qt main thread.
class PlaybackJitterLogger
{
public:
    enum class EventType : uint32_t {
        PositionUpdate     = 0,
        StateRunning       = 1,
        StatePaused        = 2,
        StateStopped       = 3,
        TimerStopInactive  = 4,
        TimerStopRecording = 5,
    };

    static PlaybackJitterLogger& instance();

    void logTick(int64_t dacNs, uint64_t consumedSamples);
    void logEvent(EventType type);
    void setSampleRate(double hz);

    muse::Ret flush(const muse::io::path_t& outPath) const;

    static constexpr int64_t kDacSentinel = INT64_MIN;

private:
    PlaybackJitterLogger();

    struct LogEntry {
        int64_t wallNs;
        int64_t dacNs;
        uint64_t consumedSamples;
        uint32_t eventType;
        uint32_t pad;
    };

    static constexpr size_t kCapacity = 32768;
    static constexpr size_t kMask     = kCapacity - 1;

    static int64_t nowSteadyNs();

    std::vector<LogEntry> m_buffer;
    size_t m_writeIdx = 0;
    int64_t m_steadyStartNs = 0;
    int64_t m_systemStartEpochNs = 0;
    double m_sampleRate = 0.0;
};
} // namespace au::playback

#ifdef AU_PLAYBACK_JITTER_LOG
    #define AU_JITTER_LOG_EVENT(e) \
    ::au::playback::PlaybackJitterLogger::instance().logEvent(e)
    #define AU_JITTER_LOG_TICK(dacNs, consumedSamples) \
    ::au::playback::PlaybackJitterLogger::instance().logTick((dacNs), (consumedSamples))
    #define AU_JITTER_LOG_SAMPLE_RATE(hz) \
    ::au::playback::PlaybackJitterLogger::instance().setSampleRate((hz))
#else
    #define AU_JITTER_LOG_EVENT(e)                      do {} while (0)
    #define AU_JITTER_LOG_TICK(dacNs, consumedSamples)  do {} while (0)
    #define AU_JITTER_LOG_SAMPLE_RATE(hz)               do {} while (0)
#endif
