/*
 * QUICKEN Input Sampling - Real SDL3 Implementation
 *
 * Polls SDL3 events, tracks key state, mouse delta, and builds usercmds.
 * Mouse is captured (relative mode) for FPS-style control.
 */

#include "core/qk_input.h"

#ifndef QK_HEADLESS

#include "ui/qk_console.h"
#include "core/qk_cvar.h"
#include <SDL3/SDL.h>
#include <string.h>
#include <math.h>

/* Persistent state between polls */
static bool    s_keys[512];
static bool    s_mouse_buttons[5];
static i32     s_mouse_dx;
static i32     s_mouse_dy;
static bool    s_quit_requested;
static bool    s_mouse_captured;
static f32     s_yaw;
static f32     s_pitch;

/* Cached cvar pointer for zero-overhead reads */
static qk_cvar_t *s_cvar_sensitivity;

#define QK_PITCH_MIN           -89.0f
#define QK_PITCH_MAX            89.0f

void qk_input_poll(qk_input_state_t *state) {
    s_mouse_dx = 0;
    s_mouse_dy = 0;

    bool console_open = qk_console_is_open();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            s_quit_requested = true;
            break;

        case SDL_EVENT_KEY_DOWN:
            /* Tilde toggle (non-repeat) */
            if (event.key.scancode == SDL_SCANCODE_GRAVE && !event.key.repeat) {
                qk_console_toggle();
                console_open = qk_console_is_open();

                if (console_open) {
                    /* Release mouse, start text input */
                    SDL_Window *win = SDL_GetKeyboardFocus();
                    if (win) {
                        SDL_SetWindowRelativeMouseMode(win, false);
                        SDL_StartTextInput(win);
                    }
                    s_mouse_captured = false;
                } else {
                    /* Stop text input; mouse re-captures on next click */
                    SDL_Window *win = SDL_GetKeyboardFocus();
                    if (win) {
                        SDL_StopTextInput(win);
                    }
                }
                break;
            }

            if (console_open) {
                /* Route key events to console */
                qk_console_key_event(event.key.scancode, true);

                /* If console closed via Escape, manage mouse/text */
                if (!qk_console_is_open()) {
                    console_open = false;
                    SDL_Window *win = SDL_GetKeyboardFocus();
                    if (win) {
                        SDL_StopTextInput(win);
                    }
                }
                break;
            }

            /* Normal game key handling */
            if (event.key.scancode < 512) {
                s_keys[event.key.scancode] = true;
            }
            if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                if (s_mouse_captured) {
                    SDL_SetWindowRelativeMouseMode(SDL_GetKeyboardFocus(), false);
                    s_mouse_captured = false;
                } else {
                    s_quit_requested = true;
                }
            }
            break;

        case SDL_EVENT_KEY_UP:
            if (!console_open) {
                if (event.key.scancode < 512) {
                    s_keys[event.key.scancode] = false;
                }
            }
            break;

        case SDL_EVENT_TEXT_INPUT:
            if (console_open) {
                qk_console_text_event(event.text.text);
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (console_open) break; /* ignore clicks when console open */

            if (event.button.button <= 5) {
                s_mouse_buttons[event.button.button - 1] = true;
            }
            if (!s_mouse_captured) {
                SDL_SetWindowRelativeMouseMode(SDL_GetKeyboardFocus(), true);
                s_mouse_captured = true;
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (console_open) break;
            if (event.button.button <= 5) {
                s_mouse_buttons[event.button.button - 1] = false;
            }
            break;

        case SDL_EVENT_MOUSE_MOTION:
            if (s_mouse_captured && !console_open) {
                s_mouse_dx += (i32)event.motion.xrel;
                s_mouse_dy += (i32)event.motion.yrel;
            }
            break;

        case SDL_EVENT_WINDOW_RESIZED:
            break;

        default:
            break;
        }
    }

    /* Update view angles from mouse (only when not in console) */
    if (s_mouse_captured && !console_open) {
        /* Lazy-cache the sensitivity cvar pointer */
        if (!s_cvar_sensitivity) {
            s_cvar_sensitivity = qk_cvar_find("sensitivity");
        }
        f32 sens = s_cvar_sensitivity ? s_cvar_sensitivity->value.f : 0.022f;

        s_yaw -= (f32)s_mouse_dx * sens;
        s_pitch -= (f32)s_mouse_dy * sens;

        while (s_yaw < 0.0f) s_yaw += 360.0f;
        while (s_yaw >= 360.0f) s_yaw -= 360.0f;

        if (s_pitch < QK_PITCH_MIN) s_pitch = QK_PITCH_MIN;
        if (s_pitch > QK_PITCH_MAX) s_pitch = QK_PITCH_MAX;
    }

    console_open = qk_console_is_open();

    if (state) {
        memcpy(state->keys, s_keys, sizeof(s_keys));
        state->mouse_dx = s_mouse_dx;
        state->mouse_dy = s_mouse_dy;
        memcpy(state->mouse_buttons, s_mouse_buttons, sizeof(s_mouse_buttons));
        state->quit_requested = s_quit_requested;
        state->console_active = console_open;
    }
}

