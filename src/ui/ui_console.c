/*
 * QUICKEN Engine - Developer Console Implementation
 *
 * Drop-down console with command execution, cvar manipulation,
 * tab-completion, history, scrollback, and slide animation.
 */

#include "ui/qk_console.h"
#include "core/qk_cvar.h"
#include "renderer/qk_renderer.h"

#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

// --- Constants ---

#define CON_INPUT_LEN        256
#define CON_SCROLLBACK_LINES 1024
#define CON_LINE_LEN         256
#define CON_HISTORY_SIZE     64
#define CON_MAX_COMMANDS     128
#define CON_MAX_TOKENS       32

static const f32 CON_FONT_SIZE    = 14.0f;
static const f32 CON_SLIDE_SPEED  = 8.0f;  // 1/seconds to fully open (~125ms)
static const u32 CON_BG_COLOR     = 0x1A1A1ACC; // dark semi-transparent
static const u32 CON_BORDER_COLOR = 0xFF8800FF;  // orange
static const u32 CON_TEXT_COLOR   = 0xCCCCCCFF;  // light gray
static const u32 CON_INPUT_COLOR  = 0xFFFFFFFF;  // white
static const u32 CON_ECHO_COLOR   = 0x88FF88FF;  // green for echoed commands
static const u32 CON_ERROR_COLOR  = 0xFF4444FF;  // red for errors
static const u32 CON_CVAR_COLOR   = 0xFFCC44FF;  // yellow for cvar info
static const char CON_PROMPT_CHAR = ']';

// --- Scrollback line ---

typedef struct {
    char text[CON_LINE_LEN];
    u32  color;
} con_line_t;

// --- Registered command ---

typedef struct {
    char                    name[64];
    char                    desc[128];
    qk_console_cmd_func_t  func;
    bool                    in_use;
} con_cmd_t;

// --- Console state (all static, zero cost when not used) ---

static struct {
    bool        open;
    f32         slide_frac;         // 0 = closed, 1 = fully open

    // Input line
    char        input[CON_INPUT_LEN];
    u32         input_len;
    u32         cursor;

    // Scrollback
    con_line_t  lines[CON_SCROLLBACK_LINES];
    u32         line_head;          // next write position (ring)
    u32         line_count;         // total lines written (up to max)
    i32         scroll_offset;      // lines scrolled up from bottom

    // History
    char        history[CON_HISTORY_SIZE][CON_INPUT_LEN];
    u32         history_head;
    u32         history_count;
    i32         history_pos;        // -1 = not browsing, 0..N = browsing
    char        history_saved[CON_INPUT_LEN]; // saved input when browsing

    // Tab completion
    char        tab_prefix[CON_INPUT_LEN];
    i32         tab_index;          // -1 = no tab state
    bool        tab_active;

    // Commands
    con_cmd_t   commands[CON_MAX_COMMANDS];
    u32         command_count;

    // Tilde suppression
    bool        suppress_next_text;

    // Cursor blink
    f32         cursor_blink_timer;

    bool        initialized;
} s_con;

// --- Forward declarations ---

static void con_execute(const char *text);
static void con_push_history(const char *text);

// --- Built-in commands ---

static void cmd_help(i32 argc, const char **argv) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);
    qk_console_printf("Available commands:");
    for (u32 i = 0; i < s_con.command_count; i++) {
        if (!s_con.commands[i].in_use) continue;
        qk_console_printf("  %-20s %s", s_con.commands[i].name,
                           s_con.commands[i].desc);
    }
    qk_console_printf("Any cvar name is also a valid command.");
}

static void cmd_clear(i32 argc, const char **argv) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);
    s_con.line_count = 0;
    s_con.line_head = 0;
    s_con.scroll_offset = 0;
}

static void cmd_quit(i32 argc, const char **argv) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);
    // Post an SDL quit event
    SDL_Event e;
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

