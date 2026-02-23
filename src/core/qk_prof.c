/*
 * QUICKEN Engine - Compile-Time Profiler Implementation
 *
 * Entire file is a no-op when QK_PROFILE is not defined.
 * All data structures are fixed-size (no malloc). Output is JSONL.
 *
 * Output strategy (keeps file small for AI agent consumption):
 *   - "stats" record every STATS_INTERVAL frames with [min, avg, max]
 *   - "spike" record (full frame breakdown) only when frame ms sets a new max
 *   - "event" records written immediately (map loads, etc.)
 *   - "summary" record at shutdown with lifetime stats
 */

#ifdef QK_PROFILE

#include "core/qk_prof.h"
#include "core/qk_platform.h"
#include <stdio.h>
#include <string.h>
#include <float.h>

#define PROF_MAX_ZONES    64
#define PROF_MAX_COUNTERS 64
#define PROF_MAX_EVENTS   16
#define STATS_INTERVAL    128   /* emit a stats record every N frames */

/* --- Per-frame transient data --- */

typedef struct {
    const char *name;
    f64         start_time;
    f64         elapsed_ms;     /* this frame */
    bool        active;
} prof_zone_t;

typedef struct {
    const char *name;
    u32         value;          /* this frame */
} prof_counter_t;

typedef struct {
    const char *name;
    f64         start_time;
    bool        active;
} prof_event_t;

/* --- Running statistics (per window and lifetime) --- */

typedef struct {
    f64 min;
    f64 max;
    f64 sum;
} stat_t;

static void stat_reset(stat_t *s) {
    s->min = DBL_MAX;
    s->max = 0.0;
    s->sum = 0.0;
}

static void stat_add(stat_t *s, f64 v) {
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    s->sum += v;
}

/* --- Global profiler state --- */

static struct {
    FILE           *file;
    f64             session_start;
    f64             frame_start;
    u32             frame_number;

    /* Per-frame transient */
    prof_zone_t     zones[PROF_MAX_ZONES];
    u32             zone_count;
    prof_counter_t  counters[PROF_MAX_COUNTERS];
    u32             counter_count;
    prof_event_t    events[PROF_MAX_EVENTS];
    u32             event_count;

    /* Window stats (reset every STATS_INTERVAL frames) */
    stat_t          win_frame;
    stat_t          win_zones[PROF_MAX_ZONES];
    stat_t          win_counters[PROF_MAX_COUNTERS];
    u32             win_frames;

    /* Lifetime stats (never reset) */
    stat_t          life_frame;
    stat_t          life_zones[PROF_MAX_ZONES];
    stat_t          life_counters[PROF_MAX_COUNTERS];

    /* Spike detection: all-time max frame ms */
    f64             spike_max_ms;
} s_prof;

/* --- Lookup helpers --- */

static i32 find_zone(const char *name) {
    for (u32 i = 0; i < s_prof.zone_count; i++) {
        if (s_prof.zones[i].name == name) return (i32)i;
    }
    for (u32 i = 0; i < s_prof.zone_count; i++) {
        if (strcmp(s_prof.zones[i].name, name) == 0) return (i32)i;
    }
    return -1;
}

static i32 find_or_add_zone(const char *name) {
    i32 idx = find_zone(name);
    if (idx >= 0) return idx;
    if (s_prof.zone_count >= PROF_MAX_ZONES) return -1;
    idx = (i32)s_prof.zone_count++;
    s_prof.zones[idx].name = name;
    s_prof.zones[idx].elapsed_ms = 0.0;
    s_prof.zones[idx].active = false;
    stat_reset(&s_prof.win_zones[idx]);
    stat_reset(&s_prof.life_zones[idx]);
    return idx;
}

static i32 find_counter(const char *name) {
    for (u32 i = 0; i < s_prof.counter_count; i++) {
        if (s_prof.counters[i].name == name) return (i32)i;
    }
    for (u32 i = 0; i < s_prof.counter_count; i++) {
        if (strcmp(s_prof.counters[i].name, name) == 0) return (i32)i;
    }
    return -1;
}

