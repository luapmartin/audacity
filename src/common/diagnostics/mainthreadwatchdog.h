/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// Main-thread stall watchdog. Runs on a dedicated std::thread; the main
// thread is expected to periodically call `beat()` (via a short-interval
// QTimer). When the watchdog observes a heartbeat staleness above the
// configured threshold, it emits an `AU_SP_EVENT("mainStallDetected")`
// signpost (visible in Instruments) and spawns `sample <pid> 1 -file ...`
// to capture the stack of every thread for later inspection.
//
// Overhead when idle: one atomic read per check interval on the background
// thread. No impact on the main thread except the QTimer firing the beat.

namespace au::diag {
class MainThreadWatchdog
{
public:
    static MainThreadWatchdog& instance();

    // Start watching. `stallThreshold` = min time without a beat before
    // the watchdog fires. `checkInterval` = how often the background thread
    // wakes to check. `sampleOutDir` = directory where sample(1) dumps go.
    // Safe to call multiple times — subsequent calls are no-ops until stop.
    void start(std::chrono::milliseconds stallThreshold, std::chrono::milliseconds checkInterval, const std::string& sampleOutDir);

    void stop();

    // Called from the main thread (typically from a QTimer) to record a
    // heartbeat. Fast: single relaxed atomic store.
    void beat();

private:
    MainThreadWatchdog() = default;
    ~MainThreadWatchdog();

    MainThreadWatchdog(const MainThreadWatchdog&) = delete;
    MainThreadWatchdog& operator=(const MainThreadWatchdog&) = delete;

    void run();
    void captureStackSample(int64_t stallNs);

    std::atomic<int64_t> m_lastBeatNs{ 0 };
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_inStall{ false };
    std::atomic<int> m_stallSequence{ 0 };

    std::thread m_thread;
    std::chrono::nanoseconds m_stallThresholdNs{ std::chrono::milliseconds{ 500 } };
    std::chrono::milliseconds m_checkInterval{ 100 };
    std::string m_sampleOutDir;
    int m_pid = 0;
};
} // namespace au::diag
