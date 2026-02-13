/*
 * QUICKEN Renderer - Internal Types
 *
 * Shared across all r_*.c files in the renderer module.
 * NOT part of the public API.
 */

#ifndef R_TYPES_H
#define R_TYPES_H

#include "quicken.h"

#ifdef QK_PLATFORM_WINDOWS
    #define VK_USE_PLATFORM_WIN32_KHR
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(QK_PLATFORM_LINUX)
    #define VK_USE_PLATFORM_XCB_KHR
#endif

#include <vulkan/vulkan.h>
#include <string.h>

/* ---- Constants ---- */

#define R_MAX_SWAPCHAIN_IMAGES  4
#define R_FRAMES_IN_FLIGHT      2
#define R_MAX_TEXTURES          256
#define R_UI_MAX_QUADS          4096
#define R_TIMESTAMP_COUNT       8
#define R_ENTITY_MAX_DRAWS      512

#define R_MEMORY_POOL_DEVICE_LOCAL  0
#define R_MEMORY_POOL_HOST_VISIBLE  1
#define R_MEMORY_POOL_COUNT         2

#define R_DEVICE_LOCAL_POOL_SIZE    (64u * 1024u * 1024u)
#define R_HOST_VISIBLE_POOL_SIZE    (16u * 1024u * 1024u)
#define R_STAGING_BUFFER_SIZE       (32u * 1024u * 1024u)

/* ---- Instance ---- */

typedef struct r_instance {
    VkInstance                  handle;
    VkDebugUtilsMessengerEXT   debug_messenger;
} r_instance_t;

/* ---- Queue Families ---- */

typedef struct r_queue_families {
    u32     graphics;
    u32     transfer;
    bool    has_dedicated_transfer;
} r_queue_families_t;

/* ---- Device ---- */

typedef struct r_device {
    VkDevice                            handle;
    VkPhysicalDevice                    physical;
    VkQueue                             graphics_queue;
    VkQueue                             transfer_queue;
    r_queue_families_t                  families;
    VkPhysicalDeviceProperties          properties;
    VkPhysicalDeviceMemoryProperties    mem_properties;
} r_device_t;

/* ---- Swapchain ---- */

typedef struct r_swapchain {
    VkSwapchainKHR  handle;
    VkSurfaceKHR    surface;
    VkFormat        format;
    VkExtent2D      extent;
    u32             image_count;
    VkImage         images[R_MAX_SWAPCHAIN_IMAGES];
    VkImageView     views[R_MAX_SWAPCHAIN_IMAGES];
} r_swapchain_t;

/* ---- Render Target ---- */

typedef struct r_render_target {
    VkImage         color_image;
    VkDeviceMemory  color_memory;
    VkImageView     color_view;
    VkImage         depth_image;
    VkDeviceMemory  depth_memory;
    VkImageView     depth_view;
    VkFramebuffer   framebuffer;
    VkRenderPass    render_pass;
    VkExtent2D      extent;
} r_render_target_t;

/* ---- Render Config ---- */

typedef struct r_render_config {
    u32     render_width;
    u32     render_height;
    u32     window_width;
    u32     window_height;
    bool    aspect_fit;
    bool    vsync;
} r_render_config_t;

/* ---- Frame Data ---- */

typedef struct r_frame_data {
    VkCommandPool       command_pool;
    VkCommandBuffer     command_buffer;

    VkSemaphore         image_available;
    VkSemaphore         render_finished;
    VkFence             in_flight;

    VkBuffer            view_ubo;
    VkDeviceMemory      view_ubo_memory;
    void               *view_ubo_mapped;
    VkDescriptorSet     view_descriptor_set;

    VkBuffer            ui_vertex_buffer;
    VkDeviceMemory      ui_vertex_memory;
    void               *ui_vertex_mapped;
} r_frame_data_t;

/* ---- Pipeline ---- */

typedef struct r_pipeline {
    VkPipeline          handle;
    VkPipelineLayout    layout;
} r_pipeline_t;

/* ---- Memory Pool ---- */

typedef struct r_memory_pool {
    VkDeviceMemory  memory;
    VkDeviceSize    size;
    VkDeviceSize    offset;
    u32             memory_type;
    void           *mapped;
} r_memory_pool_t;

/* ---- Staging Buffer ---- */

typedef struct r_staging_buffer {
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    void           *mapped;
    VkDeviceSize    size;
    VkDeviceSize    offset;
} r_staging_buffer_t;

/* ---- World Geometry ---- */

typedef struct r_world_vertex {
    f32     position[3];
    f32     normal[3];
    f32     uv[2];
    u32     texture_id;
} r_world_vertex_t;