static void cmd_cvarlist(i32 argc, const char **argv) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);
    u32 count = qk_cvar_count();
    qk_cvar_t *all = qk_cvar_get_all();
    qk_console_printf("--- %u cvars ---", count);
    for (u32 i = 0; i < count; i++) {
        if (!all[i].in_use) continue;
        char val_buf[64];
        qk_cvar_to_string(&all[i], val_buf, sizeof(val_buf));
        qk_console_printf("  %-24s = %s", all[i].name, val_buf);
    }
}

static void cmd_reset(i32 argc, const char **argv) {
    if (argc < 2) {
        qk_console_print("Usage: reset <cvar_name>");
        return;
    }
    qk_cvar_t *cv = qk_cvar_find(argv[1]);
    if (!cv) {
        qk_console_printf("Unknown cvar: %s", argv[1]);
        return;
    }
    qk_cvar_reset(cv);
    char val_buf[64];
    qk_cvar_to_string(cv, val_buf, sizeof(val_buf));
    qk_console_printf("%s reset to %s", cv->name, val_buf);
}

static void cmd_echo(i32 argc, const char **argv) {
    if (argc < 2) return;
    char buf[CON_LINE_LEN];
    u32 pos = 0;
    for (i32 i = 1; i < argc; i++) {
        if (i > 1 && pos < CON_LINE_LEN - 1) buf[pos++] = ' ';
        u32 len = (u32)strlen(argv[i]);
        if (pos + len >= CON_LINE_LEN) len = CON_LINE_LEN - 1 - pos;
        memcpy(buf + pos, argv[i], len);
        pos += len;
    }
    buf[pos] = '\0';
    qk_console_print(buf);
}

// --- Lifecycle ---

void qk_console_init(void) {
    memset(&s_con, 0, sizeof(s_con));
    s_con.history_pos = -1;
    s_con.tab_index = -1;

    qk_console_register_cmd("help",     cmd_help,     "List all commands");
    qk_console_register_cmd("clear",    cmd_clear,    "Clear console output");
    qk_console_register_cmd("quit",     cmd_quit,     "Quit the game");
    qk_console_register_cmd("exit",     cmd_quit,     "Quit the game");
    qk_console_register_cmd("cvarlist", cmd_cvarlist,  "List all cvars");
    qk_console_register_cmd("reset",    cmd_reset,    "Reset cvar to default");
    qk_console_register_cmd("echo",     cmd_echo,     "Print text to console");

    s_con.initialized = true;
    qk_console_print("QUICKEN Console initialized. Type 'help' for commands.");
}

void qk_console_shutdown(void) {
    s_con.initialized = false;
}

// --- State ---

bool qk_console_is_open(void) {
    return s_con.open;
}

void qk_console_toggle(void) {
    s_con.open = !s_con.open;
    s_con.suppress_next_text = true;
    s_con.tab_active = false;
    s_con.tab_index = -1;
}

// --- Output ---

static void con_push_line(const char *text, u32 color) {
    con_line_t *line = &s_con.lines[s_con.line_head % CON_SCROLLBACK_LINES];
    snprintf(line->text, CON_LINE_LEN, "%s", text);
    line->color = color;
    s_con.line_head++;
    if (s_con.line_count < CON_SCROLLBACK_LINES) s_con.line_count++;

    // Auto-scroll to bottom when new text arrives
    s_con.scroll_offset = 0;
}

void qk_console_print(const char *text) {
    if (!text) return;

    // Handle multi-line: split on newlines
    const char *start = text;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            char buf[CON_LINE_LEN];
            u32 len = (u32)(p - start);
            if (len >= CON_LINE_LEN) len = CON_LINE_LEN - 1;
            memcpy(buf, start, len);
            buf[len] = '\0';
            con_push_line(buf, CON_TEXT_COLOR);
            if (*p == '\0') break;
            start = p + 1;
        }
    }
}

