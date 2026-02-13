/*
 * QUICKEN Engine - Clock Synchronization
 *
 * Ping-pong clock sync protocol. The client sends probes with its local time,
 * the server echoes them back with the server time. The client computes RTT
 * and clock offset, maintaining a rolling average with outlier rejection.
 */

#include "netcode/n_internal.h"
#include <stdlib.h>

void n_clock_init(n_clock_state_t *clk) {
    memset(clk, 0, sizeof(*clk));
}

/* Compare for qsort */
static int cmp_f64(const void *a, const void *b) {
    f64 va = *(const f64 *)a;
    f64 vb = *(const f64 *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

void n_clock_add_sample(n_clock_state_t *clk, f64 rtt, f64 offset) {
    u32 idx = clk->sample_index % N_CLOCK_SYNC_SAMPLES;
    clk->rtt_samples[idx] = rtt;
    clk->offset_samples[idx] = offset;
    clk->sample_index++;
    if (clk->sample_count < N_CLOCK_SYNC_SAMPLES) {
        clk->sample_count++;
    }

    if (clk->sample_count >= N_CLOCK_CONVERGE_COUNT) {
        clk->converged = true;
    }

    /* Compute filtered offset and RTT:
     * Sort RTT samples, find median, discard outliers (>2x median),
     * average remaining offset samples. */
    u32 n = clk->sample_count;
    f64 sorted_rtt[N_CLOCK_SYNC_SAMPLES];
    f64 corresponding_offset[N_CLOCK_SYNC_SAMPLES];

    /* Gather valid samples */
    for (u32 i = 0; i < n; i++) {
        u32 si = (clk->sample_index - n + i) % N_CLOCK_SYNC_SAMPLES;
        sorted_rtt[i] = clk->rtt_samples[si];
        corresponding_offset[i] = clk->offset_samples[si];
    }

    /* Find median RTT */
    f64 rtt_copy[N_CLOCK_SYNC_SAMPLES];
    memcpy(rtt_copy, sorted_rtt, n * sizeof(f64));
    qsort(rtt_copy, n, sizeof(f64), cmp_f64);
    f64 median_rtt = rtt_copy[n / 2];

    /* Average offset and RTT, excluding outliers */
    f64 sum_offset = 0.0;
    f64 sum_rtt = 0.0;
    u32 count = 0;
    f64 rtt_threshold = median_rtt * 2.0;
    if (rtt_threshold < 0.001) rtt_threshold = 0.001; /* min 1ms threshold */

    for (u32 i = 0; i < n; i++) {
        if (sorted_rtt[i] <= rtt_threshold) {
            sum_offset += corresponding_offset[i];
            sum_rtt += sorted_rtt[i];
            count++;
        }
    }

    if (count > 0) {
        clk->smoothed_offset = sum_offset / (f64)count;
        clk->smoothed_rtt = sum_rtt / (f64)count;
    }
}

bool n_clock_should_sync(const n_clock_state_t *clk, f64 now) {
    if (clk->last_sync_time <= 0.0) return true;

    f64 interval = clk->converged ? N_CLOCK_SYNC_INTERVAL : N_CLOCK_SYNC_FAST;
    return (now - clk->last_sync_time) >= interval;
}

void n_clock_mark_sent(n_clock_state_t *clk, f64 now) {
    clk->last_sync_time = now;
}