typedef struct r_draw_surface {
    u32     index_offset;
    u32     index_count;
    u32     vertex_offset;
    u32     texture_index;
} r_draw_surface_t;

typedef struct r_world_geometry {
    VkBuffer        vertex_buffer;
    VkDeviceMemory  vertex_memory;
    u32             vertex_count;

    VkBuffer        index_buffer;
    VkDeviceMemory  index_memory;
    u32             index_count;

    u32                 surface_count;
    r_draw_surface_t   *surfaces;
} r_world_geometry_t;

/* ---- UI Types ---- */

typedef struct r_ui_vertex {
    f32     position[2];
    f32     uv[2];
    u32     color;
} r_ui_vertex_t;

typedef struct r_ui_quad {
    f32     x, y, w, h;
    f32     u0, v0, u1, v1;
    u32     color;
    u32     texture_id;
} r_ui_quad_t;

/* ---- Texture ---- */

typedef struct r_texture {
    VkImage         image;
    VkImageView     view;
    VkDeviceMemory  memory;
    VkDescriptorSet descriptor_set;
    u32             width;
    u32             height;
    VkFormat        format;
    bool            in_use;
} r_texture_t;

typedef struct r_texture_manager {
    r_texture_t     textures[R_MAX_TEXTURES];
    VkSampler       sampler_nearest;
    VkSampler       sampler_linear;
    u32             next_free;
} r_texture_manager_t;

/* ---- View Uniforms ---- */

typedef struct r_view_uniforms {
    f32     view_projection[16];
    f32     camera_pos[3];
    f32     time;
} r_view_uniforms_t;

/* ---- Entity Types ---- */

typedef struct r_entity_vertex {
    f32     position[3];
    f32     normal[3];
} r_entity_vertex_t;

typedef struct r_entity_push_constants {
    f32     model[16];      /* column-major 4x4 */
    f32     color[4];       /* RGBA */
} r_entity_push_constants_t;

typedef enum {
    R_ENTITY_MESH_CAPSULE = 0,
    R_ENTITY_MESH_SPHERE,
    R_ENTITY_MESH_COUNT
} r_entity_mesh_type_t;

typedef struct r_entity_mesh {
    VkBuffer        vertex_buffer;
    VkDeviceMemory  vertex_memory;
    VkBuffer        index_buffer;
    VkDeviceMemory  index_memory;
    u32             index_count;
} r_entity_mesh_t;

typedef struct r_entity_draw {
    r_entity_push_constants_t push;
    r_entity_mesh_type_t      mesh_type;
} r_entity_draw_t;

typedef struct r_entity_state {
    r_entity_mesh_t     meshes[R_ENTITY_MESH_COUNT];
    r_entity_draw_t     draws[R_ENTITY_MAX_DRAWS];
    u32                 draw_count;
    bool                initialized;
} r_entity_state_t;

/* ---- Composition Push Constants ---- */

typedef struct r_compose_push_constants {
    f32     viewport[4];
    u32     mode;
} r_compose_push_constants_t;

/* ---- GPU Timers ---- */

typedef struct r_gpu_timers {
    VkQueryPool     pool;
    u64             results[R_TIMESTAMP_COUNT];
    f64             gpu_frame_ms;
    f64             world_pass_ms;
    f64             ui_pass_ms;
    f64             compose_pass_ms;
} r_gpu_timers_t;

/* ---- Global Renderer State ---- */

typedef struct r_state {
    r_instance_t            instance;
    r_device_t              device;
    r_swapchain_t           swapchain;
    r_render_config_t       config;

    r_render_target_t       world_target;
    r_render_target_t       ui_target;

    r_frame_data_t          frames[R_FRAMES_IN_FLIGHT];
    u32                     frame_index;
    u32                     current_image_index;

    VkPipelineCache         pipeline_cache_handle;
    r_pipeline_t            world_pipeline;
    r_pipeline_t            entity_pipeline;
    r_pipeline_t            ui_pipeline;
    r_pipeline_t            compose_pipeline;

    VkDescriptorSetLayout   view_set_layout;
    VkDescriptorSetLayout   texture_set_layout;
    VkDescriptorSetLayout   compose_set_layout;
    VkDescriptorPool        descriptor_pool;

    VkDescriptorSet         compose_descriptor_set;
    VkSampler               compose_sampler;

    /* Composition framebuffers (one per swapchain image) */
    VkRenderPass            compose_render_pass;
    VkFramebuffer           compose_framebuffers[R_MAX_SWAPCHAIN_IMAGES];

    r_world_geometry_t      world;

    r_entity_state_t        entities;

    r_texture_manager_t     textures;

    r_memory_pool_t         pools[R_MEMORY_POOL_COUNT];

    r_staging_buffer_t      staging;

    r_ui_quad_t             ui_quads[R_UI_MAX_QUADS];
    u32                     ui_quad_count;

    VkBuffer                ui_index_buffer;
    VkDeviceMemory          ui_index_memory;

    r_gpu_timers_t          gpu_timers;

    /* Stats for current frame */
    u32                     stats_draw_calls;
    u32                     stats_triangles;

    bool                    initialized;
    bool                    swapchain_needs_recreate;
} r_state_t;