void qk_console_printf(const char *fmt, ...) {
    char buf[CON_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    qk_console_print(buf);
}

// --- Command registration ---

void qk_console_register_cmd(const char *name, qk_console_cmd_func_t func,
                               const char *desc) {
    if (s_con.command_count >= CON_MAX_COMMANDS) return;

    con_cmd_t *cmd = &s_con.commands[s_con.command_count++];
    cmd->in_use = true;
    snprintf(cmd->name, sizeof(cmd->name), "%s", name);
    snprintf(cmd->desc, sizeof(cmd->desc), "%s", desc ? desc : "");
    cmd->func = func;
}

// --- Tokenizer (Q3 style: space-delimited, quoted strings) ---

static i32 con_tokenize(const char *text, const char *tokens[], char *token_buf,
                          u32 buf_size) {
    i32 argc = 0;
    u32 buf_pos = 0;
    const char *p = text;

    while (*p && argc < CON_MAX_TOKENS) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        const char *start;
        u32 len;

        if (*p == '"') {
            // Quoted string
            p++;
            start = p;
            while (*p && *p != '"') p++;
            len = (u32)(p - start);
            if (*p == '"') p++;
        } else {
            start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            len = (u32)(p - start);
        }

        if (buf_pos + len + 1 > buf_size) break;
        memcpy(token_buf + buf_pos, start, len);
        token_buf[buf_pos + len] = '\0';
        tokens[argc++] = token_buf + buf_pos;
        buf_pos += len + 1;
    }

    return argc;
}

// --- Command execution ---

static void con_execute(const char *text) {
    if (!text || !text[0]) return;

    // Echo command in green
    char echo_buf[CON_LINE_LEN];
    snprintf(echo_buf, sizeof(echo_buf), "] %.253s", text);
    con_push_line(echo_buf, CON_ECHO_COLOR);

    // Push to history
    con_push_history(text);

    // Tokenize
    const char *tokens[CON_MAX_TOKENS];
    char token_buf[1024];
    i32 argc = con_tokenize(text, tokens, token_buf, sizeof(token_buf));
    if (argc == 0) return;

    // 1. Try registered commands first
    for (u32 i = 0; i < s_con.command_count; i++) {
        if (!s_con.commands[i].in_use) continue;
        if (strcmp(s_con.commands[i].name, tokens[0]) == 0) {
            s_con.commands[i].func(argc, tokens);
            return;
        }
    }

    // 2. Try cvar lookup
    qk_cvar_t *cv = qk_cvar_find(tokens[0]);
    if (cv) {
        if (argc == 1) {
            // Print current value
            char val_buf[64];
            qk_cvar_to_string(cv, val_buf, sizeof(val_buf));
            char range_buf[64] = "";
            if (cv->has_range) {
                snprintf(range_buf, sizeof(range_buf), " [%.4g..%.4g]",
                         (double)cv->min_val, (double)cv->max_val);
            }
            con_push_line(val_buf, CON_CVAR_COLOR);
            if (range_buf[0]) {
                con_push_line(range_buf, CON_TEXT_COLOR);
            }
        } else {
            // Set value from string
            if (cv->flags & QK_CVAR_READONLY) {
                con_push_line("Cvar is read-only.", CON_ERROR_COLOR);
            } else if (qk_cvar_set_from_string(cv, tokens[1])) {
                char val_buf[64];
                qk_cvar_to_string(cv, val_buf, sizeof(val_buf));
                char msg[CON_LINE_LEN];
                snprintf(msg, sizeof(msg), "%s = %s", cv->name, val_buf);
                con_push_line(msg, CON_CVAR_COLOR);
            } else {
                con_push_line("Failed to set cvar.", CON_ERROR_COLOR);
            }
        }
        return;
    }

    // 3. Unknown
    char err_buf[CON_LINE_LEN];
    snprintf(err_buf, sizeof(err_buf), "Unknown command: %s", tokens[0]);
    con_push_line(err_buf, CON_ERROR_COLOR);
}

// --- History ---

