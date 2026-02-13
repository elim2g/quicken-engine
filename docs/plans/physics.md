# Physics Module Implementation Plan

Vertical slice scope: ground movement, air movement with strafejumping, jumping, gravity, friction, and collision against brush geometry (SlideMove/StepSlideMove) at a fixed 128Hz tick rate with cross-platform determinism.

---

## 1. Collision Model Representation

The engine core parses `.map` files and hands us collision geometry. We store it in a physics-internal representation optimized for tracing.

### Brush Representation

A brush is a convex hull defined by the intersection of half-spaces (planes). This is the native Quake .map format. We store brushes as arrays of planes -- no need to compute vertices for collision; plane-based clipping is faster and more robust.

```c
/* p_collision.h */

typedef struct {
    f32 x, y, z;
} vec3_t;

typedef struct {
    vec3_t normal;   /* unit normal pointing OUT of solid */
    f32    dist;     /* dot(normal, point_on_plane) */
} p_plane_t;

typedef struct {
    p_plane_t *planes;      /* array of bounding planes */
    u32        plane_count;
    vec3_t     mins;         /* AABB for broadphase */
    vec3_t     maxs;
} p_brush_t;

typedef struct {
    p_brush_t *brushes;
    u32        brush_count;
} p_collision_model_t;
```

The AABB (`mins`/`maxs`) per brush is precomputed at load time for broadphase rejection. It is the tightest axis-aligned box enclosing all plane intersections of that brush.

### Loading from Engine Core

The engine core provides us with a flat array of brushes after parsing the `.map` file. Our init function takes ownership of the collision model:

```c
typedef struct {
    const p_collision_model_t *collision_model;
    /* spatial acceleration structure built at init */
    void *spatial_index;  /* opaque, internal type */
} p_world_t;
```

---

## 2. Spatial Acceleration Structure

### Evaluation of Options

**BSP tree (id Software approach):**
- Pros: proven for Quake geometry, excellent worst-case performance, ordered front-to-back traversal.
- Cons: build complexity, splitting brushes across partitions increases brush count, rigid structure.

**BVH (Bounding Volume Hierarchy):**
- Pros: natural fit for convex objects (no splitting), simple to build via SAH or median split, cache-friendly with flat array layout, works well with AABB broadphase we already need.
- Cons: slightly worse worst-case than BSP for degenerate camera-inside-many-boxes cases.

**Spatial hashing / uniform grid:**
- Pros: trivial to implement, O(1) cell lookup.
- Cons: poor for non-uniform brush sizes (hallways vs large rooms), wastes memory on empty cells, bad for long rays.

**Brute force:**
- Pros: zero overhead, simplest possible code.
- Cons: scales as O(n) per trace. A typical Quake 3 duel map has 500-2000 brushes. At 128Hz with multiple traces per player per tick (SlideMove does up to 4-5 traces, plus ground check, etc.), this is roughly 10 traces * 1500 brushes = 15,000 brush tests per player per tick. On modern hardware with AABB broadphase rejection, this is actually fine for small maps.

### Decision: BVH, with brute-force as initial implementation

For the vertical slice, start with **brute force + AABB broadphase** per brush. This is trivial to implement, trivial to verify, and fast enough for the vertical slice (single player, one map).

Follow up with a **BVH** for production. A BVH with SAH (Surface Area Heuristic) built at map load is the right long-term structure:
- No brush splitting (unlike BSP).
- Flat array layout (unlike pointer-based BSP) for cache performance.
- Natural AABB tests at each node, which SIMD accelerates trivially.
- Rebuild is not needed at runtime (static world).

BSP is rejected unless profiling reveals a specific traversal pattern that BVH handles poorly. For Quake-scale geometry with convex brushes, BVH is simpler and equally fast.

### BVH Data Structure (for production, not vertical slice)

```c
typedef struct {
    vec3_t mins;
    vec3_t maxs;
    union {
        struct {
            u32 left_child;   /* index into node array */
            u32 right_child;
        };
        struct {
            u32 first_brush;  /* index into brush array */
            u32 brush_count;  /* leaf: brush_count > 0 */
        };
    };
} p_bvh_node_t;

typedef struct {
    p_bvh_node_t *nodes;
    u32            node_count;
} p_bvh_t;
```

Leaf nodes have `brush_count > 0`. Internal nodes have `brush_count == 0`. Flat array, no pointers, cache-friendly depth-first layout.

---

## 3. Trace Implementation

A trace sweeps an AABB from `start` to `end` through the world and returns the first collision. This is the fundamental operation of the physics module -- everything else is built on top of it.

### Trace Result

```c
typedef struct {
    f32    fraction;      /* 0.0 = start in solid, 1.0 = no hit */
    vec3_t end_pos;       /* final position (start + fraction * delta) */
    vec3_t plane_normal;  /* normal of the surface hit */
    f32    plane_dist;    /* distance of the hit plane */
    bool   start_solid;   /* true if start position was inside a brush */
    bool   all_solid;     /* true if entire trace was inside solid */
    i32    brush_index;   /* which brush was hit (-1 if none) */
} p_trace_result_t;
```

### Trace Against a Single Brush

This is the Quake `CM_TraceThroughBrush` algorithm. Sweep an AABB against a convex hull defined by planes.

To trace a box (mins/maxs) against a brush, we expand each brush plane by the trace box extents (Minkowski sum). This converts the box trace into a ray trace against an expanded brush:

```c
/* For each brush plane, compute the offset based on trace box extents */
static f32 plane_expand_dist(const p_plane_t *plane, vec3_t mins, vec3_t maxs) {
    f32 expand = 0.0f;
    /* For each axis: if normal component is positive, use mins; else maxs.
       This is the support point of -normal against the AABB. */
    expand += (plane->normal.x >= 0.0f) ? -mins.x : -maxs.x;
    expand += (plane->normal.y >= 0.0f) ? -mins.y : -maxs.y;
    expand += (plane->normal.z >= 0.0f) ? -mins.z : -maxs.z;
    /* Negate because we expanded the brush outward */
    return expand;
}
```