/* ---- Global state accessor (defined in r_vulkan.c) ---- */
extern r_state_t g_r;

/* ---- Internal function declarations ---- */

/* r_vulkan.c */
qk_result_t r_vulkan_create_instance(void);
qk_result_t r_vulkan_pick_physical_device(void);
qk_result_t r_vulkan_create_device(void);
qk_result_t r_vulkan_create_surface(void *sdl_window);
qk_result_t r_vulkan_create_swapchain(void);
void        r_vulkan_destroy_swapchain(void);
qk_result_t r_vulkan_recreate_swapchain(void);

/* r_memory.c */
qk_result_t r_memory_init(void);
void        r_memory_shutdown(void);
bool        r_memory_find_type(u32 type_filter, VkMemoryPropertyFlags properties, u32 *out_type);
qk_result_t r_memory_pool_alloc(u32 pool_index, VkDeviceSize size, VkDeviceSize alignment,
                                VkDeviceSize *out_offset);
qk_result_t r_memory_create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                   VkMemoryPropertyFlags properties,
                                   VkBuffer *out_buffer, VkDeviceMemory *out_memory);
qk_result_t r_staging_init(void);
void        r_staging_shutdown(void);
void        r_staging_reset(void);
void       *r_staging_alloc(VkDeviceSize size, VkDeviceSize *out_offset);
void        r_staging_flush_copies(VkCommandBuffer cmd);

/* r_commands.c */
qk_result_t r_commands_init(void);
void        r_commands_shutdown(void);
VkCommandBuffer r_commands_begin_single(void);
void        r_commands_end_single(VkCommandBuffer cmd);

/* r_pipeline.c */
qk_result_t r_pipeline_cache_init(void);
void        r_pipeline_cache_save(void);
void        r_pipeline_cache_shutdown(void);
VkShaderModule r_pipeline_load_shader(const char *path);
qk_result_t r_pipeline_create_world(void);
qk_result_t r_pipeline_create_entity(void);
qk_result_t r_pipeline_create_ui(void);
qk_result_t r_pipeline_create_compose(void);
void        r_pipeline_destroy_all(void);

/* r_world.c */
void r_world_init(void);
void r_world_shutdown(void);
void r_world_record_commands(VkCommandBuffer cmd, u32 frame_index);

/* r_entity.c */
qk_result_t r_entity_init(void);
void        r_entity_shutdown(void);
void        r_entity_record_commands(VkCommandBuffer cmd, u32 frame_index);

/* r_ui.c */
qk_result_t r_ui_init(void);
void        r_ui_shutdown(void);
void        r_ui_record_commands(VkCommandBuffer cmd, u32 frame_index);

/* r_compose.c */
qk_result_t r_compose_init(void);
void        r_compose_shutdown(void);
void        r_compose_update_descriptors(void);
void        r_compose_record_commands(VkCommandBuffer cmd, u32 image_index);

/* r_texture.c */
qk_result_t r_texture_init(void);
void        r_texture_shutdown(void);
u32         r_texture_upload(const u8 *pixels, u32 width, u32 height, u32 channels);
VkDescriptorSet r_texture_get_descriptor(u32 texture_id);

/* r_debug.c */
qk_result_t r_debug_init(void);
void        r_debug_shutdown(void);
void        r_debug_begin_label(VkCommandBuffer cmd, const char *name, float r, float g, float b);
void        r_debug_end_label(VkCommandBuffer cmd);
void        r_debug_timers_init(void);
void        r_debug_timers_shutdown(void);
void        r_debug_timestamp_write(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, u32 query);
void        r_debug_timers_read(void);

/* Render target helpers */
qk_result_t r_create_render_targets(void);
void        r_destroy_render_targets(void);

/* Descriptor helpers */
qk_result_t r_descriptors_init(void);
void        r_descriptors_shutdown(void);

#endif /* R_TYPES_H */
