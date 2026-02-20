/*
 * QUICKEN Engine - Console Variable (Cvar) System
 *
 * Static array registry, typed setters with range clamping + callbacks.
 */

#include "core/qk_cvar.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static qk_cvar_t s_cvars[QK_CVAR_MAX_COUNT];
static u32 s_cvar_count;

void qk_cvar_init(void) {
    memset(s_cvars, 0, sizeof(s_cvars));
    s_cvar_count = 0;
}

void qk_cvar_shutdown(void) {
    s_cvar_count = 0;
}

// --- Internal Helpers ---

static qk_cvar_t *cvar_alloc(const char *name) {
    if (s_cvar_count >= QK_CVAR_MAX_COUNT) return NULL;

    // Check for duplicate
    for (u32 i = 0; i < s_cvar_count; i++) {
        if (s_cvars[i].in_use && strcmp(s_cvars[i].name, name) == 0) {
            return &s_cvars[i];
        }
    }

    qk_cvar_t *cv = &s_cvars[s_cvar_count++];
    memset(cv, 0, sizeof(*cv));
    cv->in_use = true;
    snprintf(cv->name, QK_CVAR_NAME_LEN, "%s", name);
    return cv;
}

// --- Registration ---

qk_cvar_t *qk_cvar_register_float(const char *name, f32 default_val,
                                    f32 min_val, f32 max_val,
                                    u32 flags, qk_cvar_callback_t cb) {
    qk_cvar_t *cv = cvar_alloc(name);
    if (!cv) return NULL;

    cv->type = QK_CVAR_FLOAT;
    cv->flags = flags;
    cv->value.f = default_val;
    cv->default_value.f = default_val;
    cv->min_val = min_val;
    cv->max_val = max_val;
    cv->has_range = (min_val < max_val);
    cv->callback = cb;
    return cv;
}

qk_cvar_t *qk_cvar_register_int(const char *name, i32 default_val,
                                  i32 min_val, i32 max_val,
                                  u32 flags, qk_cvar_callback_t cb) {
    qk_cvar_t *cv = cvar_alloc(name);
    if (!cv) return NULL;

    cv->type = QK_CVAR_INT;
    cv->flags = flags;
    cv->value.i = default_val;
    cv->default_value.i = default_val;
    cv->min_val = (f32)min_val;
    cv->max_val = (f32)max_val;
    cv->has_range = (min_val < max_val);
    cv->callback = cb;
    return cv;
}

qk_cvar_t *qk_cvar_register_bool(const char *name, bool default_val,
                                   u32 flags, qk_cvar_callback_t cb) {
    qk_cvar_t *cv = cvar_alloc(name);
    if (!cv) return NULL;

    cv->type = QK_CVAR_BOOL;
    cv->flags = flags;
    cv->value.b = default_val;
    cv->default_value.b = default_val;
    cv->has_range = false;
    cv->callback = cb;
    return cv;
}

qk_cvar_t *qk_cvar_register_string(const char *name, const char *default_val,
                                     u32 flags, qk_cvar_callback_t cb) {
    qk_cvar_t *cv = cvar_alloc(name);
    if (!cv) return NULL;

    cv->type = QK_CVAR_STRING;
    cv->flags = flags;
    snprintf(cv->value.s, QK_CVAR_STRING_LEN, "%s", default_val ? default_val : "");
    snprintf(cv->default_value.s, QK_CVAR_STRING_LEN, "%s", default_val ? default_val : "");
    cv->has_range = false;
    cv->callback = cb;
    return cv;
}

// --- Lookup ---

qk_cvar_t *qk_cvar_find(const char *name) {
    if (!name) return NULL;
    for (u32 i = 0; i < s_cvar_count; i++) {
        if (s_cvars[i].in_use && strcmp(s_cvars[i].name, name) == 0) {
            return &s_cvars[i];
        }
    }
    return NULL;
}

// --- Setters ---

