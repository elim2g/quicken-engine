/*
 * QUICKEN Engine - Console Variable (Cvar) System
 *
 * Typed cvars with range clamping, change callbacks, and flags.
 * Modules cache qk_cvar_t* at registration time for zero-overhead reads.
 */

#ifndef QK_CVAR_H
#define QK_CVAR_H

#include "quicken.h"

// --- Types ---

typedef enum {
    QK_CVAR_FLOAT,
    QK_CVAR_INT,
    QK_CVAR_BOOL,
    QK_CVAR_STRING
} qk_cvar_type_t;

// Flags
enum {
    QK_CVAR_ARCHIVE  = (1 << 0),    // saved to config file
    QK_CVAR_READONLY = (1 << 1),    // cannot be changed at runtime
};

#define QK_CVAR_MAX_COUNT   512
#define QK_CVAR_NAME_LEN    64
#define QK_CVAR_STRING_LEN  256

typedef struct qk_cvar qk_cvar_t;

// Callback: called after value changes, receives the cvar that changed
typedef void (*qk_cvar_callback_t)(qk_cvar_t *cvar);

struct qk_cvar {
    char                name[QK_CVAR_NAME_LEN];
    qk_cvar_type_t      type;
    u32                 flags;
    union { f32 f; i32 i; bool b; char s[QK_CVAR_STRING_LEN]; } value;
    union { f32 f; i32 i; bool b; char s[QK_CVAR_STRING_LEN]; } default_value;
    f32                 min_val, max_val;
    bool                has_range;
    qk_cvar_callback_t  callback;
    bool                in_use;
};

// --- Lifecycle ---

void qk_cvar_init(void);
void qk_cvar_shutdown(void);

// --- Registration (returns cached pointer for direct reads) ---

qk_cvar_t *qk_cvar_register_float(const char *name, f32 default_val,
                                    f32 min_val, f32 max_val,
                                    u32 flags, qk_cvar_callback_t cb);
qk_cvar_t *qk_cvar_register_int(const char *name, i32 default_val,
                                  i32 min_val, i32 max_val,
                                  u32 flags, qk_cvar_callback_t cb);
qk_cvar_t *qk_cvar_register_bool(const char *name, bool default_val,
                                   u32 flags, qk_cvar_callback_t cb);
qk_cvar_t *qk_cvar_register_string(const char *name, const char *default_val,
                                     u32 flags, qk_cvar_callback_t cb);

// --- Lookup ---

qk_cvar_t *qk_cvar_find(const char *name);

// --- Setters (with clamping + callback) ---

bool qk_cvar_set_float(qk_cvar_t *cvar, f32 value);
bool qk_cvar_set_int(qk_cvar_t *cvar, i32 value);
bool qk_cvar_set_bool(qk_cvar_t *cvar, bool value);
bool qk_cvar_set_string(qk_cvar_t *cvar, const char *value);

// Set from string (auto-parses based on type)
bool qk_cvar_set_from_string(qk_cvar_t *cvar, const char *str);

// Reset to default
void qk_cvar_reset(qk_cvar_t *cvar);

// --- Iteration ---

u32         qk_cvar_count(void);
qk_cvar_t  *qk_cvar_get_all(void);   // returns static array

// Format value to string
void qk_cvar_to_string(const qk_cvar_t *cvar, char *buf, u32 buf_size);

#endif // QK_CVAR_H
