# QUICKEN Renderer -- Vertical Slice Implementation Plan

## Scope

Get a Quake map playable on screen through Vulkan. Two offscreen render targets (world, UI) composited to the swapchain. Configurable render resolution independent of window/display resolution. Mailbox present mode. Target sub-1ms frame times on high-end hardware with Quake-era geometry.

This document is an implementation plan, not a design overview. It specifies Vulkan objects, data layouts, pipeline configurations, and C struct definitions at a level of detail sufficient to write the code directly.

---

## 1. File Structure

```
src/renderer/
    r_vulkan.c          Vulkan instance, physical/logical device, swapchain, surface
    r_pipeline.c         Pipeline creation, shader module loading, pipeline cache
    r_memory.c           GPU memory allocator, staging buffer pool
    r_commands.c         Command pool/buffer management, frame command recording
    r_world.c            World geometry rendering (brush surfaces from .map data)
    r_ui.c               UI rendering (quads, glyphs, immediate-mode submission)
    r_compose.c          Composition pass: combine world + UI, present to swapchain
    r_texture.c          Texture upload, VkImage/VkImageView/VkSampler management
    r_debug.c            Validation layer setup, debug messenger, GPU timing queries

include/renderer/
    renderer.h           Public API (called by main loop)
    r_types.h            Internal renderer types shared across r_*.c files
```

---

## 2. Vulkan Initialization (`r_vulkan.c`)

### 2.1 Instance Creation

- Vulkan 1.2 minimum. Query `vkEnumerateInstanceVersion` and require >= 1.2.
- Instance extensions:
  - `VK_KHR_surface`
  - Platform surface extension (`VK_KHR_win32_surface` or `VK_KHR_xcb_surface` / `VK_KHR_wayland_surface`)
  - `VK_EXT_debug_utils` (debug builds only)
- Validation layers: `VK_LAYER_KHRONOS_validation` in debug builds only.
- Use SDL3's `SDL_Vulkan_GetInstanceExtensions` to get required surface extensions, then append our own.

```c
typedef struct r_instance {
    VkInstance           handle;
    VkDebugUtilsMessengerEXT debug_messenger;  /* VK_NULL_HANDLE in release */
} r_instance_t;
```

### 2.2 Physical Device Selection

Score devices. Requirements (hard fail if missing):
- Vulkan 1.2 support
- `VK_KHR_swapchain` extension
- A queue family with both graphics and present capability (prefer unified)
- A queue family with transfer capability (can be same family)
- Discrete GPU preferred. Integrated GPU acceptable as fallback.

Prefer:
- Discrete GPU over integrated
- Higher `maxImageDimension2D`
- Device-local heap size (more VRAM = better)

Store selected queue family indices:

```c
typedef struct r_queue_families {
    u32 graphics;       /* graphics + present (must support both) */
    u32 transfer;       /* dedicated transfer queue, or fallback to graphics */
    bool has_dedicated_transfer;
} r_queue_families_t;
```

### 2.3 Logical Device and Queues

Create logical device with:
- 1 graphics queue (priority 1.0)
- 1 transfer queue (priority 0.5) -- same family index is fine if no dedicated transfer family
- Device extensions: `VK_KHR_swapchain`
- Enable Vulkan 1.2 features via `VkPhysicalDeviceVulkan12Features`:
  - `timelineSemaphore` = true (for frame synchronization)
  - `descriptorIndexing` = true (for bindless textures, future)
- Enable Vulkan 1.2 features via `VkPhysicalDeviceVulkan11Features`:
  - `shaderDrawParameters` = true (for gl_DrawID in indirect draws, future)

```c
typedef struct r_device {
    VkDevice             handle;
    VkPhysicalDevice     physical;
    VkQueue              graphics_queue;
    VkQueue              transfer_queue;
    r_queue_families_t   families;
    VkPhysicalDeviceProperties       properties;
    VkPhysicalDeviceMemoryProperties mem_properties;
} r_device_t;
```

### 2.4 Surface and Swapchain

Surface created via `SDL_Vulkan_CreateSurface`.

Swapchain configuration:
- **Format**: Prefer `VK_FORMAT_B8G8R8A8_SRGB` with `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`. Fall back to `VK_FORMAT_B8G8R8A8_UNORM`.
- **Present mode**: `VK_PRESENT_MODE_MAILBOX_KHR` (lowest latency without tearing). Fall back to `VK_PRESENT_MODE_FIFO_KHR`.
- **Image count**: `minImageCount + 1`, capped by `maxImageCount` (if nonzero). Typically 3.
- **Image usage**: `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT` (we blit the composed image to the swapchain).
- **Composite alpha**: `VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR`.
- **Pre-transform**: `currentTransform` (no rotation).
- **Extent**: Matches window size. This is the display resolution, NOT the render resolution.

```c
#define R_MAX_SWAPCHAIN_IMAGES 4

typedef struct r_swapchain {
    VkSwapchainKHR       handle;
    VkSurfaceKHR         surface;
    VkFormat             format;
    VkExtent2D           extent;         /* window/display resolution */
    u32                  image_count;
    VkImage              images[R_MAX_SWAPCHAIN_IMAGES];
    VkImageView          views[R_MAX_SWAPCHAIN_IMAGES];
} r_swapchain_t;
```

Swapchain recreation on `VK_ERROR_OUT_OF_DATE_KHR` or `VK_SUBOPTIMAL_KHR`. Triggered by window resize. On recreation, `vkDeviceWaitIdle`, destroy old swapchain, create new one, recreate composition pipeline framebuffers if swapchain extent changed.

---

## 3. Offscreen Render Targets

Two offscreen textures at user-configurable render resolution, independent of window size.

### 3.1 Configuration

```c
typedef struct r_render_config {
    u32 render_width;       /* e.g. 1920 -- world + UI render resolution width */
    u32 render_height;      /* e.g. 1080 -- world + UI render resolution height */
    u32 window_width;       /* actual window/display width */
    u32 window_height;      /* actual window/display height */
    bool aspect_fit;        /* true = letterbox/pillarbox; false = stretch */
} r_render_config_t;
```