static i32 find_or_add_counter(const char *name) {
    i32 idx = find_counter(name);
    if (idx >= 0) return idx;
    if (s_prof.counter_count >= PROF_MAX_COUNTERS) return -1;
    idx = (i32)s_prof.counter_count++;
    s_prof.counters[idx].name = name;
    s_prof.counters[idx].value = 0;
    stat_reset(&s_prof.win_counters[idx]);
    stat_reset(&s_prof.life_counters[idx]);
    return idx;
}

static i32 find_event(const char *name) {
    for (u32 i = 0; i < s_prof.event_count; i++) {
        if (s_prof.events[i].name == name) return (i32)i;
    }
    for (u32 i = 0; i < s_prof.event_count; i++) {
        if (strcmp(s_prof.events[i].name, name) == 0) return (i32)i;
    }
    return -1;
}

static i32 find_or_add_event(const char *name) {
    i32 idx = find_event(name);
    if (idx >= 0) return idx;
    if (s_prof.event_count >= PROF_MAX_EVENTS) return -1;
    idx = (i32)s_prof.event_count++;
    s_prof.events[idx].name = name;
    s_prof.events[idx].active = false;
    return idx;
}

/* --- Write helpers --- */

/* Write zone data as JSON object (only non-zero entries) */
static void write_zones_json(FILE *f, const char *key,
                              const prof_zone_t *zones, u32 count) {
    bool any = false;
    for (u32 i = 0; i < count; i++) {
        if (zones[i].elapsed_ms > 0.0) { any = true; break; }
    }
    if (!any) return;

    fprintf(f, ",\"%s\":{", key);
    bool first = true;
    for (u32 i = 0; i < count; i++) {
        if (zones[i].elapsed_ms <= 0.0) continue;
        if (!first) fprintf(f, ",");
        fprintf(f, "\"%s\":%.3f", zones[i].name, zones[i].elapsed_ms);
        first = false;
    }
    fprintf(f, "}");
}

/* Write counter data as JSON object (only non-zero entries) */
static void write_counters_json(FILE *f, const char *key,
                                 const prof_counter_t *counters, u32 count) {
    bool any = false;
    for (u32 i = 0; i < count; i++) {
        if (counters[i].value > 0) { any = true; break; }
    }
    if (!any) return;

    fprintf(f, ",\"%s\":{", key);
    bool first = true;
    for (u32 i = 0; i < count; i++) {
        if (counters[i].value == 0) continue;
        if (!first) fprintf(f, ",");
        fprintf(f, "\"%s\":%u", counters[i].name, counters[i].value);
        first = false;
    }
    fprintf(f, "}");
}

/* Write a stat_t triple as [min, avg, max] */
static void write_stat_triple(FILE *f, const stat_t *s, u32 n) {
    f64 avg = (n > 0) ? s->sum / (f64)n : 0.0;
    f64 mn  = (s->min < DBL_MAX) ? s->min : 0.0;
    fprintf(f, "[%.3f,%.3f,%.3f]", mn, avg, s->max);
}

/* Write stats record for current window */
static void write_stats_record(void) {
    FILE *f = s_prof.file;
    u32 n = s_prof.win_frames;
    f64 t = qk_platform_time_now() - s_prof.session_start;

    fprintf(f, "{\"type\":\"stats\",\"n\":%u,\"t\":%.3f,\"frames\":%u,\"ms\":",
            s_prof.frame_number, t, n);
    write_stat_triple(f, &s_prof.win_frame, n);

    /* Zone stats */
    if (s_prof.zone_count > 0) {
        fprintf(f, ",\"zones\":{");
        bool first = true;
        for (u32 i = 0; i < s_prof.zone_count; i++) {
            if (s_prof.win_zones[i].max <= 0.0) continue;
            if (!first) fprintf(f, ",");
            fprintf(f, "\"%s\":", s_prof.zones[i].name);
            write_stat_triple(f, &s_prof.win_zones[i], n);
            first = false;
        }
        fprintf(f, "}");
    }

    /* Counter stats */
    if (s_prof.counter_count > 0) {
        fprintf(f, ",\"counters\":{");
        bool first = true;
        for (u32 i = 0; i < s_prof.counter_count; i++) {
            if (s_prof.win_counters[i].max <= 0.0) continue;
            if (!first) fprintf(f, ",");
            fprintf(f, "\"%s\":", s_prof.counters[i].name);
            write_stat_triple(f, &s_prof.win_counters[i], n);
            first = false;
        }
        fprintf(f, "}");
    }

    fprintf(f, "}\n");
    fflush(f);
}

