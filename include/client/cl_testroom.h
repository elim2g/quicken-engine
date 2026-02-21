/*
 * QUICKEN Engine - Test Room
 *
 * Procedural grid texture and box geometry for the default test room.
 */

#ifndef CL_TESTROOM_H
#define CL_TESTROOM_H

#include "renderer/qk_renderer.h"

qk_texture_id_t cl_testroom_create_texture(void);
void            cl_testroom_upload_geometry(qk_texture_id_t tex_id);

#endif /* CL_TESTROOM_H */
