/*
 * QUICKEN Renderer Module - Public API
 *
 * Graphics rendering and visual effects.
 * Uses fast floating-point with aggressive optimizations.
 */

#ifndef QUICKEN_RENDERER_H
#define QUICKEN_RENDERER_H

#include "quicken.h"

/* Initialize renderer */
void renderer_init(void);

/* Begin rendering a new frame */
void renderer_begin_frame(void);

/* Finish and present the current frame */
void renderer_end_frame(void);

/* Cleanup renderer */
void renderer_shutdown(void);

#endif /* QUICKEN_RENDERER_H */