qk_usercmd_t qk_input_build_usercmd(const qk_input_state_t *state, u32 server_time) {
    qk_usercmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.server_time = server_time;

    if (!state) return cmd;

    /* No game input while console is open */
    if (state->console_active) {
        cmd.pitch = s_pitch;
        cmd.yaw = s_yaw;
        return cmd;
    }

    /* Movement from WASD */
    f32 forward = 0.0f;
    f32 side = 0.0f;

    if (state->keys[SDL_SCANCODE_W]) forward += 1.0f;
    if (state->keys[SDL_SCANCODE_S]) forward -= 1.0f;
    if (state->keys[SDL_SCANCODE_D]) side += 1.0f;
    if (state->keys[SDL_SCANCODE_A]) side -= 1.0f;

    /* Normalize diagonal movement */
    f32 len = sqrtf(forward * forward + side * side);
    if (len > 1.0f) {
        forward /= len;
        side /= len;
    }

    cmd.forward_move = forward;
    cmd.side_move = side;

    /* View angles */
    cmd.pitch = s_pitch;
    cmd.yaw = s_yaw;

    /* Buttons */
    if (state->mouse_buttons[0]) cmd.buttons |= QK_BUTTON_ATTACK;
    if (state->keys[SDL_SCANCODE_SPACE]) cmd.buttons |= QK_BUTTON_JUMP;
    if (state->keys[SDL_SCANCODE_LCTRL] || state->keys[SDL_SCANCODE_C]) {
        cmd.buttons |= QK_BUTTON_CROUCH;
    }
    if (state->keys[SDL_SCANCODE_E] || state->keys[SDL_SCANCODE_F]) {
        cmd.buttons |= QK_BUTTON_USE;
    }

    /* Weapon select (number keys) */
    if (state->keys[SDL_SCANCODE_1]) cmd.weapon_select = QK_WEAPON_ROCKET;
    if (state->keys[SDL_SCANCODE_2]) cmd.weapon_select = QK_WEAPON_RAIL;
    if (state->keys[SDL_SCANCODE_3]) cmd.weapon_select = QK_WEAPON_LG;

    return cmd;
}

f32 qk_input_get_pitch(void) { return s_pitch; }
f32 qk_input_get_yaw(void) { return s_yaw; }

#else /* QK_HEADLESS */

#include <string.h>

void qk_input_poll(qk_input_state_t *state) {
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

qk_usercmd_t qk_input_build_usercmd(const qk_input_state_t *state, u32 server_time) {
    QK_UNUSED(state);
    qk_usercmd_t cmd = {0};
    cmd.server_time = server_time;
    return cmd;
}

f32 qk_input_get_pitch(void) { return 0.0f; }
f32 qk_input_get_yaw(void) { return 0.0f; }

#endif /* QK_HEADLESS */