static void con_push_history(const char *text) {
    if (!text || !text[0]) return;

    // Don't push duplicates of the last entry
    if (s_con.history_count > 0) {
        u32 last = (s_con.history_head + CON_HISTORY_SIZE - 1) % CON_HISTORY_SIZE;
        if (strcmp(s_con.history[last], text) == 0) return;
    }

    snprintf(s_con.history[s_con.history_head], CON_INPUT_LEN, "%s", text);
    s_con.history_head = (s_con.history_head + 1) % CON_HISTORY_SIZE;
    if (s_con.history_count < CON_HISTORY_SIZE) s_con.history_count++;
    s_con.history_pos = -1;
}

static void con_history_up(void) {
    if (s_con.history_count == 0) return;

    if (s_con.history_pos == -1) {
        // Save current input
        snprintf(s_con.history_saved, CON_INPUT_LEN, "%s", s_con.input);
        s_con.history_pos = 0;
    } else if (s_con.history_pos < (i32)s_con.history_count - 1) {
        s_con.history_pos++;
    } else {
        return;
    }

    // Get entry: history_head - 1 - history_pos
    u32 idx = (s_con.history_head + CON_HISTORY_SIZE - 1 - (u32)s_con.history_pos) % CON_HISTORY_SIZE;
    snprintf(s_con.input, CON_INPUT_LEN, "%s", s_con.history[idx]);
    s_con.input_len = (u32)strlen(s_con.input);
    s_con.cursor = s_con.input_len;
}

static void con_history_down(void) {
    if (s_con.history_pos == -1) return;

    s_con.history_pos--;
    if (s_con.history_pos < 0) {
        // Restore saved input
        snprintf(s_con.input, CON_INPUT_LEN, "%s", s_con.history_saved);
        s_con.input_len = (u32)strlen(s_con.input);
        s_con.cursor = s_con.input_len;
        s_con.history_pos = -1;
        return;
    }

    u32 idx = (s_con.history_head + CON_HISTORY_SIZE - 1 - (u32)s_con.history_pos) % CON_HISTORY_SIZE;
    snprintf(s_con.input, CON_INPUT_LEN, "%s", s_con.history[idx]);
    s_con.input_len = (u32)strlen(s_con.input);
    s_con.cursor = s_con.input_len;
}

// --- Tab Completion ---

static void con_tab_complete(void) {
    // Build prefix from input if not already in tab mode
    if (!s_con.tab_active) {
        snprintf(s_con.tab_prefix, CON_INPUT_LEN, "%s", s_con.input);
        s_con.tab_index = -1;
        s_con.tab_active = true;
    }

    u32 prefix_len = (u32)strlen(s_con.tab_prefix);
    if (prefix_len == 0) return;

    // Build a list of matches: commands first, then cvars
    const char *matches[256];
    u32 match_count = 0;

    for (u32 i = 0; i < s_con.command_count && match_count < 256; i++) {
        if (!s_con.commands[i].in_use) continue;
        if (strncmp(s_con.commands[i].name, s_con.tab_prefix, prefix_len) == 0) {
            matches[match_count++] = s_con.commands[i].name;
        }
    }

    u32 cvar_count = qk_cvar_count();
    qk_cvar_t *cvars = qk_cvar_get_all();
    for (u32 i = 0; i < cvar_count && match_count < 256; i++) {
        if (!cvars[i].in_use) continue;
        if (strncmp(cvars[i].name, s_con.tab_prefix, prefix_len) == 0) {
            matches[match_count++] = cvars[i].name;
        }
    }

    if (match_count == 0) return;

    // Cycle through matches
    s_con.tab_index = (s_con.tab_index + 1) % (i32)match_count;

    // Fill input with match + trailing space
    snprintf(s_con.input, CON_INPUT_LEN, "%s ", matches[s_con.tab_index]);
    s_con.input_len = (u32)strlen(s_con.input);
    s_con.cursor = s_con.input_len;
}

// --- Input handling ---