### 3.2 World Render Target

- **Color attachment**: `VK_FORMAT_R8G8B8A8_SRGB`, `render_width x render_height`.
  - Usage: `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`
  - Tiling: `VK_IMAGE_TILING_OPTIMAL`
  - Memory: `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`
- **Depth attachment**: `VK_FORMAT_D32_SFLOAT` (prefer) or `VK_FORMAT_D24_UNORM_S8_UINT` (fallback).
  - Usage: `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`
  - Memory: `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`

```c
typedef struct r_render_target {
    VkImage          color_image;
    VkDeviceMemory   color_memory;
    VkImageView      color_view;
    VkImage          depth_image;        /* VK_NULL_HANDLE for targets that don't need depth */
    VkDeviceMemory   depth_memory;
    VkImageView      depth_view;
    VkFramebuffer    framebuffer;
    VkRenderPass     render_pass;
    VkExtent2D       extent;
} r_render_target_t;
```

### 3.3 UI Render Target

- **Color attachment**: `VK_FORMAT_R8G8B8A8_SRGB`, `render_width x render_height`.
  - Usage: `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`
  - Memory: `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`
- **No depth attachment.** UI is rendered back-to-front with painter's algorithm. No depth testing needed.
- Clear to `(0, 0, 0, 0)` each frame -- fully transparent where no UI is drawn. The composition pass alpha-blends UI over the world.

### 3.4 Render Passes

**World render pass**:
- Attachment 0: Color (SRGB). Load op = `CLEAR`, Store op = `STORE`. Initial layout = `UNDEFINED`, Final layout = `SHADER_READ_ONLY_OPTIMAL`.
- Attachment 1: Depth. Load op = `CLEAR`, Store op = `DONT_CARE`. Initial layout = `UNDEFINED`, Final layout = `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`.
- One subpass: color attachment 0 as color output, attachment 1 as depth-stencil.
- No subpass dependencies needed (we use explicit pipeline barriers between passes).

**UI render pass**:
- Attachment 0: Color (SRGB). Load op = `CLEAR`, Store op = `STORE`. Initial layout = `UNDEFINED`, Final layout = `SHADER_READ_ONLY_OPTIMAL`.
- One subpass: color attachment 0 as color output.
- No depth.

**Composition render pass** (renders to swapchain):
- Attachment 0: Swapchain color. Load op = `CLEAR`, Store op = `STORE`. Initial layout = `UNDEFINED`, Final layout = `PRESENT_SRC_KHR`.
- One subpass: color attachment 0 as color output.

### 3.5 Resolution Change

When the user changes render resolution at runtime:
1. `vkDeviceWaitIdle`
2. Destroy world and UI render targets (images, views, memory, framebuffers).
3. Recreate at new resolution.
4. Update descriptor sets that reference these images (composition pass samplers).
5. No swapchain recreation needed -- swapchain stays at window resolution.

---

## 4. Composition Pass (`r_compose.c`)

The composition pass draws a full-screen quad (or triangle) that samples the world and UI offscreen textures and writes to the swapchain image.

### 4.1 Fullscreen Triangle

Use a single triangle with vertices generated in the vertex shader (no vertex buffer):

```glsl
// compose.vert
#version 450

layout(location = 0) out vec2 uv;

void main() {
    // Fullscreen triangle: 3 vertices, no vertex buffer
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    uv.y = 1.0 - uv.y;  // Flip Y for Vulkan coordinate system
}
```

```glsl
// compose.frag
#version 450

layout(set = 0, binding = 0) uniform sampler2D world_texture;
layout(set = 0, binding = 1) uniform sampler2D ui_texture;

layout(push_constant) uniform ComposeParams {
    vec4 viewport;      // x_offset, y_offset, width, height (normalized 0-1)
    uint mode;          // 0 = stretch, 1 = aspect fit
} params;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
    // Map screen UV to render target UV based on viewport rect
    vec2 render_uv = (uv - params.viewport.xy) / params.viewport.zw;

    // Outside the viewport rect: black (letterbox/pillarbox)
    if (render_uv.x < 0.0 || render_uv.x > 1.0 ||
        render_uv.y < 0.0 || render_uv.y > 1.0) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 world = texture(world_texture, render_uv);
    vec4 ui = texture(ui_texture, render_uv);

    // Alpha blend UI over world
    out_color = vec4(mix(world.rgb, ui.rgb, ui.a), 1.0);
}
```

### 4.2 Aspect Ratio Handling

Compute the viewport rectangle as push constants:

```c
typedef struct r_compose_push_constants {
    f32 viewport[4];    /* x_offset, y_offset, width, height -- all normalized 0..1 */
    u32 mode;           /* 0 = stretch, 1 = aspect fit */
} r_compose_push_constants_t;

static r_compose_push_constants_t compute_viewport(
    u32 render_w, u32 render_h,
    u32 window_w, u32 window_h,
    bool aspect_fit)
{
    r_compose_push_constants_t pc = {0};
    pc.mode = aspect_fit ? 1 : 0;

    if (!aspect_fit) {
        /* Stretch: full window */
        pc.viewport[0] = 0.0f;
        pc.viewport[1] = 0.0f;
        pc.viewport[2] = 1.0f;
        pc.viewport[3] = 1.0f;
    } else {
        f32 render_aspect = (f32)render_w / (f32)render_h;
        f32 window_aspect = (f32)window_w / (f32)window_h;

        if (window_aspect > render_aspect) {
            /* Window is wider: pillarbox (black bars on sides) */
            f32 scale = render_aspect / window_aspect;
            pc.viewport[0] = (1.0f - scale) * 0.5f;
            pc.viewport[1] = 0.0f;
            pc.viewport[2] = scale;
            pc.viewport[3] = 1.0f;
        } else {
            /* Window is taller: letterbox (black bars top/bottom) */
            f32 scale = window_aspect / render_aspect;
            pc.viewport[0] = 0.0f;
            pc.viewport[1] = (1.0f - scale) * 0.5f;
            pc.viewport[2] = 1.0f;
            pc.viewport[3] = scale;
        }
    }
    return pc;
}
```

