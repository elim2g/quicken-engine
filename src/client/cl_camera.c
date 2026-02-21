/*
 * QUICKEN Engine - Client Camera
 */

#include "client/cl_camera.h"

#include <math.h>
#include <string.h>

static void build_perspective(f32 *out, f32 fov_deg, f32 aspect,
                               f32 znear, f32 zfar) {
    memset(out, 0, 16 * sizeof(f32));
    f32 fov_rad = fov_deg * (3.14159265f / 180.0f);
    f32 focal = 1.0f / tanf(fov_rad * 0.5f);

    out[0]  = focal / aspect;
    out[5]  = -focal;  // Vulkan Y-down in clip space
    out[10] = zfar / (znear - zfar);
    out[11] = -1.0f;
    out[14] = (znear * zfar) / (znear - zfar);
}

static void build_view(f32 *out, f32 pos_x, f32 pos_y, f32 pos_z,
                        f32 pitch_deg, f32 yaw_deg) {
    f32 pitch_rad = pitch_deg * (3.14159265f / 180.0f);
    f32 yaw_rad   = yaw_deg   * (3.14159265f / 180.0f);

    f32 cos_pitch = cosf(pitch_rad), sin_pitch = sinf(pitch_rad);
    f32 cos_yaw   = cosf(yaw_rad),   sin_yaw   = sinf(yaw_rad);

    // forward = direction the camera looks
    f32 fwd_x = cos_pitch * cos_yaw;
    f32 fwd_y = cos_pitch * sin_yaw;
    f32 fwd_z = sin_pitch;

    // right = cross(forward, world_up)
    f32 right_x = sin_yaw;
    f32 right_y = -cos_yaw;
    f32 right_z = 0.0f;

    // up = cross(right, forward)
    f32 up_x = right_y * fwd_z - right_z * fwd_y;
    f32 up_y = right_z * fwd_x - right_x * fwd_z;
    f32 up_z = right_x * fwd_y - right_y * fwd_x;

    // View matrix (column-major)
    memset(out, 0, 16 * sizeof(f32));
    out[0] = right_x;  out[4] = right_y;  out[8]  = right_z;
    out[1] = up_x;     out[5] = up_y;     out[9]  = up_z;
    out[2] = -fwd_x;   out[6] = -fwd_y;   out[10] = -fwd_z;

    out[12] = -(right_x * pos_x + right_y * pos_y + right_z * pos_z);
    out[13] = -(up_x * pos_x + up_y * pos_y + up_z * pos_z);
    out[14] = -(-fwd_x * pos_x + (-fwd_y) * pos_y + (-fwd_z) * pos_z);
    out[15] = 1.0f;
}

static void mat4_mul(f32 *out, const f32 *a, const f32 *b) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[i * 4 + j] = 0.0f;
            for (int k = 0; k < 4; k++) {
                out[i * 4 + j] += a[k * 4 + j] * b[i * 4 + k];
            }
        }
    }
}

qk_camera_t cl_camera_build(f32 pos_x, f32 pos_y, f32 pos_z,
                              f32 pitch, f32 yaw,
                              f32 fov, f32 aspect) {
    qk_camera_t cam;
    f32 proj[16], view[16];

    build_perspective(proj, fov, aspect, 0.1f, 4096.0f);

    // Eye position is at player origin + eye height
    f32 eye_z = pos_z + 26.0f;
    build_view(view, pos_x, pos_y, eye_z, pitch, yaw);

    mat4_mul(cam.view_projection, proj, view);
    cam.position[0] = pos_x;
    cam.position[1] = pos_y;
    cam.position[2] = eye_z;

    return cam;
}
