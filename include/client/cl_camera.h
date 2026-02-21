/*
 * QUICKEN Engine - Client Camera
 *
 * Perspective projection, view matrix, VP construction.
 */

#ifndef CL_CAMERA_H
#define CL_CAMERA_H

#include "renderer/qk_renderer.h"

qk_camera_t cl_camera_build(f32 pos_x, f32 pos_y, f32 pos_z,
                              f32 pitch, f32 yaw,
                              f32 fov, f32 aspect);

#endif /* CL_CAMERA_H */
