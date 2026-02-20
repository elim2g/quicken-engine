/*
 * QUICKEN Engine - Developer Console
 *
 * Half-screen drop-down console (Quake style). Tilde to toggle.
 * Supports command execution, cvar manipulation, tab-completion,
 * command history, and scrollback.
 */

#ifndef QK_CONSOLE_H
#define QK_CONSOLE_H

#include "quicken.h"

// Command function signature: func(argc, argv)
typedef void (*qk_console_cmd_func_t)(i32 argc, const char **argv);

// --- Lifecycle ---
void    qk_console_init(void);
void    qk_console_shutdown(void);

// --- State ---
bool    qk_console_is_open(void);
void    qk_console_toggle(void);

// --- Input (called from qk_input when console is open) ---
void    qk_console_key_event(u32 scancode, bool pressed);
void    qk_console_text_event(const char *text);

// --- Rendering ---
void    qk_console_draw(f32 screen_w, f32 screen_h, f32 dt);

// --- Output ---
void    qk_console_print(const char *text);
void    qk_console_printf(const char *fmt, ...);

// --- Command registration ---
void    qk_console_register_cmd(const char *name,
                                 qk_console_cmd_func_t func,
                                 const char *desc);

#endif /* QK_CONSOLE_H */