void qk_console_key_event(u32 scancode, bool pressed) {
    if (!pressed) return;

    // Reset tab state on any key except Tab
    if (scancode != SDL_SCANCODE_TAB) {
        s_con.tab_active = false;
        s_con.tab_index = -1;
    }

    switch (scancode) {
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        if (s_con.input_len > 0) {
            con_execute(s_con.input);
            s_con.input[0] = '\0';
            s_con.input_len = 0;
            s_con.cursor = 0;
        }
        break;

    case SDL_SCANCODE_BACKSPACE:
        if (s_con.cursor > 0) {
            // Shift characters left
            memmove(&s_con.input[s_con.cursor - 1],
                    &s_con.input[s_con.cursor],
                    s_con.input_len - s_con.cursor + 1);
            s_con.cursor--;
            s_con.input_len--;
        }
        break;

    case SDL_SCANCODE_DELETE:
        if (s_con.cursor < s_con.input_len) {
            memmove(&s_con.input[s_con.cursor],
                    &s_con.input[s_con.cursor + 1],
                    s_con.input_len - s_con.cursor);
            s_con.input_len--;
        }
        break;

    case SDL_SCANCODE_LEFT:
        if (s_con.cursor > 0) s_con.cursor--;
        break;

    case SDL_SCANCODE_RIGHT:
        if (s_con.cursor < s_con.input_len) s_con.cursor++;
        break;

    case SDL_SCANCODE_HOME:
        s_con.cursor = 0;
        break;

    case SDL_SCANCODE_END:
        s_con.cursor = s_con.input_len;
        break;

    case SDL_SCANCODE_UP:
        con_history_up();
        break;

    case SDL_SCANCODE_DOWN:
        con_history_down();
        break;

    case SDL_SCANCODE_PAGEUP:
        s_con.scroll_offset += 5;
        if (s_con.scroll_offset > (i32)s_con.line_count) {
            s_con.scroll_offset = (i32)s_con.line_count;
        }
        break;

    case SDL_SCANCODE_PAGEDOWN:
        s_con.scroll_offset -= 5;
        if (s_con.scroll_offset < 0) s_con.scroll_offset = 0;
        break;

    case SDL_SCANCODE_TAB:
        con_tab_complete();
        break;

    case SDL_SCANCODE_ESCAPE:
        s_con.open = false;
        break;

    default:
        break;
    }
}

void qk_console_text_event(const char *text) {
    if (!text) return;

    // Suppress tilde/backtick that triggered the toggle
    if (s_con.suppress_next_text) {
        s_con.suppress_next_text = false;
        if (text[0] == '`' || text[0] == '~') return;
    }

    // Insert text at cursor
    u32 text_len = (u32)strlen(text);
    for (u32 i = 0; i < text_len; i++) {
        char ch = text[i];
        if (ch < 32 || ch > 126) continue;  // printable ASCII only
        if (s_con.input_len >= CON_INPUT_LEN - 1) break;

        // Insert at cursor position
        memmove(&s_con.input[s_con.cursor + 1],
                &s_con.input[s_con.cursor],
                s_con.input_len - s_con.cursor + 1);
        s_con.input[s_con.cursor] = ch;
        s_con.cursor++;
        s_con.input_len++;
    }
    s_con.input[s_con.input_len] = '\0';

    // Reset tab state on text input
    s_con.tab_active = false;
    s_con.tab_index = -1;
}

// --- Rendering ---

