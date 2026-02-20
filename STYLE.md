# QUICKEN Engine — C11 Style Guide

This document defines the coding style for the QUICKEN engine. The goal is to
make the codebase readable to modern C developers without losing the engine's
architectural identity (module prefixes, snake_case, data-oriented design).

All rules below are C11-compatible. Nothing here requires C++ or compiler
extensions.

---

## Conventions We Keep

These look "oldschool" but are idiomatic C. Do NOT change them.

- **Module prefixes** (`r_`, `g_`, `p_`, `n_`, `qk_`) — this is namespacing in C.
- **`_t` suffix** on typedef'd structs/enums — standard convention.
- **`s_` prefix** for file-scoped statics — clear ownership signal.
- **`out_` prefix** for output parameters — self-documenting.
- **`u8`/`u32`/`f32` typedefs** — compact and universal in game engines.
- **Early-return error handling** with `qk_result_t` — modern and clean.
- **Designated initializers** (C99/C11) — use everywhere.
- **`do { } while(0)` macro idiom** — correct multi-statement macro form.
- **`QK_ASSERT` macro** — keep as-is.

---

## Rule 1: `//` for Line Comments

Use `//` for all single-line and inline comments. Reserve `/* */` only for
multi-line doc/block comments at the top of files and above functions.

```c
// GOOD
f32 sin_pitch = p_sinf(pitch * P_DEG2RAD);  // sin of pitch angle

// GOOD — file header block comment
/*
 * QUICKEN Engine — Arena Allocator
 *
 * Simple bump allocator with 16-byte alignment.
 */

// BAD
f32 sin_pitch = p_sinf(pitch * P_DEG2RAD);  /* sin of pitch angle */
```

## Rule 2: Short Section Dividers

Replace verbose `/* ---- Section ---- */` dividers with `// ---` style.

```c
// GOOD
// --- Solid Rectangle ---

// BAD
/* ---- Solid Rectangle ---- */
```

## Rule 3: Spell Out Local Variable Names

No abbreviated locals in non-trivial code. The reader should not need a lookup
table to understand what a variable holds.

```c
// GOOD
f32 sin_pitch = p_sinf(pitch * P_DEG2RAD);
f32 cos_pitch = p_cosf(pitch * P_DEG2RAD);
f32 sin_yaw   = p_sinf(yaw * P_DEG2RAD);
f32 cos_yaw   = p_cosf(yaw * P_DEG2RAD);

// BAD
f32 sp = p_sinf(pitch * P_DEG2RAD);
f32 cp = p_cosf(pitch * P_DEG2RAD);
f32 sy = p_sinf(yaw * P_DEG2RAD);
f32 cy = p_cosf(yaw * P_DEG2RAD);
```

**Exceptions** — these short names are universally understood and fine to use:
- `i`, `j`, `k` — simple loop indices
- `x`, `y`, `z`, `w` — coordinate/vector components
- `dt` — delta time
- `a`, `b` — two things being compared/combined (e.g. `vec3_add(a, b)`)
- `n` — count, when obvious from context
- `t` — interpolation parameter (0..1)

## Rule 4: `static const` for Typed Constants

Use `static const` instead of `#define` for typed numeric constants (floats,
sizes with known types). This gives type safety and debugger visibility.

```c
// GOOD
static const f32 P_PI           = 3.14159265358979323846f;
static const f32 P_2PI          = 6.28318530717958647692f;
static const f32 P_DEG2RAD      = P_PI / 180.0f;
static const f32 P_CLIP_EPSILON = 0.001f;

// BAD
#define P_PI            3.14159265358979323846f
#define P_2PI           6.28318530717958647692f
#define P_DEG2RAD       (P_PI / 180.0f)
#define P_CLIP_EPSILON  0.001f
```

**Keep `#define` for:**
- Array sizes used in struct/array declarations (e.g. `R_MAX_TEXTURES`)
- Conditional compilation flags (e.g. `QK_PLATFORM_WINDOWS`)
- Macros with logic (e.g. `QK_ASSERT`)

## Rule 5: Anonymous `enum` for Related Integer Groups

When you have a group of related integer constants, prefer anonymous `enum`
over multiple `#define`s. Debuggers can show the symbolic name.

```c
// GOOD
enum {
    R_MEMORY_POOL_DEVICE_LOCAL = 0,
    R_MEMORY_POOL_HOST_VISIBLE,
    R_MEMORY_POOL_COUNT
};

// BAD
#define R_MEMORY_POOL_DEVICE_LOCAL  0
#define R_MEMORY_POOL_HOST_VISIBLE  1
#define R_MEMORY_POOL_COUNT         2
```

Note: array-size constants like `R_MAX_TEXTURES 256` that are used in
declarations should stay as `#define` since C11 requires constant expressions
for array sizes.

## Rule 6: `_Static_assert` for Layout Assumptions

Use C11 `_Static_assert` to verify struct sizes and field offsets when the
layout matters (GPU UBOs, network packets, memory-mapped structures).

```c
_Static_assert(sizeof(r_view_uniforms_t) == 80,
               "UBO struct size changed — update descriptor");
_Static_assert(sizeof(n_packet_header_t) == 8,
               "packet header must be exactly 8 bytes");
```

## Rule 7: Declare Variables at First Use

Declare variables at the point of first use, not at the top of the function.
This is valid C99+ and makes it obvious where a variable's lifetime begins.

```c
// GOOD
void some_function(u32 count) {
    // ... setup code ...

    for (u32 i = 0; i < count; i++) {
        f32 fraction = (f32)i / (f32)count;
        // ...
    }
}

// BAD
void some_function(u32 count) {
    u32 i;
    f32 fraction;

    // ... 20 lines of setup ...

    for (i = 0; i < count; i++) {
        fraction = (f32)i / (f32)count;
        // ...
    }
}
```

## Rule 8: Designated Initializers for All Struct Construction

Use designated initializers consistently when constructing structs. This is
self-documenting and resistant to field reordering.

```c
// GOOD
qk_ui_quad_t quad = {
    .x = x, .y = y, .w = w, .h = h,
    .u0 = 0.0f, .v0 = 0.0f, .u1 = 1.0f, .v1 = 1.0f,
    .color = color_rgba,
    .texture_id = 0,
};

// BAD — field-by-field assignment when initializer would work
qk_ui_quad_t quad = {0};
quad.x = x;
quad.y = y;
quad.w = w;
quad.h = h;
quad.u0 = 0.0f;
quad.v0 = 0.0f;
quad.u1 = 1.0f;
quad.v1 = 1.0f;
quad.color = color_rgba;
quad.texture_id = 0;
```

---

## Summary

| #   | Rule                                         | Applies to                    |
| --- | -------------------------------------------- | ----------------------------- |
| 1   | `//` for line comments                       | All files                     |
| 2   | `// ---` section dividers                    | All files                     |
| 3   | Spell out local variable names               | Non-trivial code              |
| 4   | `static const` for typed numeric constants   | Floats, config values         |
| 5   | Anonymous `enum` for integer constant groups | Related integer groups        |
| 6   | `_Static_assert` for struct layout           | Structs with ABI requirements |
| 7   | Declare variables at first use               | All functions                 |
| 8   | Designated initializers for struct init      | All struct construction       |