Wait -- the standard Quake approach works slightly differently. The correct Minkowski expansion for each plane:

```
For plane with normal N and dist D:
  offset = dot(N, support(-N, box))
  expanded_dist = D - offset

where support(-N, box) returns the point of the AABB closest
to the plane along -N direction.
```

Which simplifies to exactly:
```
For each axis i:
  if N[i] < 0: offset[i] = maxs[i]
  else:        offset[i] = mins[i]
expanded_dist = D - dot(N, offset)
```

The trace through a single brush (core algorithm):

```c
p_trace_result_t p_trace_brush(const p_brush_t *brush,
                               vec3_t start, vec3_t end,
                               vec3_t mins, vec3_t maxs) {
    p_trace_result_t result = {0};
    result.fraction = 1.0f;

    f32 enter_frac = -1.0f;
    f32 leave_frac = 1.0f;
    const p_plane_t *clip_plane = NULL;

    bool starts_out = false;
    bool gets_out = false;

    vec3_t delta = vec3_sub(end, start);

    for (u32 i = 0; i < brush->plane_count; i++) {
        const p_plane_t *plane = &brush->planes[i];

        /* Expand plane by AABB extents (Minkowski sum) */
        f32 expand = 0.0f;
        expand += (plane->normal.x >= 0.0f) ? mins.x : maxs.x;
        expand += (plane->normal.y >= 0.0f) ? mins.y : maxs.y;
        expand += (plane->normal.z >= 0.0f) ? mins.z : maxs.z;
        f32 dist = plane->dist - expand;

        f32 d_start = vec3_dot(start, plane->normal) - dist;
        f32 d_end   = vec3_dot(end,   plane->normal) - dist;

        if (d_start > 0.0f) starts_out = true;
        if (d_end > 0.0f)   gets_out = true;

        /* Both in front: completely outside this plane */
        if (d_start > 0.0f && d_end >= d_start) {
            /* Ray moves away from or along the plane -- no intersection */
            return result;  /* fraction = 1.0, no hit */
        }

        /* Both behind: inside this half-space, continue */
        if (d_start <= 0.0f && d_end <= 0.0f) {
            continue;
        }

        /* Crosses the plane */
        f32 f;
        if (d_start > d_end) {
            /* Entering the brush */
            f = (d_start - TRACE_EPSILON) / (d_start - d_end);
            if (f < 0.0f) f = 0.0f;
            if (f > enter_frac) {
                enter_frac = f;
                clip_plane = plane;
            }
        } else {
            /* Leaving the brush */
            f = (d_start + TRACE_EPSILON) / (d_start - d_end);
            if (f > 1.0f) f = 1.0f;
            if (f < leave_frac) {
                leave_frac = f;
            }
        }
    }

    if (!starts_out) {
        result.start_solid = true;
        if (!gets_out) {
            result.all_solid = true;
            result.fraction = 0.0f;
        }
        return result;
    }

    if (enter_frac < leave_frac) {
        if (enter_frac > -1.0f && enter_frac < result.fraction) {
            if (enter_frac < 0.0f) enter_frac = 0.0f;
            result.fraction = enter_frac;
            result.plane_normal = clip_plane->normal;
            result.plane_dist = clip_plane->dist;
            result.end_pos = vec3_add(start, vec3_scale(delta, enter_frac));
        }
    }

    return result;
}
```

`TRACE_EPSILON` is `0.03125f` (1/32), matching Quake 3.

### World Trace (All Brushes)

```c
p_trace_result_t p_trace_world(const p_world_t *world,
                                vec3_t start, vec3_t end,
                                vec3_t mins, vec3_t maxs) {
    p_trace_result_t best = {0};
    best.fraction = 1.0f;
    best.end_pos = end;

    /* Compute the swept AABB for broadphase */
    vec3_t swept_mins, swept_maxs;
    p_compute_swept_aabb(start, end, mins, maxs, &swept_mins, &swept_maxs);

    for (u32 i = 0; i < world->collision_model->brush_count; i++) {
        const p_brush_t *brush = &world->collision_model->brushes[i];

        /* Broadphase: AABB overlap test */
        if (!p_aabb_overlap(swept_mins, swept_maxs, brush->mins, brush->maxs)) {
            continue;
        }

        p_trace_result_t result = p_trace_brush(brush, start, end, mins, maxs);

        if (result.all_solid) {
            return result;
        }

        if (result.fraction < best.fraction) {
            best = result;
            best.brush_index = (i32)i;
        }
    }

    /* Compute final end position */
    best.end_pos.x = start.x + best.fraction * (end.x - start.x);
    best.end_pos.y = start.y + best.fraction * (end.y - start.y);
    best.end_pos.z = start.z + best.fraction * (end.z - start.z);

    return best;
}
```

### Ground Check Trace

A special case: short downward trace to determine if the player is on ground.

```c
bool p_check_ground(const p_world_t *world, vec3_t origin,
                    vec3_t mins, vec3_t maxs, vec3_t *ground_normal) {
    vec3_t down = { origin.x, origin.y, origin.z - 0.25f };
    p_trace_result_t trace = p_trace_world(world, origin, down, mins, maxs);

    if (trace.fraction == 1.0f) return false;
    if (trace.plane_normal.z < PM_MIN_WALK_NORMAL) return false;

    if (ground_normal) *ground_normal = trace.plane_normal;
    return true;
}
```

`PM_MIN_WALK_NORMAL` is `0.7f` (approximately 45 degrees), matching Q3.

---

## 4. Movement System

### Input Command

The engine provides us with a user command each tick:

```c
typedef struct {
    f32 forward_move;    /* -1.0 to 1.0, from +forward/-back keys */
    f32 side_move;       /* -1.0 to 1.0, from +moveright/+moveleft keys */
    f32 up_move;         /* jump button state as 1.0 or 0.0 */
    f32 view_angles[3];  /* pitch, yaw, roll in degrees */
    u32 buttons;         /* bitmask: jump, crouch, etc. */
    u32 server_time;     /* timestamp of this command in ms */
} p_usercmd_t;

#define BUTTON_JUMP   (1 << 0)
#define BUTTON_CROUCH (1 << 1)
#define BUTTON_ATTACK (1 << 2)
```