void qk_console_draw(f32 screen_w, f32 screen_h, f32 dt) {
    if (!s_con.initialized) return;

    // Animate slide
    f32 target = s_con.open ? 1.0f : 0.0f;
    if (s_con.slide_frac < target) {
        s_con.slide_frac += dt * CON_SLIDE_SPEED;
        if (s_con.slide_frac > 1.0f) s_con.slide_frac = 1.0f;
    } else if (s_con.slide_frac > target) {
        s_con.slide_frac -= dt * CON_SLIDE_SPEED;
        if (s_con.slide_frac < 0.0f) s_con.slide_frac = 0.0f;
    }

    // Zero cost when fully closed
    if (s_con.slide_frac <= 0.0f) return;

    f32 con_height = screen_h * 0.5f * s_con.slide_frac;
    f32 line_height = CON_FONT_SIZE + 2.0f;
    f32 padding = 6.0f;

    // Background
    qk_ui_draw_rect(0, 0, screen_w, con_height, CON_BG_COLOR);

    // Bottom border
    qk_ui_draw_rect(0, con_height - 2.0f, screen_w, 2.0f, CON_BORDER_COLOR);

    // Input line at bottom of console
    f32 input_y = con_height - line_height - padding;

    // Draw prompt ']'
    char prompt[2] = { CON_PROMPT_CHAR, '\0' };
    qk_ui_draw_text(padding, input_y, prompt, CON_FONT_SIZE, CON_BORDER_COLOR);

    // Draw input text
    f32 prompt_w = qk_ui_text_width(prompt, CON_FONT_SIZE);
    f32 text_x = padding + prompt_w + 2.0f;
    if (s_con.input_len > 0) {
        qk_ui_draw_text(text_x, input_y, s_con.input, CON_FONT_SIZE, CON_INPUT_COLOR);
    }

    // Blinking cursor
    s_con.cursor_blink_timer += dt;
    if (s_con.cursor_blink_timer > 1.0f) s_con.cursor_blink_timer -= 1.0f;
    if (s_con.open && s_con.cursor_blink_timer < 0.5f) {
        // Measure width up to cursor position
        char tmp[CON_INPUT_LEN];
        memcpy(tmp, s_con.input, s_con.cursor);
        tmp[s_con.cursor] = '\0';
        f32 cursor_x = text_x + qk_ui_text_width(tmp, CON_FONT_SIZE);
        qk_ui_draw_rect(cursor_x, input_y, 2.0f, CON_FONT_SIZE, CON_INPUT_COLOR);
    }

    // Separator line above input
    f32 sep_y = input_y - 4.0f;
    qk_ui_draw_rect(0, sep_y, screen_w, 1.0f, CON_BORDER_COLOR);

    // Draw scrollback lines (bottom-up)
    f32 y = sep_y - line_height;
    u32 visible_lines = 0;

    if (s_con.line_count > 0) {
        // Start from the newest visible line, adjusted by scroll offset
        i32 start_idx = (i32)s_con.line_head - 1 - s_con.scroll_offset;

        for (i32 i = start_idx; i >= 0 && y >= 0; i--, visible_lines++) {
            u32 ring_idx = (u32)i % CON_SCROLLBACK_LINES;
            con_line_t *line = &s_con.lines[ring_idx];
            if (line->text[0]) {
                qk_ui_draw_text(padding, y, line->text, CON_FONT_SIZE, line->color);
            }
            y -= line_height;
        }

        // If we started with high line_head, also render wrapped entries
        if (s_con.line_count >= CON_SCROLLBACK_LINES && start_idx < 0) {
            // wrapped case: line_head has wrapped around the ring buffer
            for (i32 i = (i32)CON_SCROLLBACK_LINES + start_idx; i >= (i32)s_con.line_head && y >= 0; i--, visible_lines++) {
                u32 ring_idx = (u32)i % CON_SCROLLBACK_LINES;
                con_line_t *line = &s_con.lines[ring_idx];
                if (line->text[0]) {
                    qk_ui_draw_text(padding, y, line->text, CON_FONT_SIZE, line->color);
                }
                y -= line_height;
            }
        }
    }

    QK_UNUSED(visible_lines);

    // Scroll indicator
    if (s_con.scroll_offset > 0) {
        char scroll_buf[32];
        snprintf(scroll_buf, sizeof(scroll_buf), "^ %d more ^", s_con.scroll_offset);
        f32 sw = qk_ui_text_width(scroll_buf, CON_FONT_SIZE);
        qk_ui_draw_text(screen_w - sw - padding, con_height - line_height - padding,
                         scroll_buf, CON_FONT_SIZE, CON_BORDER_COLOR);
    }
}
