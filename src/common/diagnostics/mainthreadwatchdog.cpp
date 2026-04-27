/*
* Audacity: A Digital Audio Editor
*/

#include "mainthreadwatchdog.h"

#include "ausignposts.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#if defined(__APPLE__) || defined(__unix__)
#include <unistd.h>
#else
#include <process.h>
#endif

namespace au::diag {
namespace {
int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

MainThreadWatchdog& MainThreadWatchdog::instance()
{
    static MainThreadWatchdog s;
    return s;
}

MainThreadWatchdog::~MainThreadWatchdog()
{
    stop();
}

void MainThreadWatchdog::start(std::chrono::milliseconds stallThreshold,
                               std::chrono::milliseconds checkInterval,
                               const std::string& sampleOutDir)
{
    if (m_running.load()) {
        return;
    }

    m_stallThresholdNs = stallThreshold;
    m_checkInterval = checkInterval;
    m_sampleOutDir = sampleOutDir;

#if defined(_WIN32)
    m_pid = _getpid();
#else
    m_pid = static_cast<int>(::getpid());
#endif

    std::error_code ec;
    std::filesystem::create_directories(m_sampleOutDir, ec);

    // Arm with a fresh beat so we don't fire during startup.
    beat();

    m_running.store(true);
    m_thread = std::thread([this]() { run(); });
}

void MainThreadWatchdog::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void MainThreadWatchdog::beat()
{
    m_lastBeatNs.store(nowNs(), std::memory_order_relaxed);
}

void MainThreadWatchdog::run()
{
    while (m_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(m_checkInterval);
        if (!m_running.load(std::memory_order_relaxed)) {
            break;
        }

        const int64_t lastBeat = m_lastBeatNs.load(std::memory_order_relaxed);
        const int64_t stalenessNs = nowNs() - lastBeat;

        if (stalenessNs > m_stallThresholdNs.count()) {
            if (!m_inStall.exchange(true)) {
                // First detection of this stall — mark it and snapshot.
                AU_SP_EVENT("mainStallDetected");
                ++m_stallSequence;
                captureStackSample(stalenessNs);
            }
        } else {
            m_inStall.store(false);
        }
    }
}

void MainThreadWatchdog::captureStackSample(int64_t stallNs)
{
#if defined(__APPLE__)
    // `sample` is a standard macOS tool that works on same-user processes
    // without sudo. It captures periodic backtraces of every thread over
    // the duration window and writes a human-readable report.
    std::time_t t = std::time(nullptr);
    std::tm tm {};
    localtime_r(&t, &tm);
    char timebuf[64] = { 0 };
    std::strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", &tm);

    const int stallMs = static_cast<int>(stallNs / 1000000);

    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
                  "sample %d 1 -file \"%s/freeze_%s_seq%d_%dms.txt\" >/dev/null 2>&1",
                  m_pid,
                  m_sampleOutDir.c_str(),
                  timebuf,
                  m_stallSequence.load(),
                  stallMs);
    (void)std::system(cmd);
#else
    (void)stallNs; // sample(1) is macOS-only; no-op elsewhere.
#endif
}
} // namespace au::diag