### Player State

```c
typedef struct {
    vec3_t origin;
    vec3_t velocity;

    /* Bounding box (changes with crouch) */
    vec3_t mins;
    vec3_t maxs;

    /* Ground state */
    bool   on_ground;
    vec3_t ground_normal;

    /* Jump tracking */
    bool   jump_held;   /* was jump held last frame? (for edge detection) */

    /* Movement parameters (allow per-player tuning if needed) */
    f32    max_speed;
    f32    gravity;

    /* Time tracking for fixed timestep */
    u32    command_time;  /* last processed command time in ms */
} p_player_state_t;
```

### Movement Constants

```c
#define PM_GROUND_ACCEL     10.0f     /* ground acceleration factor */
#define PM_AIR_ACCEL        1.0f      /* air acceleration factor */
#define PM_FLY_ACCEL        8.0f      /* noclip/spectator */
#define PM_GROUND_FRICTION  6.0f      /* ground friction factor */
#define PM_MAX_SPEED        320.0f    /* max horizontal wish speed (units/sec) */
#define PM_JUMP_VELOCITY    270.0f    /* vertical velocity added on jump */
#define PM_GRAVITY          800.0f    /* gravity in units/sec^2 */
#define PM_STEP_HEIGHT      18.0f     /* max step-up height */
#define PM_OVERCLIP         1.001f    /* slight push off collision planes */
#define PM_STOP_SPEED       100.0f    /* below this speed, friction applies at this value */
#define PM_MIN_WALK_NORMAL  0.7f      /* steeper than ~45 deg = not walkable */
#define PM_TICK_RATE        128       /* physics ticks per second */
#define PM_TICK_MSEC        (1000 / PM_TICK_RATE)  /* 7 or 8ms per tick */
#define PM_TICK_DT          (1.0f / (f32)PM_TICK_RATE) /* seconds per tick */
#define TRACE_EPSILON       0.03125f  /* 1/32, collision epsilon */
#define CLIP_EPSILON         0.001f   /* velocity clipping epsilon */
```

