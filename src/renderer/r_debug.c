/*
 * QUICKEN Renderer - Debug Support
 *
 * Validation layers, debug messenger, GPU timestamp queries.
 */

#include "r_types.h"
#include <string.h>

/* ---- Debug Labels ---- */

#ifdef QUICKEN_DEBUG
static PFN_vkCmdBeginDebugUtilsLabelEXT s_begin_label = NULL;
static PFN_vkCmdEndDebugUtilsLabelEXT   s_end_label   = NULL;
#endif

static void r_debug_cache_procs(void)
{
#ifdef QUICKEN_DEBUG
    s_begin_label = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetDeviceProcAddr(
        g_r.device.handle, "vkCmdBeginDebugUtilsLabelEXT");
    s_end_label = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetDeviceProcAddr(
        g_r.device.handle, "vkCmdEndDebugUtilsLabelEXT");
#endif
}

void r_debug_begin_label(VkCommandBuffer cmd, const char *name, float cr, float cg, float cb)
{
#ifdef QUICKEN_DEBUG
    if (s_begin_label) {
        VkDebugUtilsLabelEXT label = {
            .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
            .pLabelName = name,
            .color      = { cr, cg, cb, 1.0f }
        };
        s_begin_label(cmd, &label);
    }
#else
    QK_UNUSED(cmd); QK_UNUSED(name);
    QK_UNUSED(cr); QK_UNUSED(cg); QK_UNUSED(cb);
#endif
}

void r_debug_end_label(VkCommandBuffer cmd)
{
#ifdef QUICKEN_DEBUG
    if (s_end_label) {
        s_end_label(cmd);
    }
#else
    QK_UNUSED(cmd);
#endif
}

/* ---- GPU Timers ---- */

void r_debug_timers_init(void)
{
    if (g_r.device.properties.limits.timestampComputeAndGraphics == VK_FALSE) {
        return;
    }

    VkQueryPoolCreateInfo pool_info = {
        .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType  = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = R_TIMESTAMP_COUNT
    };

    vkCreateQueryPool(g_r.device.handle, &pool_info, NULL, &g_r.gpu_timers.pool);
}

void r_debug_timers_shutdown(void)
{
    if (g_r.gpu_timers.pool) {
        vkDestroyQueryPool(g_r.device.handle, g_r.gpu_timers.pool, NULL);
        g_r.gpu_timers.pool = VK_NULL_HANDLE;
    }
}

void r_debug_timestamp_write(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, u32 query)
{
    if (g_r.gpu_timers.pool && query < R_TIMESTAMP_COUNT) {
        vkCmdWriteTimestamp(cmd, stage, g_r.gpu_timers.pool, query);
    }
}

void r_debug_timers_read(void)
{
    if (!g_r.gpu_timers.pool) return;

    VkResult vr = vkGetQueryPoolResults(
        g_r.device.handle, g_r.gpu_timers.pool,
        0, R_TIMESTAMP_COUNT,
        sizeof(g_r.gpu_timers.results), g_r.gpu_timers.results,
        sizeof(u64), VK_QUERY_RESULT_64_BIT);

    if (vr != VK_SUCCESS && vr != VK_NOT_READY) return;
    if (vr == VK_NOT_READY) return;

    f64 period = (f64)g_r.device.properties.limits.timestampPeriod; /* nanoseconds */

    /* Timestamps: 0=frame start, 1=world end, 2=ui end, 3=compose end */
    if (g_r.gpu_timers.results[0] > 0 && g_r.gpu_timers.results[3] > 0) {
        g_r.gpu_timers.gpu_frame_ms = (f64)(g_r.gpu_timers.results[3] - g_r.gpu_timers.results[0])
                                    * period / 1000000.0;
    }
    if (g_r.gpu_timers.results[0] > 0 && g_r.gpu_timers.results[1] > 0) {
        g_r.gpu_timers.world_pass_ms = (f64)(g_r.gpu_timers.results[1] - g_r.gpu_timers.results[0])
                                     * period / 1000000.0;
    }
    if (g_r.gpu_timers.results[1] > 0 && g_r.gpu_timers.results[2] > 0) {
        g_r.gpu_timers.ui_pass_ms = (f64)(g_r.gpu_timers.results[2] - g_r.gpu_timers.results[1])
                                   * period / 1000000.0;
    }
    if (g_r.gpu_timers.results[2] > 0 && g_r.gpu_timers.results[3] > 0) {
        g_r.gpu_timers.compose_pass_ms = (f64)(g_r.gpu_timers.results[3] - g_r.gpu_timers.results[2])
                                        * period / 1000000.0;
    }
}

qk_result_t r_debug_init(void)
{
    r_debug_cache_procs();
    r_debug_timers_init();
    return QK_SUCCESS;
}

void r_debug_shutdown(void)
{
    r_debug_timers_shutdown();
}