bool qk_cvar_set_float(qk_cvar_t *cvar, f32 value) {
    if (!cvar || cvar->type != QK_CVAR_FLOAT) return false;
    if (cvar->flags & QK_CVAR_READONLY) return false;

    if (cvar->has_range) {
        if (value < cvar->min_val) value = cvar->min_val;
        if (value > cvar->max_val) value = cvar->max_val;
    }

    cvar->value.f = value;
    if (cvar->callback) cvar->callback(cvar);
    return true;
}

bool qk_cvar_set_int(qk_cvar_t *cvar, i32 value) {
    if (!cvar || cvar->type != QK_CVAR_INT) return false;
    if (cvar->flags & QK_CVAR_READONLY) return false;

    if (cvar->has_range) {
        if (value < (i32)cvar->min_val) value = (i32)cvar->min_val;
        if (value > (i32)cvar->max_val) value = (i32)cvar->max_val;
    }

    cvar->value.i = value;
    if (cvar->callback) cvar->callback(cvar);
    return true;
}

bool qk_cvar_set_bool(qk_cvar_t *cvar, bool value) {
    if (!cvar || cvar->type != QK_CVAR_BOOL) return false;
    if (cvar->flags & QK_CVAR_READONLY) return false;

    cvar->value.b = value;
    if (cvar->callback) cvar->callback(cvar);
    return true;
}

bool qk_cvar_set_string(qk_cvar_t *cvar, const char *value) {
    if (!cvar || cvar->type != QK_CVAR_STRING) return false;
    if (cvar->flags & QK_CVAR_READONLY) return false;

    snprintf(cvar->value.s, QK_CVAR_STRING_LEN, "%s", value ? value : "");
    if (cvar->callback) cvar->callback(cvar);
    return true;
}

bool qk_cvar_set_from_string(qk_cvar_t *cvar, const char *str) {
    if (!cvar || !str) return false;

    switch (cvar->type) {
    case QK_CVAR_FLOAT:
        return qk_cvar_set_float(cvar, (f32)atof(str));
    case QK_CVAR_INT:
        return qk_cvar_set_int(cvar, atoi(str));
    case QK_CVAR_BOOL:
        if (strcmp(str, "1") == 0 || strcmp(str, "true") == 0 || strcmp(str, "on") == 0)
            return qk_cvar_set_bool(cvar, true);
        else
            return qk_cvar_set_bool(cvar, false);
    case QK_CVAR_STRING:
        return qk_cvar_set_string(cvar, str);
    }
    return false;
}

void qk_cvar_reset(qk_cvar_t *cvar) {
    if (!cvar) return;
    if (cvar->flags & QK_CVAR_READONLY) return;

    switch (cvar->type) {
    case QK_CVAR_FLOAT:  cvar->value.f = cvar->default_value.f; break;
    case QK_CVAR_INT:    cvar->value.i = cvar->default_value.i; break;
    case QK_CVAR_BOOL:   cvar->value.b = cvar->default_value.b; break;
    case QK_CVAR_STRING:
        snprintf(cvar->value.s, QK_CVAR_STRING_LEN, "%s", cvar->default_value.s);
        break;
    }
    if (cvar->callback) cvar->callback(cvar);
}

// --- Iteration ---

u32 qk_cvar_count(void) {
    return s_cvar_count;
}

qk_cvar_t *qk_cvar_get_all(void) {
    return s_cvars;
}

// --- Format ---

void qk_cvar_to_string(const qk_cvar_t *cvar, char *buf, u32 buf_size) {
    if (!cvar || !buf || buf_size == 0) return;

    switch (cvar->type) {
    case QK_CVAR_FLOAT:
        snprintf(buf, buf_size, "%.4g", (double)cvar->value.f);
        break;
    case QK_CVAR_INT:
        snprintf(buf, buf_size, "%d", cvar->value.i);
        break;
    case QK_CVAR_BOOL:
        snprintf(buf, buf_size, "%s", cvar->value.b ? "true" : "false");
        break;
    case QK_CVAR_STRING:
        snprintf(buf, buf_size, "\"%s\"", cvar->value.s);
        break;
    }
}
