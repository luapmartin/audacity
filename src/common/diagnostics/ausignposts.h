/*
* Audacity: A Digital Audio Editor
*/
#pragma once

// Apple Instruments os_signpost helper — shared across modules.
// Compiled out on non-Apple platforms, and compiled out on Apple when
// AU_APPLE_SIGNPOSTS is not defined.
//
// Signposts live in the "PointsOfInterest" category so Instruments' default
// os_signpost lane (and the CPU Profiler template) auto-displays them.
// Near-zero-cost when Instruments is not recording.
//
// Usage:
//   #include "ausignposts.h"
//   void doStuff() {
//       AU_SP_SCOPE("doStuff");            // RAII interval, auto-ends at }
//       ...
//       AU_SP_EVENT("milestone");          // point event
//   }

#if defined(__APPLE__) && defined(AU_APPLE_SIGNPOSTS)

#include <os/log.h>
#include <os/signpost.h>

namespace au::diag {
inline os_log_t signpostLog()
{
    // function-local static inside an inline function: one process-wide
    // instance across all translation units.
    static os_log_t s_log = os_log_create("org.audacityteam.Audacity", "PointsOfInterest");
    return s_log;
}

class SignpostScope
{
public:
    explicit SignpostScope(const char* name)
        : m_id(os_signpost_id_generate(signpostLog())), m_name(name)
    {
        os_signpost_interval_begin(signpostLog(), m_id, "scope", "%{public}s", m_name);
    }

    ~SignpostScope()
    {
        os_signpost_interval_end(signpostLog(), m_id, "scope", "%{public}s", m_name);
    }

    SignpostScope(const SignpostScope&) = delete;
    SignpostScope& operator=(const SignpostScope&) = delete;

private:
    os_signpost_id_t m_id;
    const char* m_name;
};
} // namespace au::diag

#define AU_SP_CONCAT_INNER(a, b) a##b
#define AU_SP_CONCAT(a, b) AU_SP_CONCAT_INNER(a, b)

#define AU_SP_EVENT(name) \
    os_signpost_event_emit(::au::diag::signpostLog(), OS_SIGNPOST_ID_EXCLUSIVE, name)

#define AU_SP_SCOPE(name) \
    ::au::diag::SignpostScope AU_SP_CONCAT(auSpScope_, __LINE__)(name)

#else

#define AU_SP_EVENT(name) do {} while (0)
#define AU_SP_SCOPE(name) do {} while (0)

#endif