### 4.3 Composition Pipeline

- **Vertex input**: No vertex input (fullscreen triangle generated in shader).
- **Input assembly**: `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`.
- **Rasterization**: `VK_CULL_MODE_NONE`, `VK_POLYGON_MODE_FILL`.
- **Depth stencil**: Disabled.
- **Color blend**: No blending (opaque write).
- **Dynamic state**: `VK_DYNAMIC_STATE_VIEWPORT`, `VK_DYNAMIC_STATE_SCISSOR`.
- **Descriptor set layout**: Set 0 with 2 combined image samplers (world texture, UI texture).
- **Push constant range**: `sizeof(r_compose_push_constants_t)`, fragment stage.
- **Samplers**: `VK_FILTER_LINEAR`, `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE`. Use `VK_FILTER_NEAREST` as a user option for pixel-sharp rendering at non-native resolutions.

---

## 5. World Rendering Pipeline (`r_world.c`)

### 5.1 Vertex Format

The engine core parses .map files and produces brush geometry. The renderer receives pre-processed vertex/index buffers in this format:

```c
typedef struct r_world_vertex {
    f32 position[3];    /* world-space XYZ */
    f32 normal[3];      /* surface normal (for lighting) */
    f32 uv[2];          /* texture coordinates */
    u32 texture_id;     /* index into texture array (for bindless, future) */
                        /* for vertical slice: unused, use per-draw texture binding */
} r_world_vertex_t;
/* Total: 36 bytes. Consider padding to 40 for alignment. */
```

Vertex input binding:

```c
VkVertexInputBindingDescription world_binding = {
    .binding   = 0,
    .stride    = sizeof(r_world_vertex_t),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
};

VkVertexInputAttributeDescription world_attrs[] = {
    { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = offsetof(r_world_vertex_t, position) },
    { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = offsetof(r_world_vertex_t, normal) },
    { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = offsetof(r_world_vertex_t, uv) },
    { .location = 3, .binding = 0, .format = VK_FORMAT_R32_UINT,            .offset = offsetof(r_world_vertex_t, texture_id) },
};
```

### 5.2 Index Format

`VK_INDEX_TYPE_UINT32`. Quake maps can exceed 65536 vertices per batch.

### 5.3 GPU Buffers for World Geometry

One large vertex buffer and one large index buffer for the entire map, uploaded once at map load via the transfer queue. No per-frame uploads for static world geometry.

```c
typedef struct r_world_geometry {
    VkBuffer        vertex_buffer;
    VkDeviceMemory  vertex_memory;
    u32             vertex_count;

    VkBuffer        index_buffer;
    VkDeviceMemory  index_memory;
    u32             index_count;

    /* Per-surface draw info for batched rendering */
    u32             surface_count;
    r_draw_surface_t *surfaces;     /* array of surface descriptors */
} r_world_geometry_t;

typedef struct r_draw_surface {
    u32 index_offset;       /* offset into index buffer (first index) */
    u32 index_count;        /* number of indices for this surface */
    u32 vertex_offset;      /* added to each index value (base vertex) */
    u32 texture_index;      /* which texture to bind for this surface */
} r_draw_surface_t;
```

### 5.4 World Shader

```glsl
// world.vert
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uint in_texture_id;

layout(set = 0, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
    vec3 camera_pos;
    float time;
} view;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out vec3 frag_world_pos;

void main() {
    gl_Position = view.view_projection * vec4(in_position, 1.0);
    frag_normal = in_normal;
    frag_uv = in_uv;
    frag_world_pos = in_position;
}
```

```glsl
// world.frag
#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in vec3 frag_world_pos;

layout(set = 0, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
    vec3 camera_pos;
    float time;
} view;

layout(set = 1, binding = 0) uniform sampler2D surface_texture;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 tex_color = texture(surface_texture, frag_uv).rgb;

    // Simple directional light + ambient for vertical slice
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
    float ndotl = max(dot(normalize(frag_normal), light_dir), 0.0);
    float ambient = 0.2;
    float diffuse = ndotl * 0.8;

    out_color = vec4(tex_color * (ambient + diffuse), 1.0);
}
```

### 5.5 World Pipeline Configuration

- **Vertex input**: As described in 5.1.
- **Input assembly**: `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`.
- **Rasterization**: `VK_CULL_MODE_BACK_BIT`, `VK_FRONT_FACE_COUNTER_CLOCKWISE`, `VK_POLYGON_MODE_FILL`.
- **Depth stencil**: Depth test ON, depth write ON, `VK_COMPARE_OP_LESS`.
- **Color blend**: No blending (opaque geometry only for vertical slice).
- **Multisampling**: `VK_SAMPLE_COUNT_1_BIT` (no MSAA for vertical slice).
- **Dynamic state**: `VK_DYNAMIC_STATE_VIEWPORT`, `VK_DYNAMIC_STATE_SCISSOR`.
- **Descriptor set layouts**:
  - Set 0: View uniforms (UBO, vertex + fragment stages)
  - Set 1: Surface texture (combined image sampler, fragment stage)
- **Push constants**: None for vertical slice. Reserved for future per-draw data.

### 5.6 View Uniform Buffer

```c
typedef struct r_view_uniforms {
    f32 view_projection[16];    /* column-major 4x4 */
    f32 camera_pos[3];
    f32 time;
} r_view_uniforms_t;
```

One UBO per frame-in-flight, host-visible + coherent, mapped persistently. Updated each frame before world render pass begins.

### 5.7 Draw Strategy (Vertical Slice)

For the vertical slice, draw world geometry with simple batching:

1. Sort surfaces by texture to minimize descriptor set rebinds.
2. For each unique texture, bind descriptor set 1 with the texture, issue `vkCmdDrawIndexed` for all surfaces using that texture.
3. Future: merge into multi-draw-indirect calls with bindless textures.

Target: a Quake map with ~5000 surfaces should need fewer than 100 draw calls after texture sorting (most surfaces share textures from a small set of WAD textures).

---

## 6. UI Rendering Pipeline (`r_ui.c`)

### 6.1 Design