/* --- Public API --- */

void qk_prof_init(void) {
    memset(&s_prof, 0, sizeof(s_prof));

    s_prof.file = fopen("quicken_prof.jsonl", "w");
    if (!s_prof.file) return;

    s_prof.session_start = qk_platform_time_now();
    s_prof.spike_max_ms = 0.0;

    stat_reset(&s_prof.win_frame);
    stat_reset(&s_prof.life_frame);

    fprintf(s_prof.file,
        "{\"type\":\"session\",\"engine\":\"QUICKEN\",\"version\":\"%d.%d.%d\","
        "\"build\":\"%s\",\"tick_rate\":%d,\"platform\":\"%s\","
        "\"stats_interval\":%d}\n",
        QUICKEN_VERSION_MAJOR, QUICKEN_VERSION_MINOR, QUICKEN_VERSION_PATCH,
#ifdef QUICKEN_DEBUG
        "debug",
#else
        "release",
#endif
        128,
#ifdef QK_PLATFORM_WINDOWS
        "windows",
#else
        "linux",
#endif
        STATS_INTERVAL
    );
    fflush(s_prof.file);
}

void qk_prof_shutdown(void) {
    if (!s_prof.file) return;

    /* Flush any remaining window */
    if (s_prof.win_frames > 0) {
        write_stats_record();
    }

    u32 n = s_prof.frame_number;
    f64 duration = qk_platform_time_now() - s_prof.session_start;
    f64 avg_fps = (n > 0 && duration > 0.0) ? (f64)n / duration : 0.0;

    fprintf(s_prof.file,
        "{\"type\":\"summary\",\"total_frames\":%u,\"duration_s\":%.1f,"
        "\"avg_fps\":%.1f,\"ms\":",
        n, duration, avg_fps);
    write_stat_triple(s_prof.file, &s_prof.life_frame, n);

    /* Zone lifetime stats */
    if (s_prof.zone_count > 0) {
        fprintf(s_prof.file, ",\"zones\":{");
        bool first = true;
        for (u32 i = 0; i < s_prof.zone_count; i++) {
            if (!first) fprintf(s_prof.file, ",");
            fprintf(s_prof.file, "\"%s\":", s_prof.zones[i].name);
            write_stat_triple(s_prof.file, &s_prof.life_zones[i], n);
            first = false;
        }
        fprintf(s_prof.file, "}");
    }

    /* Counter lifetime stats */
    if (s_prof.counter_count > 0) {
        fprintf(s_prof.file, ",\"counters\":{");
        bool first = true;
        for (u32 i = 0; i < s_prof.counter_count; i++) {
            if (!first) fprintf(s_prof.file, ",");
            fprintf(s_prof.file, "\"%s\":", s_prof.counters[i].name);
            write_stat_triple(s_prof.file, &s_prof.life_counters[i], n);
            first = false;
        }
        fprintf(s_prof.file, "}");
    }

    fprintf(s_prof.file, "}\n");
    fclose(s_prof.file);
    s_prof.file = NULL;
}

void qk_prof_frame_begin(void) {
    if (!s_prof.file) return;

    s_prof.frame_start = qk_platform_time_now();

    for (u32 i = 0; i < s_prof.zone_count; i++) {
        s_prof.zones[i].elapsed_ms = 0.0;
        s_prof.zones[i].active = false;
    }
    for (u32 i = 0; i < s_prof.counter_count; i++) {
        s_prof.counters[i].value = 0;
    }
}

