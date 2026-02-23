/*
 * QUICKEN Engine - Compile-Time Opt-In Profiler
 *
 * Per-system timing zones, complexity counters, and load-time events.
 * Gated behind QK_PROFILE: when undefined, every macro is ((void)0).
 * Output is JSONL (quicken_prof.jsonl) for AI agent consumption.
 *
 * Usage:
 *   premake5 vs2022 --profile   (enables QK_PROFILE workspace-wide)
 *   premake5 vs2022             (no profiler, zero overhead)
 */

#ifndef QK_PROF_H
#define QK_PROF_H

#include "quicken.h"

#ifdef QK_PROFILE

void qk_prof_init(void);
void qk_prof_shutdown(void);
void qk_prof_frame_begin(void);
void qk_prof_frame_end(void);
void qk_prof_zone_begin(const char *name);
void qk_prof_zone_end(const char *name);
void qk_prof_counter_add(const char *name, u32 value);
void qk_prof_event_begin(const char *name);
void qk_prof_event_end(const char *name);

#define QK_PROF_INIT()              qk_prof_init()
#define QK_PROF_SHUTDOWN()          qk_prof_shutdown()
#define QK_PROF_FRAME_BEGIN()       qk_prof_frame_begin()
#define QK_PROF_FRAME_END()         qk_prof_frame_end()
#define QK_PROF_ZONE_BEGIN(name)    qk_prof_zone_begin(name)
#define QK_PROF_ZONE_END(name)      qk_prof_zone_end(name)
#define QK_PROF_COUNTER(name, val)  qk_prof_counter_add((name), (val))
#define QK_PROF_EVENT_BEGIN(name)   qk_prof_event_begin(name)
#define QK_PROF_EVENT_END(name)     qk_prof_event_end(name)

#else /* QK_PROFILE not defined */

#define QK_PROF_INIT()              ((void)0)
#define QK_PROF_SHUTDOWN()          ((void)0)
#define QK_PROF_FRAME_BEGIN()       ((void)0)
#define QK_PROF_FRAME_END()         ((void)0)
#define QK_PROF_ZONE_BEGIN(name)    ((void)0)
#define QK_PROF_ZONE_END(name)      ((void)0)
#define QK_PROF_COUNTER(name, val)  ((void)0)
#define QK_PROF_EVENT_BEGIN(name)   ((void)0)
#define QK_PROF_EVENT_END(name)     ((void)0)

#endif /* QK_PROFILE */

#endif /* QK_PROF_H */