Immediate-mode quad renderer. Each frame, the game pushes UI draw commands (quads with texture/color/UV). The renderer batches them into a single vertex buffer upload and draws them in submission order (back-to-front via painter's algorithm).

### 6.2 UI Vertex Format

```c
typedef struct r_ui_vertex {
    f32 position[2];    /* screen-space XY in pixels (0,0 = top-left) */
    f32 uv[2];          /* texture coordinates */
    u32 color;          /* packed RGBA8 (ABGR byte order for little-endian) */
} r_ui_vertex_t;
/* 20 bytes per vertex */
```

### 6.3 UI Draw Command Interface

```c
/* Submitted by game code each frame */
typedef struct r_ui_quad {
    f32 x, y, w, h;    /* screen position and size in render-resolution pixels */
    f32 u0, v0, u1, v1;/* texture UV rect */
    u32 color;          /* packed RGBA8 */
    u32 texture_id;     /* 0 = white pixel (solid color), otherwise texture handle */
} r_ui_quad_t;

#define R_UI_MAX_QUADS 4096
```

Each quad expands to 6 vertices (2 triangles) or 4 vertices + 6 indices. Use indexed drawing with a pre-built index buffer pattern (quad 0 = indices 0,1,2,2,3,0; quad 1 = 4,5,6,6,7,4; ...).

### 6.4 UI Shader

```glsl
// ui.vert
#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_color;

layout(push_constant) uniform UIParams {
    vec2 screen_size;   // render resolution for NDC conversion
} params;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;

void main() {
    // Convert pixel coordinates to NDC
    vec2 ndc = (in_position / params.screen_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    frag_uv = in_uv;

    // Unpack RGBA8 color
    frag_color = vec4(
        float((in_color >>  0) & 0xFF) / 255.0,
        float((in_color >>  8) & 0xFF) / 255.0,
        float((in_color >> 16) & 0xFF) / 255.0,
        float((in_color >> 24) & 0xFF) / 255.0
    );
}
```

```glsl
// ui.frag
#version 450

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D ui_texture;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex = texture(ui_texture, frag_uv);
    out_color = tex * frag_color;
}
```

### 6.5 UI Pipeline Configuration

- **Vertex input**: position (R32G32_SFLOAT), uv (R32G32_SFLOAT), color (R32_UINT).
- **Input assembly**: `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`.
- **Rasterization**: `VK_CULL_MODE_NONE`, `VK_POLYGON_MODE_FILL`.
- **Depth stencil**: Disabled.
- **Color blend**: Enabled. Pre-multiplied alpha:
  - `srcColorBlendFactor = VK_BLEND_FACTOR_ONE`
  - `dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA`
  - `srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE`
  - `dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA`
- **Dynamic state**: `VK_DYNAMIC_STATE_VIEWPORT`, `VK_DYNAMIC_STATE_SCISSOR`.
- **Descriptor set layout**: Set 0 with 1 combined image sampler.
- **Push constants**: `vec2 screen_size`, vertex stage.

### 6.6 UI Vertex Buffer Strategy

One host-visible, coherent vertex buffer per frame-in-flight. Sized for `R_UI_MAX_QUADS * 4 * sizeof(r_ui_vertex_t)` = 4096 * 4 * 20 = 320 KB. Persistently mapped. Each frame, write vertices into the current frame's buffer; no staging needed.

One pre-built device-local index buffer containing the repeating quad pattern (0,1,2,2,3,0,4,5,6,...) for `R_UI_MAX_QUADS` quads. Uploaded once at init.

### 6.7 UI Draw Strategy

1. Game code calls `r_ui_push_quad()` during its update phase, which appends to a CPU-side array.
2. At render time, sort quads by texture_id to minimize rebinds. Quads with the same texture_id are drawn in submission order (stable sort preserves painter's algorithm).
3. Write all vertices to the current frame's vertex buffer.
4. For each batch of quads sharing a texture, bind the texture and issue one `vkCmdDrawIndexed`.

For text rendering in the vertical slice: use a bitmap font atlas (a single texture with pre-rasterized glyphs). Each character is one `r_ui_quad_t`. No FreeType dependency.

---

## 7. Resource Management (`r_memory.c`, `r_texture.c`)

### 7.1 Memory Allocation

For the vertical slice, use a simple sub-allocator per memory type. No VMA dependency.

```c
typedef struct r_memory_pool {
    VkDeviceMemory  memory;
    VkDeviceSize    size;
    VkDeviceSize    offset;         /* next free offset (bump allocator) */
    u32             memory_type;
    void           *mapped;         /* non-NULL for host-visible memory */
} r_memory_pool_t;

#define R_MEMORY_POOL_DEVICE_LOCAL   0   /* GPU-only, for textures and static buffers */
#define R_MEMORY_POOL_HOST_VISIBLE   1   /* CPU-writable, for uniform/staging buffers */
#define R_MEMORY_POOL_COUNT          2
```

Allocate large blocks (e.g. 64 MB device-local, 16 MB host-visible) and sub-allocate from them with a bump allocator. Alignment handled by rounding `offset` up to `minUniformBufferOffsetAlignment` or `bufferImageGranularity` as appropriate.

This is intentionally simple. Replace with a proper allocator (free list, buddy) once we need dynamic geometry or texture streaming.

### 7.2 Staging Buffer

For uploading textures and static geometry to device-local memory:

```c
typedef struct r_staging_buffer {
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    void           *mapped;
    VkDeviceSize    size;
    VkDeviceSize    offset;         /* current write position */
} r_staging_buffer_t;
```

- Size: 32 MB (sufficient for a full Quake map's textures + geometry in one upload).
- Use the transfer queue with a dedicated command buffer.
- Fence-synchronized: submit transfer commands, wait on fence before the staged data is used.
- Reset offset to 0 between map loads.

Upload flow:
1. Map load: core engine produces vertex/index data and texture pixels.
2. Renderer copies data into staging buffer.
3. Record `vkCmdCopyBuffer` / `vkCmdCopyBufferToImage` commands.
4. Submit on transfer queue with a fence.
5. Wait on fence.
6. Static buffers are now in device-local memory, ready for rendering.

### 7.3 Texture Management

```c
#define R_MAX_TEXTURES 256

typedef struct r_texture {
    VkImage         image;
    VkImageView     view;
    VkDeviceMemory  memory;         /* from device-local pool */
    u32             width;
    u32             height;
    VkFormat        format;
    bool            in_use;
} r_texture_t;

typedef struct r_texture_manager {
    r_texture_t     textures[R_MAX_TEXTURES];
    VkSampler       sampler_nearest;        /* point sampling (retro look) */
    VkSampler       sampler_linear;         /* bilinear sampling */
    VkSampler       sampler_linear_mip;     /* trilinear sampling */
    u32             next_free;
} r_texture_manager_t;
```

Texture upload: data staged into staging buffer, then `vkCmdCopyBufferToImage`, then transition layout from `TRANSFER_DST_OPTIMAL` to `SHADER_READ_ONLY_OPTIMAL` via a pipeline barrier.

For the vertical slice, generate mipmaps with `vkCmdBlitImage` in a chain (each level from the previous). This is simple and sufficient for Quake's low-resolution textures.

A 1x1 white pixel texture is created at init for use as the default UI texture (solid-color quads).

---

## 8. Descriptor Sets

### 8.1 Descriptor Pool

```c
VkDescriptorPoolSize pool_sizes[] = {
    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         16 },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  R_MAX_TEXTURES + 16 },
};
```

One pool, created at init, sized for all descriptors needed during a frame. `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` is NOT set (we do not free individual sets -- reset the whole pool between map loads if needed).

### 8.2 Descriptor Set Layouts

**World view set (set 0)**:
- Binding 0: `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`, 1 count, `VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT`.

**World texture set (set 1)**:
- Binding 0: `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, 1 count, `VK_SHADER_STAGE_FRAGMENT_BIT`.
- One descriptor set allocated per unique texture. Descriptor sets for textures are allocated at map load and never freed until map unload.

**UI texture set (set 0)**:
- Binding 0: `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, 1 count, `VK_SHADER_STAGE_FRAGMENT_BIT`.
- Same layout as world texture set 1, but used in a different pipeline.

**Composition set (set 0)**:
- Binding 0: `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, 1 count, `VK_SHADER_STAGE_FRAGMENT_BIT` (world texture).
- Binding 1: `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, 1 count, `VK_SHADER_STAGE_FRAGMENT_BIT` (UI texture).
- One descriptor set, updated when render targets are recreated (resolution change).

---

## 9. Frame Loop and Synchronization (`r_commands.c`)

### 9.1 Frames in Flight

Two frames in flight. This is the minimum that allows the CPU to record frame N+1 while the GPU executes frame N. More frames = more latency, which is unacceptable for a competitive FPS.

```c
#define R_FRAMES_IN_FLIGHT 2

typedef struct r_frame_data {
    VkCommandPool    command_pool;
    VkCommandBuffer  command_buffer;

    /* Sync primitives */
    VkSemaphore      image_available;     /* signaled when swapchain image acquired */
    VkSemaphore      render_finished;     /* signaled when rendering complete */
    VkFence          in_flight;           /* CPU waits on this before reusing frame data */

    /* Per-frame resources */
    VkBuffer         view_ubo;            /* view uniforms for this frame */
    void            *view_ubo_mapped;     /* persistently mapped */
    VkDescriptorSet  view_descriptor_set;

    VkBuffer         ui_vertex_buffer;    /* UI vertices for this frame */
    void            *ui_vertex_mapped;    /* persistently mapped */
} r_frame_data_t;
```

### 9.2 Frame Loop Pseudocode

```
frame_loop:
    current_frame = frame_index % R_FRAMES_IN_FLIGHT
    frame = &frames[current_frame]

    // 1. Wait for this frame's previous GPU work to complete
    vkWaitForFences(device, 1, &frame->in_flight, VK_TRUE, UINT64_MAX)
    vkResetFences(device, 1, &frame->in_flight)

    // 2. Acquire swapchain image
    result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                   frame->image_available, VK_NULL_HANDLE, &image_index)
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); goto frame_loop; }

    // 3. Update per-frame data
    update_view_uniforms(frame, camera)
    write_ui_vertices(frame, ui_quads, ui_quad_count)

    // 4. Record command buffer
    vkResetCommandPool(device, frame->command_pool, 0)
    vkBeginCommandBuffer(frame->command_buffer, &begin_info)

    // 4a. World render pass
    //     Transition world color image: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (handled by render pass)
    vkCmdBeginRenderPass(cmd, &world_pass_info, VK_SUBPASS_CONTENTS_INLINE)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline)
        vkCmdSetViewport(cmd, 0, 1, &render_viewport)
        vkCmdSetScissor(cmd, 0, 1, &render_scissor)
        vkCmdBindDescriptorSets(cmd, ..., view_descriptor_set)
        vkCmdBindVertexBuffers(cmd, 0, 1, &world_vbo, &offset)
        vkCmdBindIndexBuffer(cmd, world_ibo, 0, VK_INDEX_TYPE_UINT32)
        for each texture batch:
            vkCmdBindDescriptorSets(cmd, ..., texture_descriptor_set)
            vkCmdDrawIndexed(cmd, index_count, 1, first_index, vertex_offset, 0)
    vkCmdEndRenderPass(cmd)
    //     World color image now in SHADER_READ_ONLY_OPTIMAL (by render pass final layout)

    // 4b. UI render pass
    vkCmdBeginRenderPass(cmd, &ui_pass_info, VK_SUBPASS_CONTENTS_INLINE)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline)
        vkCmdSetViewport(cmd, 0, 1, &render_viewport)
        vkCmdSetScissor(cmd, 0, 1, &render_scissor)
        vkCmdBindVertexBuffers(cmd, 0, 1, &frame->ui_vertex_buffer, &offset)
        vkCmdBindIndexBuffer(cmd, ui_index_buffer, 0, VK_INDEX_TYPE_UINT16)
        for each texture batch:
            vkCmdBindDescriptorSets(cmd, ..., ui_texture_descriptor)
            vkCmdDrawIndexed(cmd, index_count, 1, first_index, 0, 0)
    vkCmdEndRenderPass(cmd)
    //     UI color image now in SHADER_READ_ONLY_OPTIMAL

    // 4c. Composition pass (renders to swapchain image)
    vkCmdBeginRenderPass(cmd, &compose_pass_info, VK_SUBPASS_CONTENTS_INLINE)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_pipeline)
        vkCmdSetViewport(cmd, 0, 1, &swapchain_viewport)
        vkCmdSetScissor(cmd, 0, 1, &swapchain_scissor)
        vkCmdBindDescriptorSets(cmd, ..., compose_descriptor_set)
        vkCmdPushConstants(cmd, compose_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(r_compose_push_constants_t), &compose_pc)
        vkCmdDraw(cmd, 3, 1, 0, 0)   // fullscreen triangle
    vkCmdEndRenderPass(cmd)
    //     Swapchain image now in PRESENT_SRC_KHR

    vkEndCommandBuffer(cmd)

    // 5. Submit
    VkSubmitInfo submit = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &frame->image_available,
        .pWaitDstStageMask = &(VkPipelineStageFlags){VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
        .commandBufferCount = 1,
        .pCommandBuffers = &frame->command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &frame->render_finished,
    };
    vkQueueSubmit(graphics_queue, 1, &submit, frame->in_flight)

    // 6. Present
    VkPresentInfoKHR present = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &frame->render_finished,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };
    result = vkQueuePresentKHR(graphics_queue, &present)
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    }

    frame_index++
```

### 9.3 Synchronization Notes

- **Why binary semaphores (not timeline) for frame sync**: Timeline semaphores are superior for complex dependency graphs, but the vertical slice has a simple linear pipeline. Binary semaphores are simpler to implement and debug. Switch to timeline semaphores when adding async compute or multi-queue overlap.
- **Why `vkResetCommandPool` instead of `vkResetCommandBuffer`**: Resetting the pool is faster when there is one command buffer per pool (which is our case). One pool per frame-in-flight.
- **No explicit barriers between render passes**: The render pass initial/final layouts handle image layout transitions. The world and UI render passes produce images in `SHADER_READ_ONLY_OPTIMAL`, which is exactly what the composition pass needs to sample them. Vulkan guarantees that render pass layout transitions include the necessary memory barriers.
- **Fence wait at top of frame**: We wait on the fence at the START of the frame, not after present. This means the CPU can start building frame N+1's commands immediately after the GPU finishes frame N-1. With 2 frames in flight, the CPU is never blocked unless the GPU is more than 1 frame behind.

---

## 10. Pipeline Cache

```c
typedef struct r_pipeline_cache {
    VkPipelineCache handle;
    const char     *cache_file_path;    /* e.g. "quicken_pipeline_cache.bin" */
} r_pipeline_cache_t;
```

- Load cache from disk at startup (`fread` the file, pass to `VkPipelineCacheCreateInfo.pInitialData`).
- Save cache to disk at shutdown (`vkGetPipelineCacheData`, `fwrite`).
- All `vkCreateGraphicsPipelines` calls pass the cache handle.
- First launch: no cache file exists, start with empty cache. Second launch: pipelines compile from cache, near-instant.

---

## 11. Debug Support (`r_debug.c`)

### 11.1 Validation Layers

In debug builds:
- Enable `VK_LAYER_KHRONOS_validation`.
- Set up `VkDebugUtilsMessengerEXT` to log validation errors/warnings to stderr.
- Filter out known benign messages by message ID if needed.

### 11.2 GPU Timing

Use `VkQueryPool` with `VK_QUERY_TYPE_TIMESTAMP` to measure:
- World pass duration
- UI pass duration
- Composition pass duration
- Total GPU frame time

```c
#define R_TIMESTAMP_COUNT 8

typedef struct r_gpu_timers {
    VkQueryPool     pool;
    u64             results[R_TIMESTAMP_COUNT];
    f64             gpu_frame_ms;
    f64             world_pass_ms;
    f64             ui_pass_ms;
    f64             compose_pass_ms;
} r_gpu_timers_t;
```

Write timestamps with `vkCmdWriteTimestamp` before/after each render pass. Read results with `vkGetQueryPoolResults` from the previous frame (to avoid pipeline stalls). Convert to milliseconds using `timestampPeriod` from device properties.

### 11.3 Debug Labels

Use `vkCmdBeginDebugUtilsLabelEXT` / `vkCmdEndDebugUtilsLabelEXT` to label render passes in GPU profilers (RenderDoc, Nsight). Wrap in `#ifdef QUICKEN_DEBUG`.

---

## 12. Public API (`include/renderer/renderer.h`)

```c
#ifndef QUICKEN_RENDERER_H
#define QUICKEN_RENDERER_H

#include "quicken.h"

/* Result codes */
typedef enum qk_result {
    QK_SUCCESS = 0,
    QK_ERROR_VULKAN_INIT,
    QK_ERROR_NO_SUITABLE_GPU,
    QK_ERROR_SWAPCHAIN,
    QK_ERROR_PIPELINE,
    QK_ERROR_OUT_OF_MEMORY,
} qk_result_t;

/* Renderer configuration -- passed at init */
typedef struct qk_renderer_config {
    void    *sdl_window;        /* SDL_Window* -- renderer gets surface from this */
    u32      render_width;
    u32      render_height;
    u32      window_width;
    u32      window_height;
    bool     aspect_fit;        /* true = letterbox, false = stretch */
    bool     vsync;             /* true = FIFO, false = MAILBOX */
} qk_renderer_config_t;

/* Camera state for rendering */
typedef struct qk_camera {
    f32 view_projection[16];    /* column-major 4x4 */
    f32 position[3];
} qk_camera_t;

/* Opaque handle for uploaded geometry */
typedef struct qk_gpu_mesh qk_gpu_mesh_t;

/* World vertex as produced by map parser */
typedef struct qk_world_vertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
    u32 texture_id;
} qk_world_vertex_t;

/* Surface draw info */
typedef struct qk_draw_surface {
    u32 index_offset;
    u32 index_count;
    u32 vertex_offset;
    u32 texture_index;
} qk_draw_surface_t;

/* UI quad for immediate-mode rendering */
typedef struct qk_ui_quad {
    f32 x, y, w, h;
    f32 u0, v0, u1, v1;
    u32 color;
    u32 texture_id;
} qk_ui_quad_t;

/* Texture handle (index into texture array) */
typedef u32 qk_texture_id_t;

/* --- Lifecycle --- */
qk_result_t qk_renderer_init(const qk_renderer_config_t *config);
void        qk_renderer_shutdown(void);

/* --- Resolution --- */
void qk_renderer_set_render_resolution(u32 width, u32 height);
void qk_renderer_set_aspect_mode(bool aspect_fit);
void qk_renderer_handle_window_resize(u32 new_width, u32 new_height);

/* --- Resource upload (map load) --- */
qk_result_t qk_renderer_upload_world(
    const qk_world_vertex_t *vertices, u32 vertex_count,
    const u32 *indices, u32 index_count,
    const qk_draw_surface_t *surfaces, u32 surface_count);

qk_texture_id_t qk_renderer_upload_texture(
    const u8 *pixels, u32 width, u32 height, u32 channels);

void qk_renderer_free_world(void);

/* --- Frame rendering --- */
void qk_renderer_begin_frame(const qk_camera_t *camera);
void qk_renderer_draw_world(void);
void qk_renderer_push_ui_quad(const qk_ui_quad_t *quad);
void qk_renderer_end_frame(void);

/* --- Debug --- */
typedef struct qk_gpu_stats {
    f64 gpu_frame_ms;
    f64 world_pass_ms;
    f64 ui_pass_ms;
    f64 compose_pass_ms;
    u32 draw_calls;
    u32 triangles;
} qk_gpu_stats_t;

void qk_renderer_get_stats(qk_gpu_stats_t *out_stats);

#endif /* QUICKEN_RENDERER_H */
```

---

## 13. Shader Compilation

Shaders are written in GLSL and compiled to SPIR-V offline using `glslc` (from the Vulkan SDK).

Directory structure:
```
src/renderer/shaders/
    world.vert
    world.frag
    ui.vert
    ui.frag
    compose.vert
    compose.frag
```

Compiled output (checked into repo for reproducibility):
```
src/renderer/shaders/compiled/
    world.vert.spv
    world.frag.spv
    ui.vert.spv
    ui.frag.spv
    compose.vert.spv
    compose.frag.spv
```

Add a shader compilation script (`build_shaders.bat` / `build_shaders.sh`) that invokes:
```
glslc -O world.vert -o compiled/world.vert.spv
glslc -O world.frag -o compiled/world.frag.spv
...
```

Load SPIR-V at runtime with `fread` into `VkShaderModuleCreateInfo.pCode`. Shader modules are created during pipeline creation and destroyed immediately after (they are not needed after the pipeline is built).

---

## 14. Build System Changes

Update `premake5.lua` for the renderer module:

- Add Vulkan SDK include path: `$(VULKAN_SDK)/Include` (or use `vulkan/vulkan.h` from system includes).
- Add Vulkan link library: `vulkan-1` (Windows) or `vulkan` (Linux).
- The renderer module already compiles as a static lib with fast float -- no changes needed to the float settings.
- SDL3 remains a dependency for window/surface creation, but that happens in the main executable, not the renderer lib. The renderer receives an `SDL_Window*` pointer and calls `SDL_Vulkan_CreateSurface` on it. This means the renderer static lib needs SDL3 headers in its include path (already configured) and the main executable links SDL3.

Add to the `quicken-renderer` project:
```lua
filter "system:windows"
    includedirs { "$(VULKAN_SDK)/Include" }
    libdirs { "$(VULKAN_SDK)/Lib" }
    links { "vulkan-1" }

filter "system:linux"
    links { "vulkan" }
```

Note: since `quicken-renderer` is a static lib, the `links` directives for Vulkan actually need to go on the main executable (`quicken` project) that links the renderer. The renderer static lib just needs the headers. Move Vulkan link directives to the `quicken` project accordingly.

---

## 15. What I Need From Other Engineers

### 15.1 From Core / Platform Engineer

1. **SDL3 window handle**: The renderer needs `SDL_Window*` passed in `qk_renderer_config_t`. Core creates the window, renderer creates the Vulkan surface from it.
2. **Window resize events**: Core must call `qk_renderer_handle_window_resize(new_w, new_h)` when the window is resized. The renderer handles swapchain recreation internally.
3. **Frame timing**: Core provides the main loop and calls `qk_renderer_begin_frame` / `qk_renderer_end_frame`. The renderer does not own the main loop.

### 15.2 From Map / Asset Engineer

1. **Processed brush geometry**: After parsing a `.map` file, provide arrays of `qk_world_vertex_t` and `u32` indices. Each brush face becomes a set of triangles. The renderer does not do BSP traversal, CSG, or face tessellation -- it receives final triangle soup.
2. **Surface descriptors**: An array of `qk_draw_surface_t` that maps index ranges to textures. This allows the renderer to batch by texture.
3. **Texture data**: Raw pixel data (RGBA8, top-left origin) with width/height. Call `qk_renderer_upload_texture()` and receive a `qk_texture_id_t` that matches the `texture_index` in `qk_draw_surface_t`.
4. **Texture coordinate generation**: The map parser must compute UV coordinates for each vertex based on Quake's texture projection (surface axes + offset + scale). The renderer does not interpret Quake texture alignment data.

### 15.3 From Game / UI Engineer

1. **UI quads each frame**: Call `qk_renderer_push_ui_quad()` for every UI element. Submit in back-to-front order (painter's algorithm). The renderer does not sort by depth.
2. **Bitmap font atlas**: Provide a texture atlas of pre-rasterized glyphs and a table mapping character codes to UV rects. The UI engineer is responsible for text layout; the renderer just draws textured quads.
3. **Camera matrix**: The game provides the `qk_camera_t` each frame with a pre-computed view-projection matrix. The renderer does not compute the view or projection matrix -- it receives the combined matrix ready to multiply with vertex positions.

---

## 16. Implementation Order

This is the build order. Each step produces a testable result.

### Phase 1: Vulkan Bootstrap (get a colored screen)
1. `r_vulkan.c`: Instance creation, debug messenger, physical device selection.
2. `r_vulkan.c`: Logical device, queue creation.
3. `r_vulkan.c`: Surface (from SDL window), swapchain creation.
4. `r_commands.c`: Command pool, command buffer, frame sync objects.
5. `r_vulkan.c`: Record a command buffer that clears the swapchain image to a solid color and presents it.
6. **Test**: Window shows a solid color. Resize works (swapchain recreation).

### Phase 2: Composition Pipeline (offscreen to swapchain)
1. `r_memory.c`: Basic memory allocator (bump allocator in large blocks).
2. `r_texture.c`: Image/view creation helpers.
3. Create world and UI offscreen render targets (clear to different colors for debugging).
4. `r_pipeline.c`: Shader module loading, pipeline cache.
5. `r_compose.c`: Composition pipeline, descriptor set, fullscreen triangle.
6. Render both offscreen targets (just clear them), compose to swapchain.
7. **Test**: Window shows the world clear color with UI clear color alpha-blended on top. Changing render resolution works. Aspect fit/stretch works.

### Phase 3: World Rendering (triangles on screen)
1. `r_world.c`: World pipeline creation.
2. Upload a hardcoded triangle or cube (no map parser yet).
3. Render to world offscreen target, compose to swapchain.
4. Implement view uniforms, camera.
5. **Test**: Spinning cube on screen at configurable render resolution.

### Phase 4: World Geometry from Map Data
1. Implement `qk_renderer_upload_world()` with staging buffer upload.
2. Implement texture-sorted draw call batching.
3. `r_texture.c`: Texture upload, mipmap generation.
4. Integrate with map parser (receive real brush geometry).
5. **Test**: A Quake .map rendered on screen. Walk around with WASD + mouse look.

### Phase 5: UI Rendering
1. `r_ui.c`: UI pipeline, vertex buffer per frame, index buffer.
2. Implement `qk_renderer_push_ui_quad()` and draw batching.
3. Create 1x1 white pixel default texture.
4. **Test**: Colored rectangles drawn over the world. Basic text with bitmap font.

### Phase 6: Polish and Performance
1. `r_debug.c`: GPU timestamp queries, stats reporting.
2. Pipeline cache save/load.
3. Profile and optimize draw call batching.
4. Verify mailbox present mode, measure input-to-photon latency.
5. **Test**: 1000+ fps on high-end hardware with a Quake map. GPU timings reported.

---

## 17. Renderer Global State

All renderer state lives in a single static struct, file-scoped in `r_vulkan.c`, with accessors for other `r_*.c` files within the renderer module.

```c
/* In r_vulkan.c -- not exposed in any header */
typedef struct r_state {
    r_instance_t         instance;
    r_device_t           device;
    r_swapchain_t        swapchain;
    r_render_config_t    config;

    /* Offscreen render targets */
    r_render_target_t    world_target;
    r_render_target_t    ui_target;

    /* Per-frame data */
    r_frame_data_t       frames[R_FRAMES_IN_FLIGHT];
    u32                  frame_index;

    /* Pipelines */
    VkPipelineCache      pipeline_cache;
    r_pipeline_t         world_pipeline;
    r_pipeline_t         ui_pipeline;
    r_pipeline_t         compose_pipeline;

    /* Descriptor set layouts */
    VkDescriptorSetLayout view_set_layout;
    VkDescriptorSetLayout texture_set_layout;
    VkDescriptorSetLayout compose_set_layout;
    VkDescriptorPool      descriptor_pool;

    /* Composition pass resources */
    VkDescriptorSet       compose_descriptor_set;
    VkSampler             compose_sampler;

    /* World geometry (current map) */
    r_world_geometry_t    world;

    /* Textures */
    r_texture_manager_t   textures;

    /* Memory pools */
    r_memory_pool_t       pools[R_MEMORY_POOL_COUNT];

    /* Staging */
    r_staging_buffer_t    staging;

    /* UI state (current frame) */
    r_ui_quad_t           ui_quads[R_UI_MAX_QUADS];
    u32                   ui_quad_count;

    /* Pre-built UI index buffer */
    VkBuffer              ui_index_buffer;
    VkDeviceMemory        ui_index_memory;

    /* Debug */
    r_gpu_timers_t        gpu_timers;
    qk_gpu_stats_t        stats;
} r_state_t;

static r_state_t r;
```

Other `r_*.c` files access this through `extern` declarations of specific sub-structs or via getter functions. The exact mechanism is an implementation detail -- the key point is that all renderer state is centralized, not scattered across file-level globals.

---

## 18. Open Questions and Future Considerations

These are NOT part of the vertical slice. Documenting them here so they are not forgotten.

1. **Bindless textures**: Replace per-draw texture descriptor binding with a descriptor indexing array. Use `texture_id` from the vertex data to index into the array in the fragment shader. Eliminates all texture-related descriptor set switches. Requires Vulkan 1.2 `descriptorIndexing`.

2. **Indirect drawing**: Replace per-batch `vkCmdDrawIndexed` with `vkCmdDrawIndexedIndirect`. Build the indirect draw buffer on the CPU (or GPU via compute shader for frustum culling). One draw call for all world geometry.

3. **Async compute**: Overlap compute work (culling, particle simulation) with the previous frame's present. Requires timeline semaphores and a compute queue.

4. **Dynamic geometry**: Player models, projectiles, item pickups. Requires a per-frame vertex buffer upload path (ring buffer). Not needed for vertical slice if we only render the static world.

5. **Lightmaps**: Quake maps include lightmap data. Implement as a second UV channel and lightmap atlas texture. Multiply surface texture by lightmap in the fragment shader. Significant visual upgrade over the directional light in the vertical slice.

6. **MSAA**: Add optional multisampling. Requires multisampled color attachment, resolve step. Toggle-able for performance.