Note on `PM_TICK_MSEC`: Q3 used `1000/125 = 8`. At 128Hz, `1000/128 = 7.8125`. Since we use integer millisecond command times (matching Q3's server time model), each tick is either 7 or 8 ms. The accumulator handles this correctly by consuming fixed steps and tracking fractional time in the command time itself. See section 7.

### PM_Move -- Top-Level Movement Function

This is called once per tick with a user command:

```c
void p_move(p_player_state_t *ps, const p_usercmd_t *cmd,
            const p_world_t *world) {

    f32 dt = PM_TICK_DT;  /* fixed timestep */

    /* 1. Compute wish direction from input + view angles */
    vec3_t forward, right, up;
    p_angle_vectors(cmd->view_angles, &forward, &right, &up);

    vec3_t wish_dir;
    wish_dir.x = forward.x * cmd->forward_move + right.x * cmd->side_move;
    wish_dir.y = forward.y * cmd->forward_move + right.y * cmd->side_move;
    wish_dir.z = 0.0f;  /* no vertical component from input */

    f32 wish_speed = vec3_length(wish_dir);
    if (wish_speed > 0.0001f) {
        wish_dir = vec3_scale(wish_dir, 1.0f / wish_speed);
        wish_speed *= PM_MAX_SPEED;
        if (wish_speed > PM_MAX_SPEED) wish_speed = PM_MAX_SPEED;
    } else {
        wish_dir = (vec3_t){0};
        wish_speed = 0.0f;
    }

    /* 2. Check ground */
    p_categorize_position(ps, world);

    /* 3. Jump check */
    p_check_jump(ps, cmd);

    /* 4. Apply friction (ground only) */
    if (ps->on_ground) {
        p_apply_friction(ps, dt);
    }

    /* 5. Accelerate */
    if (ps->on_ground) {
        p_accelerate(ps, wish_dir, wish_speed, PM_GROUND_ACCEL, dt);
    } else {
        p_air_accelerate(ps, wish_dir, wish_speed, PM_AIR_ACCEL, dt);
    }

    /* 6. Apply gravity (air only) */
    if (!ps->on_ground) {
        ps->velocity.z -= ps->gravity * dt;
    }

    /* 7. Move and collide */
    if (ps->on_ground) {
        p_step_slide_move(ps, world, dt);
    } else {
        p_slide_move(ps, world, dt, 4);
    }

    /* 8. Re-check ground after move */
    p_categorize_position(ps, world);
}
```

### Categorize Position

```c
static void p_categorize_position(p_player_state_t *ps,
                                  const p_world_t *world) {
    /* Trace 0.25 units down from current position */
    vec3_t point = ps->origin;
    point.z -= 0.25f;

    p_trace_result_t trace = p_trace_world(world, ps->origin, point,
                                            ps->mins, ps->maxs);

    if (trace.fraction == 1.0f ||
        trace.plane_normal.z < PM_MIN_WALK_NORMAL) {
        ps->on_ground = false;
        ps->ground_normal = (vec3_t){0.0f, 0.0f, 0.0f};
    } else {
        ps->on_ground = true;
        ps->ground_normal = trace.plane_normal;
    }
}
```

### Jump Check

```c
static void p_check_jump(p_player_state_t *ps, const p_usercmd_t *cmd) {
    /* Only jump on button press edge (not hold) */
    if (cmd->buttons & BUTTON_JUMP) {
        if (ps->jump_held) return;  /* still holding from last frame */
        ps->jump_held = true;

        if (!ps->on_ground) return;  /* can't jump in air */

        ps->on_ground = false;
        ps->velocity.z = PM_JUMP_VELOCITY;

        /* Q3 behavior: do NOT apply friction on the jump frame.
           The jump happens BEFORE friction, so we set on_ground = false
           which skips friction in step 4 above. Actually, looking at
           the call order: jump check is step 3, friction is step 4.
           By setting on_ground = false here, friction is skipped. This
           is correct Q3 behavior and is essential for consistent
           strafejump speeds. */
    } else {
        ps->jump_held = false;
    }
}
```

---

## 5. Strafejumping Implementation

This is the most important section. The speed gain from strafejumping comes from how `PM_AirAccelerate` interacts with the velocity cap check.

### Ground Acceleration (PM_Accelerate)

Standard Quake 3 ground acceleration. Projects current velocity onto wish direction, then accelerates up to max speed:

```c
static void p_accelerate(p_player_state_t *ps, vec3_t wish_dir,
                          f32 wish_speed, f32 accel, f32 dt) {
    /* Current speed projected onto wish direction */
    f32 current_speed = vec3_dot(ps->velocity, wish_dir);

    /* How much we can accelerate before hitting wish_speed */
    f32 add_speed = wish_speed - current_speed;
    if (add_speed <= 0.0f) return;

    /* Acceleration this frame */
    f32 accel_speed = accel * wish_speed * dt;
    if (accel_speed > add_speed) {
        accel_speed = add_speed;
    }

    ps->velocity.x += accel_speed * wish_dir.x;
    ps->velocity.y += accel_speed * wish_dir.y;
    ps->velocity.z += accel_speed * wish_dir.z;
}
```

### Air Acceleration (PM_AirAccelerate) -- The Heart of Strafejumping

This is where the magic happens. The critical difference from ground acceleration is the `wish_speed` cap to 30 units/sec in the add_speed calculation:

```c
static void p_air_accelerate(p_player_state_t *ps, vec3_t wish_dir,
                              f32 wish_speed, f32 accel, f32 dt) {
    /* THIS IS THE KEY: cap wish_speed to 30 for the projection test */
    f32 wish_speed_capped = wish_speed;
    if (wish_speed_capped > 30.0f) {
        wish_speed_capped = 30.0f;
    }

    /* Current speed projected onto wish direction */
    f32 current_speed = vec3_dot(ps->velocity, wish_dir);

    /* How much we can add -- uses CAPPED wish speed for the limit */
    f32 add_speed = wish_speed_capped - current_speed;
    if (add_speed <= 0.0f) return;

    /* But the acceleration magnitude uses the ORIGINAL wish_speed */
    f32 accel_speed = accel * wish_speed * dt;
    if (accel_speed > add_speed) {
        accel_speed = add_speed;
    }

    ps->velocity.x += accel_speed * wish_dir.x;
    ps->velocity.y += accel_speed * wish_dir.y;
    ps->velocity.z += accel_speed * wish_dir.z;
}
```

### Why This Produces Strafejumping

Mathematical explanation for the implementation record:

When the player holds W+A (forward + strafe left), the wish direction is 45 degrees left of the view direction. If the player also rotates their view to the right at the correct rate, they can keep the angle between wish_dir and velocity_dir at the "sweet spot."

The speed gain works because:

1. `current_speed = dot(velocity, wish_dir)` -- this is the projection of velocity onto the wish direction. When wish_dir diverges from the velocity vector, this projection is LESS than the actual speed magnitude.

2. `add_speed = 30.0 - current_speed` -- since current_speed (the projection) is less than the actual speed, and the cap is only 30, there is almost always room to add some speed. Without the cap at 30, once you exceed `wish_speed` (320), `add_speed` would go to zero because the projection would eventually exceed 320. The 30 cap means the check passes as long as the projection onto wish_dir is less than 30, which is almost always true when the angle between velocity and wish_dir is large enough.

3. `accel_speed = accel * wish_speed * dt = 1.0 * 320.0 * dt` -- this is the actual acceleration applied, which uses the FULL 320 (not the capped 30). But it is clamped to `add_speed`.

4. The acceleration is added in the wish_dir direction. Since wish_dir is angled away from velocity, this adds a perpendicular component that increases the magnitude of the velocity vector. The velocity vector curves toward wish_dir, but the player is also turning their view, which moves wish_dir, so the velocity keeps curving and accelerating.

The optimal strafe angle (the angle between velocity and wish_dir that maximizes speed gain per frame) is:

```
theta_optimal = acos(30 / |velocity|)
```

At 320 u/s: `acos(30/320) = ~84.6 degrees`
At 400 u/s: `acos(30/400) = ~85.7 degrees`
At 600 u/s: `acos(30/600) = ~87.1 degrees`

As speed increases, the optimal angle approaches 90 degrees, making it progressively harder to gain additional speed. This is the natural "difficulty curve" of strafejumping.

### Air Acceleration Value 30 -- The Magic Number

The `30.0f` cap is hardcoded in the original Q3 source (`PM_AirMove` in `bg_pmove.c`). It is not `pm_airaccelerate` (which is 1). It is the `wishspeed` clamped to 30 before the `PM_Accelerate` call. We replicate this exactly.

```c
#define PM_AIR_WISHSPEED_CAP  30.0f
```

### Ground Friction

```c
static void p_apply_friction(p_player_state_t *ps, f32 dt) {
    f32 speed = vec3_length(ps->velocity);
    if (speed < 0.1f) {
        ps->velocity.x = 0.0f;
        ps->velocity.y = 0.0f;
        /* preserve vertical velocity */
        return;
    }

    /* Q3 friction: use max(speed, PM_STOP_SPEED) for the friction term.
       This means slow movement gets MORE friction relative to speed,
       bringing it to a stop faster. */
    f32 control = (speed < PM_STOP_SPEED) ? PM_STOP_SPEED : speed;
    f32 drop = control * PM_GROUND_FRICTION * dt;

    f32 new_speed = speed - drop;
    if (new_speed < 0.0f) new_speed = 0.0f;

    f32 scale = new_speed / speed;
    ps->velocity.x *= scale;
    ps->velocity.y *= scale;
    /* z velocity preserved -- important for ramp interactions */
}
```

---

## 6. SlideMove and StepSlideMove

### ClipVelocity

Reflects velocity off a collision plane, with a slight push-off factor (`PM_OVERCLIP`):

```c
static vec3_t p_clip_velocity(vec3_t velocity, vec3_t normal, f32 overbounce) {
    f32 backoff = vec3_dot(velocity, normal) * overbounce;

    vec3_t result;
    result.x = velocity.x - normal.x * backoff;
    result.y = velocity.y - normal.y * backoff;
    result.z = velocity.z - normal.z * backoff;

    /* Ensure no tiny residual into-plane velocity (numerical cleanup) */
    f32 adjust = vec3_dot(result, normal);
    if (adjust < 0.0f) {
        result.x -= normal.x * adjust;
        result.y -= normal.y * adjust;
        result.z -= normal.z * adjust;
    }

    return result;
}
```

### SlideMove

The core collision response loop. Attempts to move the player along their velocity, clipping against collision planes up to `max_bumps` times:

```c
#define MAX_CLIP_PLANES 5

bool p_slide_move(p_player_state_t *ps, const p_world_t *world,
                  f32 dt, i32 max_bumps) {
    vec3_t planes[MAX_CLIP_PLANES];
    i32 num_planes = 0;
    vec3_t primal_velocity = ps->velocity;  /* save original */
    f32 time_left = dt;

    for (i32 bump = 0; bump < max_bumps; bump++) {
        /* Where we want to go this sub-step */
        vec3_t end;
        end.x = ps->origin.x + ps->velocity.x * time_left;
        end.y = ps->origin.y + ps->velocity.y * time_left;
        end.z = ps->origin.z + ps->velocity.z * time_left;

        p_trace_result_t trace = p_trace_world(world, ps->origin, end,
                                                ps->mins, ps->maxs);

        if (trace.all_solid) {
            /* Stuck in solid -- kill velocity */
            ps->velocity = (vec3_t){0};
            return true;
        }

        if (trace.fraction > 0.0f) {
            /* Move to the contact point */
            ps->origin = trace.end_pos;
        }

        if (trace.fraction == 1.0f) {
            break;  /* Moved the full distance without hitting anything */
        }

        /* Reduce remaining time */
        time_left -= time_left * trace.fraction;

        /* Record this clip plane */
        if (num_planes >= MAX_CLIP_PLANES) {
            ps->velocity = (vec3_t){0};
            return true;
        }
        planes[num_planes] = trace.plane_normal;
        num_planes++;

        /* Try to clip velocity against all accumulated planes */
        i32 i, j;
        for (i = 0; i < num_planes; i++) {
            vec3_t clipped = p_clip_velocity(ps->velocity, planes[i],
                                              PM_OVERCLIP);

            /* Check that the clipped velocity doesn't re-enter any
               previously recorded plane */
            for (j = 0; j < num_planes; j++) {
                if (j == i) continue;
                if (vec3_dot(clipped, planes[j]) < 0.0f) {
                    break;  /* clips into another plane */
                }
            }

            if (j == num_planes) {
                /* This clip works against all planes */
                ps->velocity = clipped;
                break;
            }
        }

        if (i == num_planes) {
            /* Could not find a valid clip against a single plane.
               Try sliding along the crease between the last two planes. */
            if (num_planes == 2) {
                vec3_t dir = vec3_cross(planes[0], planes[1]);
                dir = vec3_normalize(dir);
                f32 d = vec3_dot(dir, ps->velocity);
                ps->velocity = vec3_scale(dir, d);
            } else {
                /* Cornered by 3+ planes -- stop */
                ps->velocity = (vec3_t){0};
                return true;
            }
        }

        /* Don't accelerate past original speed (no speed gain from
           bouncing off walls) */
        if (vec3_dot(ps->velocity, primal_velocity) <= 0.0f) {
            ps->velocity = (vec3_t){0};
            return true;
        }
    }

    return (num_planes > 0);
}
```

### StepSlideMove

Handles stepping up stairs/ledges. This is used when the player is on the ground. The algorithm:

1. Try a normal SlideMove.
2. If that got blocked, try stepping up by `PM_STEP_HEIGHT`, doing the SlideMove from there, then stepping back down.
3. Use whichever attempt got further.

```c
void p_step_slide_move(p_player_state_t *ps, const p_world_t *world,
                       f32 dt) {
    /* Save starting state */
    vec3_t start_origin = ps->origin;
    vec3_t start_velocity = ps->velocity;

    /* First, try a normal slide move */
    bool hit_wall = p_slide_move(ps, world, dt, 4);

    /* Save the result of the non-stepped move */
    vec3_t down_origin = ps->origin;
    vec3_t down_velocity = ps->velocity;

    /* Restore and try the stepped move */
    ps->origin = start_origin;
    ps->velocity = start_velocity;

    /* Trace up by step height */
    vec3_t up = start_origin;
    up.z += PM_STEP_HEIGHT;
    p_trace_result_t trace = p_trace_world(world, ps->origin, up,
                                            ps->mins, ps->maxs);
    if (!trace.all_solid) {
        ps->origin = trace.end_pos;
    }

    /* Slide move from the stepped-up position */
    p_slide_move(ps, world, dt, 4);

    /* Step back down */
    vec3_t down = ps->origin;
    down.z -= PM_STEP_HEIGHT;
    trace = p_trace_world(world, ps->origin, down, ps->mins, ps->maxs);
    if (!trace.all_solid) {
        ps->origin = trace.end_pos;
    }

    /* Snap to ground if we landed on a walkable surface */
    if (trace.fraction < 1.0f && trace.plane_normal.z >= PM_MIN_WALK_NORMAL) {
        /* Use the stepped result if it got further horizontally */
        f32 step_dist_sq = (ps->origin.x - start_origin.x) *
                           (ps->origin.x - start_origin.x) +
                           (ps->origin.y - start_origin.y) *
                           (ps->origin.y - start_origin.y);
        f32 down_dist_sq = (down_origin.x - start_origin.x) *
                           (down_origin.x - start_origin.x) +
                           (down_origin.y - start_origin.y) *
                           (down_origin.y - start_origin.y);

        if (step_dist_sq > down_dist_sq) {
            /* Stepped move was better */
            return;
        }
    }

    /* Non-stepped move was better (or stepped move didn't land) */
    ps->origin = down_origin;
    ps->velocity = down_velocity;
}
```

---

## 7. Fixed Timestep Integration

Physics runs at 128Hz. The engine's main loop calls us with real elapsed time; we consume it in fixed steps.

### Tick Rate and Integer Time

Q3 used integer millisecond timestamps for server/command time. We follow the same model. The tick duration at 128Hz is 7.8125ms, which is not an integer. We handle this the same way Q3 did: the server time advances by either 7 or 8 ms per tick, alternating to maintain the correct average rate.

Actually, the simpler and more correct approach for 128Hz: use a float-based accumulator that consumes exactly `1.0/128.0` seconds per tick, and track command time in milliseconds separately. The accumulator is purely internal. Command timestamps from the network use integer ms.

```c
typedef struct {
    f32 accumulator;       /* unprocessed time in seconds */
    u32 tick_count;        /* total physics ticks processed */
} p_time_state_t;

void p_update(p_time_state_t *ts, f32 frame_dt,
              p_player_state_t *ps, const p_usercmd_t *cmd,
              const p_world_t *world) {

    /* Clamp incoming dt to prevent spiral-of-death */
    if (frame_dt > 0.25f) frame_dt = 0.25f;

    ts->accumulator += frame_dt;

    while (ts->accumulator >= PM_TICK_DT) {
        p_move(ps, cmd, world);
        ts->accumulator -= PM_TICK_DT;
        ts->tick_count++;
    }

    /* The remaining accumulator (0 <= accumulator < PM_TICK_DT) is the
       interpolation fraction for the renderer:
       alpha = accumulator / PM_TICK_DT
       The renderer uses this to lerp between the previous and current
       physics state for smooth display. */
}
```

### Interpolation State for the Renderer

The physics module exposes previous and current state for interpolation:

```c
typedef struct {
    vec3_t prev_origin;
    vec3_t curr_origin;
    f32    alpha;  /* 0.0 to 1.0, interpolation fraction */
} p_render_state_t;

void p_get_render_state(const p_time_state_t *ts,
                        const p_player_state_t *ps,
                        p_render_state_t *out);
```

The game loop stores the previous origin before each `p_move` call and computes alpha from the remaining accumulator.

### Server Time Synchronization

For netcode (not in vertical slice, but the design must support it): the server runs at the same 128Hz and assigns integer tick numbers. Client prediction runs the same `p_move` for predicted ticks. Rollback replays from an authoritative snapshot. Because `p_move` is deterministic and uses a fixed timestep, identical inputs produce identical outputs. The accumulator state is per-client and is not networked.

---

## 8. Deterministic Math Considerations

### Compiler Settings (already enforced in premake5.lua)

- MSVC: `/fp:precise` -- no reassociation, no fused multiply-add by default, IEEE-754 compliant.
- GCC/Clang: no `-ffast-math`, no `-funsafe-math-optimizations`, no `-ffinite-math-only`. We add `-ffp-contract=off` to prevent FMA contraction, which can differ between compilers.

**Action item**: Add `-ffp-contract=off` to the GCC/Clang build flags in premake5.lua. This is currently missing and is required for determinism.

### Standard Library Functions to Avoid in Gameplay Paths

The following `<math.h>` functions have implementation-defined behavior that varies between MSVC and GCC/glibc:

| Function | Problem | Replacement |
|----------|---------|-------------|
| `sinf`, `cosf` | Results differ in last 1-3 ULP across implementations | Our own Taylor/polynomial or a portable library (e.g., cephes single-precision) |
| `sqrtf` | Generally IEEE-754 correct on both, BUT FMA contraction can affect intermediate results | Safe to use IF `-ffp-contract=off` is set; verify on both platforms |
| `atan2f` | Varies across implementations | Own implementation or table-based |
| `acosf`, `asinf` | Varies | Own implementation (but rarely needed in physics tick) |
| `powf`, `expf`, `logf` | Varies | Not needed in movement code |

For the vertical slice, the only transcendental functions we actually need in the physics tick are:

1. **`sqrtf`**: Used in `vec3_length` and `vec3_normalize`. This is the most critical. IEEE-754 mandates correctly-rounded `sqrt`, so `sqrtf` SHOULD be identical across platforms IF the compiler does not optimize the intermediate into an FMA or reassociate. With `/fp:precise` and `-ffp-contract=off`, standard `sqrtf` should be safe. We verify this with cross-platform testing.

2. **`sinf`/`cosf`**: Used only in `p_angle_vectors` to convert view angles to forward/right vectors. These are called once per tick per player. Options:
   - Use a polynomial approximation that we control, guaranteeing identical results.
   - Use a lookup table with linear interpolation (Q3 used this in some codepaths).
   - Decision: write our own `p_sinf`/`p_cosf` using a minimax polynomial. This is simple, fast, and portable.

### Our Deterministic sinf/cosf

```c
/* Attempt 1: Bhaskara I approximation (fast, ~0.001 max error)
   Good enough for converting view angles to direction vectors.

   Better option: 5th-order minimax polynomial on [-pi, pi].
   Max error < 1e-7, which is more than sufficient. */

#include <math.h>  /* for M_PI -- but define our own to be safe */
#define P_PI  3.14159265358979323846f
#define P_2PI 6.28318530717958647692f

static f32 p_sinf(f32 x) {
    /* Reduce x to [-PI, PI] */
    x = x - P_2PI * (f32)((i32)(x / P_2PI));
    if (x > P_PI)  x -= P_2PI;
    if (x < -P_PI) x += P_2PI;

    /* Minimax 5th-order polynomial for sin(x) on [-PI, PI] */
    /* Coefficients from Sollya or similar tool */
    f32 x2 = x * x;
    f32 x3 = x2 * x;
    f32 x5 = x3 * x2;
    return x - (x3 * 0.16666666641156543f) + (x5 * 0.008333303183547615f)
         - (x5 * x2 * 0.00019840874255389372f)
         + (x5 * x2 * x2 * 0.0000027557314205868495f);
}

static f32 p_cosf(f32 x) {
    return p_sinf(x + (P_PI * 0.5f));
}
```

Alternatively, we can use a Horner-form evaluation for better numerical behavior:

```c
static f32 p_sinf(f32 x) {
    /* range reduction to [-PI, PI] */
    /* ... */
    f32 x2 = x * x;
    /* sin(x) ~= x * (1 - x^2/6 + x^4/120 - x^6/5040 + x^8/362880) */
    f32 result = 1.0f;
    result = result * x2 * (-1.0f / 362880.0f) + (1.0f / 5040.0f);
    /* hmm, Horner form is messier for odd functions. Stick with explicit. */
    return x * (1.0f - x2 * (1.0f/6.0f - x2 * (1.0f/120.0f -
           x2 * (1.0f/5040.0f - x2 * (1.0f/362880.0f)))));
}
```

The exact polynomial coefficients should be determined by running Sollya or Remez algorithm for a 5th or 7th order minimax polynomial on [-pi, pi]. The key property is that the polynomial is evaluated using only additions and multiplications with explicit float constants, producing identical results on any IEEE-754 platform with `-ffp-contract=off`.

### Float Literal Discipline

Every floating-point literal MUST have the `f` suffix. Without it, the literal is `double`, and the expression is promoted to `double` arithmetic, then truncated back to `float`. This promotion can differ between compilers.

```c
f32 x = 3.14f;   /* correct */
f32 x = 3.14;    /* WRONG -- double literal, implicit narrowing */
```

### No `double` Intermediates

All physics arithmetic is `f32`. Never mix `f32` and `f64` in a computation. If you need extra precision for a specific calculation, do it entirely in `f64` and cast back at a defined point.

### Operation Order

With `/fp:precise` and `-ffp-contract=off`, the compiler respects C source-code operation order. `a + b + c` is evaluated as `(a + b) + c`. This means our code IS the specification -- if the source is identical, the results are identical. But we must be careful:

- Never rely on commutativity of floating-point addition in generated code.
- Use explicit parentheses where order matters.
- Avoid complex expressions that a compiler might legally reorder under C11 rules (C11 allows reassociation within an expression unless contracted, which we disabled).

Actually, C11 does NOT allow reassociation by default. The "as-if" rule requires the compiler to produce the same result as evaluating the expression left-to-right with standard associativity. So with precise float and no contraction, we are safe. The danger is only if someone turns on `-ffast-math` or forgets `-ffp-contract=off`.

---

## 9. Data Structures and Interfaces

### What Physics Receives from Other Modules

```c
/* From the engine core (map loader): */
typedef struct {
    p_brush_t *brushes;
    u32        brush_count;
} p_collision_model_t;

/* From input/netcode: */
typedef struct {
    f32 forward_move;     /* normalized -1 to 1 */
    f32 side_move;        /* normalized -1 to 1 */
    f32 up_move;          /* normalized -1 to 1 */
    f32 view_angles[3];   /* pitch, yaw, roll in degrees */
    u32 buttons;          /* bitmask */
    u32 server_time;      /* ms */
} p_usercmd_t;
```

### What Physics Provides to Other Modules

```c
/* To the renderer (interpolated state): */
typedef struct {
    vec3_t origin;        /* interpolated position */
    vec3_t velocity;      /* for effects, sound, etc. */
    f32    alpha;          /* interpolation fraction */
} p_render_state_t;

/* To netcode (snapshot state): */
typedef struct {
    vec3_t origin;
    vec3_t velocity;
    bool   on_ground;
    u32    command_time;
} p_net_state_t;

/* Full player state is also available for prediction/rollback: */
/* p_player_state_t (defined in section 4) */
```

### Public API (include/physics/physics.h)

```c
#ifndef QUICKEN_PHYSICS_H
#define QUICKEN_PHYSICS_H

#include "quicken.h"

/* ---- Types ---- */

typedef struct { f32 x, y, z; } vec3_t;

typedef struct {
    vec3_t normal;
    f32    dist;
} p_plane_t;

typedef struct {
    p_plane_t *planes;
    u32        plane_count;
    vec3_t     mins;
    vec3_t     maxs;
} p_brush_t;

typedef struct {
    p_brush_t *brushes;
    u32        brush_count;
} p_collision_model_t;

typedef struct {
    f32    fraction;
    vec3_t end_pos;
    vec3_t plane_normal;
    f32    plane_dist;
    bool   start_solid;
    bool   all_solid;
    i32    brush_index;
} p_trace_result_t;

typedef struct {
    f32 forward_move;
    f32 side_move;
    f32 up_move;
    f32 view_angles[3];
    u32 buttons;
    u32 server_time;
} p_usercmd_t;

typedef struct {
    vec3_t origin;
    vec3_t velocity;
    vec3_t mins;
    vec3_t maxs;
    bool   on_ground;
    vec3_t ground_normal;
    bool   jump_held;
    f32    max_speed;
    f32    gravity;
    u32    command_time;
} p_player_state_t;

/* Opaque world handle (holds collision model + spatial index) */
typedef struct p_world_s p_world_t;

typedef struct {
    f32 accumulator;
    u32 tick_count;
} p_time_state_t;

/* ---- Constants ---- */

#define PM_GROUND_ACCEL     10.0f
#define PM_AIR_ACCEL        1.0f
#define PM_GROUND_FRICTION  6.0f
#define PM_MAX_SPEED        320.0f
#define PM_JUMP_VELOCITY    270.0f
#define PM_GRAVITY          800.0f
#define PM_STEP_HEIGHT      18.0f
#define PM_OVERCLIP         1.001f
#define PM_STOP_SPEED       100.0f
#define PM_MIN_WALK_NORMAL  0.7f
#define PM_TICK_RATE        128
#define PM_TICK_DT          (1.0f / 128.0f)

#define BUTTON_JUMP         (1 << 0)
#define BUTTON_CROUCH       (1 << 1)
#define BUTTON_ATTACK       (1 << 2)

/* ---- Public API ---- */

/* Initialize the physics world from a collision model.
   Takes ownership of the collision model memory. */
p_world_t *qk_physics_world_create(p_collision_model_t *cm);
void       qk_physics_world_destroy(p_world_t *world);

/* Initialize a player state with default values */
void qk_physics_player_init(p_player_state_t *ps, vec3_t spawn_origin);

/* Run one physics tick. Call this from the fixed-timestep loop. */
void qk_physics_move(p_player_state_t *ps, const p_usercmd_t *cmd,
                     const p_world_t *world);

/* Fixed timestep update: feed real frame time, consumes fixed ticks. */
void qk_physics_update(p_time_state_t *ts, f32 frame_dt,
                       p_player_state_t *ps, const p_usercmd_t *cmd,
                       const p_world_t *world);

/* Trace a box through the world. */
p_trace_result_t qk_physics_trace(const p_world_t *world,
                                  vec3_t start, vec3_t end,
                                  vec3_t mins, vec3_t maxs);

/* Get interpolation alpha for rendering. */
f32 qk_physics_get_alpha(const p_time_state_t *ts);

#endif /* QUICKEN_PHYSICS_H */
```

### File Layout

```
src/physics/
  p_move.c       PM_Move, categorize position, jump check, friction
  p_accel.c      PM_Accelerate, PM_AirAccelerate
  p_slide.c      SlideMove, StepSlideMove, ClipVelocity
  p_trace.c      p_trace_brush, p_trace_world, ground check
  p_brush.c      brush AABB computation, Minkowski expansion helpers
  p_world.c      p_world_t creation, spatial index (brute force initially)
  p_math.c       deterministic sinf/cosf, vec3 operations, angle_vectors
  p_time.c       fixed timestep accumulator
  physics.c      public API (qk_physics_* wrappers), init/shutdown

include/physics/
  physics.h      public API (as defined above)
  p_internal.h   internal types and functions shared between .c files
  p_math.h       vec3_t operations, deterministic trig
```

---

## 10. Implementation Order

For the vertical slice, implement in this order. Each step is testable independently.

### Phase 1: Math Foundation
1. `p_math.h` / `p_math.c` -- vec3 operations (add, sub, scale, dot, cross, length, normalize), deterministic sinf/cosf, angle_vectors.
2. Add `-ffp-contract=off` to premake5.lua for GCC/Clang.
3. Write a cross-platform math verification test: compute a fixed sequence of operations, print results, diff between Windows and Linux builds.

### Phase 2: Collision Primitives
4. `p_brush.c` -- brush AABB computation from planes.
5. `p_trace.c` -- `p_trace_brush` (single brush trace), `p_trace_world` (all brushes, brute force).
6. Test: hard-code a simple room (6 brushes = floor/ceiling/4 walls) and verify traces produce correct fractions and normals.

### Phase 3: Movement Core
7. `p_accel.c` -- `p_accelerate`, `p_air_accelerate`.
8. `p_slide.c` -- `p_clip_velocity`, `p_slide_move`.
9. `p_move.c` -- `p_categorize_position`, `p_check_jump`, `p_apply_friction`, `p_move`.
10. `p_slide.c` -- `p_step_slide_move`.
11. Test: spawn a player above the floor. Verify they fall and land. Verify they can walk. Verify they can jump.

### Phase 4: Strafejumping Verification
12. Feed a synthetic input sequence: W+A held, yaw rotating at a fixed rate. Measure speed after N jumps.
13. Compare with known Q3 behavior: starting at 320 u/s, a good strafejump sequence should reach ~400+ u/s after 3-4 jumps, ~500+ after 8-10 jumps.
14. Tune if needed (but if the math is right, it should match without tuning).

### Phase 5: Integration
15. `p_time.c` -- fixed timestep accumulator.
16. `p_world.c` -- world creation from collision model provided by engine core.
17. `physics.c` -- public API wrappers.
18. Wire into engine main loop.

### Phase 6: Cross-Platform Determinism Verification
19. Record input sequence (array of `p_usercmd_t` over N ticks).
20. Run on Windows (MSVC) and Linux (GCC).
21. Diff the output (player origin and velocity after each tick).
22. They must be bit-identical. If not, find and fix the divergence.

---

## 11. Known Edge Cases and Q3 Quirks to Implement

- **Overbounce**: `PM_OVERCLIP = 1.001f` pushes velocity slightly away from collision planes. Without this, floating-point precision causes the player to slowly drift into brushes on subsequent frames.

- **Ramp jumping**: When jumping while running up a slope, the vertical velocity from the jump adds to the existing upward velocity component from the ramp, producing higher jumps. This is emergent from the math -- no special case needed, but verify it works.

- **No friction on jump frame**: As implemented in `p_check_jump`, setting `on_ground = false` before the friction step skips friction. This preserves full speed into the jump and is critical for consistent strafejump chains.

- **Start-solid handling**: If a trace starts inside a brush (`start_solid == true`), the trace reports `fraction = 0` but does not push the player out. A separate "unstick" routine may be needed for robustness, but is not in the vertical slice scope.

- **Ground snap**: After `StepSlideMove`, the player should snap to the ground surface. If the step-down trace lands on a walkable surface, accept it. This prevents the player from "floating" down stairs.

- **MAX_CLIP_PLANES = 5**: Q3 uses 5. If we hit this limit, it means the player is wedged between many surfaces and we zero their velocity. This is correct behavior.

- **Velocity clipping order**: In SlideMove, when clipping against multiple planes, we must check each clipped velocity against ALL previously accumulated planes. The two-plane crease case (sliding along the intersection of two planes) uses the cross product. The three-plane case zeros velocity. This matches Q3 exactly.

---

## 12. Things Explicitly NOT in Vertical Slice

- Water movement
- Crouch / crouchslide
- Ladders
- Moving platforms
- BVH spatial acceleration (brute force is sufficient)
- Networked prediction / rollback
- Multiple players
- Projectile physics
- Trigger volumes

These are all future work and the architecture supports adding them, but the vertical slice focuses on the core movement loop: run, jump, strafejump, collide with static brush geometry.