void qk_prof_frame_end(void) {
    if (!s_prof.file) return;

    f64 now = qk_platform_time_now();
    f64 frame_ms = (now - s_prof.frame_start) * 1000.0;
    f64 t = now - s_prof.session_start;

    /* Accumulate into window and lifetime stats */
    stat_add(&s_prof.win_frame, frame_ms);
    stat_add(&s_prof.life_frame, frame_ms);

    for (u32 i = 0; i < s_prof.zone_count; i++) {
        f64 ms = s_prof.zones[i].elapsed_ms;
        stat_add(&s_prof.win_zones[i], ms);
        stat_add(&s_prof.life_zones[i], ms);
    }
    for (u32 i = 0; i < s_prof.counter_count; i++) {
        f64 v = (f64)s_prof.counters[i].value;
        stat_add(&s_prof.win_counters[i], v);
        stat_add(&s_prof.life_counters[i], v);
    }

    s_prof.win_frames++;

    /* Spike: write full frame breakdown when frame ms sets a new all-time max */
    if (frame_ms > s_prof.spike_max_ms) {
        s_prof.spike_max_ms = frame_ms;

        fprintf(s_prof.file,
            "{\"type\":\"spike\",\"n\":%u,\"t\":%.3f,\"ms\":%.3f",
            s_prof.frame_number, t, frame_ms);
        write_zones_json(s_prof.file, "zones",
                          s_prof.zones, s_prof.zone_count);
        write_counters_json(s_prof.file, "counters",
                             s_prof.counters, s_prof.counter_count);
        fprintf(s_prof.file, "}\n");
    }

    s_prof.frame_number++;

    /* Periodic stats record */
    if (s_prof.win_frames >= STATS_INTERVAL) {
        write_stats_record();

        /* Reset window */
        stat_reset(&s_prof.win_frame);
        for (u32 i = 0; i < s_prof.zone_count; i++) {
            stat_reset(&s_prof.win_zones[i]);
        }
        for (u32 i = 0; i < s_prof.counter_count; i++) {
            stat_reset(&s_prof.win_counters[i]);
        }
        s_prof.win_frames = 0;
    }
}

void qk_prof_zone_begin(const char *name) {
    if (!s_prof.file) return;

    i32 idx = find_or_add_zone(name);
    if (idx < 0) return;

    s_prof.zones[idx].start_time = qk_platform_time_now();
    s_prof.zones[idx].active = true;
}

void qk_prof_zone_end(const char *name) {
    if (!s_prof.file) return;

    i32 idx = find_zone(name);
    if (idx < 0 || !s_prof.zones[idx].active) return;

    f64 now = qk_platform_time_now();
    s_prof.zones[idx].elapsed_ms += (now - s_prof.zones[idx].start_time) * 1000.0;
    s_prof.zones[idx].active = false;
}

void qk_prof_counter_add(const char *name, u32 value) {
    if (!s_prof.file) return;

    i32 idx = find_or_add_counter(name);
    if (idx < 0) return;

    s_prof.counters[idx].value += value;
}

void qk_prof_event_begin(const char *name) {
    if (!s_prof.file) return;

    i32 idx = find_or_add_event(name);
    if (idx < 0) return;

    s_prof.events[idx].start_time = qk_platform_time_now();
    s_prof.events[idx].active = true;
}

void qk_prof_event_end(const char *name) {
    if (!s_prof.file) return;

    i32 idx = find_event(name);
    if (idx < 0 || !s_prof.events[idx].active) return;

    f64 now = qk_platform_time_now();
    f64 ms = (now - s_prof.events[idx].start_time) * 1000.0;
    f64 t = now - s_prof.session_start;

    fprintf(s_prof.file,
        "{\"type\":\"event\",\"name\":\"%s\",\"ms\":%.1f,\"t\":%.3f}\n",
        name, ms, t);
    fflush(s_prof.file);

    s_prof.events[idx].active = false;
}

#endif /* QK_PROFILE */

typedef int qk_prof_empty_tu_;
