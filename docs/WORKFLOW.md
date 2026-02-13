# QUICKEN Engine -- Parallel Development Workflow

**Author**: Principal Engineer
**Status**: Canonical
**Date**: 2026-02-12

This document defines the operational workflow for multiple AI agents developing
the QUICKEN engine in parallel. Every agent MUST read and follow this document
before writing any code. Violating these rules will break other agents' builds.

This is an operational document. It contains exact commands and exact rules.
There is no ambiguity.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Phase 0 -- Contract Lock (Principal Engineer)](#2-phase-0----contract-lock-principal-engineer)
3. [Phase 1 -- Branch and Worktree Setup](#3-phase-1----branch-and-worktree-setup)
4. [Phase 2 -- Parallel Development Rules](#4-phase-2----parallel-development-rules)
5. [Phase 3 -- Testing in Isolation](#5-phase-3----testing-in-isolation)
6. [Phase 4 -- Integration Merges](#6-phase-4----integration-merges)
7. [Mid-Development Header Updates](#7-mid-development-header-updates)
8. [Interface Change Request Process](#8-interface-change-request-process)
9. [Agent Completion Checklist](#9-agent-completion-checklist)
10. [Worktree Cleanup](#10-worktree-cleanup)

---

## 1. Overview

The architecture is **interface-first, parallel implementation**. The Principal
Engineer commits all shared headers, stub implementations, and the full build
configuration to `main` BEFORE any agent branches off. Each agent then works
in an isolated git worktree on their own feature branch, implementing real
logic behind the already-compiling stubs. Agents never touch each other's
files. Integration happens at the end, in dependency order.

```
Timeline:

  Phase 0 ──> Phase 1 ──> Phase 2 ──────────────────> Phase 3 ──> Phase 4
  (PE locks   (branches   (parallel development        (test in    (merge in
   contracts)  created)    by all agents)                isolation)  DAG order)
```

---

## 2. Phase 0 -- Contract Lock (Principal Engineer)

The Principal Engineer commits the following to `main` BEFORE any agent
branches off. After Phase 0, `main` compiles and links on both Windows and
Linux with every module present as a no-op.

### 2.1 Shared Headers (complete type definitions and function signatures)

Every public header listed below must contain the FULL type definitions,
function signatures, constants, and include guards as specified in the
Integration Plan (`docs/plans/INTEGRATION.md`).

| Header                              | Contents                                                          |
|-------------------------------------|-------------------------------------------------------------------|
| `include/quicken.h`                 | Base types (u8..f64), platform detection, `qk_result_t`, assert   |
| `include/qk_math.h`                | `vec3_t`, `bbox_t`, inline math operations                        |
| `include/qk_types.h`               | `qk_usercmd_t`, `qk_player_state_t`, `qk_trace_result_t`, all enums, all `QK_` constants from Appendix B |
| `include/qk_arena.h`               | Arena allocator API (`qk_arena_create`, `qk_arena_alloc`, `qk_arena_reset`, `qk_arena_destroy`) |
| `include/physics/qk_physics.h`     | Physics world, move, trace API                                    |
| `include/renderer/qk_renderer.h`   | Renderer init, frame, world upload, UI quad, UI draw helpers      |
| `include/netcode/qk_netcode.h`     | Server/client lifecycle, tick, interpolation                      |
| `include/netcode/n_types.h`        | `n_entity_state_t`, `n_input_t`, netcode wire format types        |
| `include/gameplay/qk_gameplay.h`   | Game init/tick, player management, entity packing                 |
| `include/gameplay/g_local.h`       | Internal gameplay types (entity_t, weapon defs) -- NOT public     |
| `include/ui/qk_ui.h`              | HUD draw, events, tick                                            |
| `include/core/qk_platform.h`      | Platform time, sleep                                              |
| `include/core/qk_input.h`         | Input sampling API                                                |
| `include/core/qk_window.h`        | Window creation/management                                        |

### 2.2 Stub Implementations

Every public function declared in the headers above gets a stub `.c` file
that compiles, links, and does nothing meaningful. Stubs return `QK_SUCCESS`
for functions returning `qk_result_t`, zero/NULL for value-returning
functions, and do nothing for `void` functions.

Stub file locations:

| Stub File                          | Implements stubs for             |
|------------------------------------|----------------------------------|
| `src/physics/physics.c`           | All functions in `qk_physics.h`  |
| `src/renderer/renderer.c`         | All functions in `qk_renderer.h` |
| `src/netcode/netcode.c`           | All functions in `qk_netcode.h`  |
| `src/gameplay/gameplay.c`         | All functions in `qk_gameplay.h` |
| `src/ui/ui.c`                     | All functions in `qk_ui.h`       |
| `src/core/qk_arena.c`            | Arena allocator (REAL implementation, not a stub) |
| `src/core/qk_platform.c`         | All functions in `qk_platform.h` |
| `src/core/qk_input.c`            | All functions in `qk_input.h`    |
| `src/core/qk_window.c`           | All functions in `qk_window.h`   |

**Critical**: The arena allocator (`include/qk_arena.h` / `src/core/qk_arena.c`)
is a REAL implementation, not a stub. All modules depend on it for memory
allocation. It must be fully functional before any agent branches off.

### 2.3 Updated premake5.lua

The build configuration must include all four build targets as specified in
Section 6 of the Integration Plan:

| Target               | Kind         | Float Mode | Source Files                                    |
|----------------------|--------------|------------|-------------------------------------------------|
| `quicken-physics`    | StaticLib    | Precise    | `src/physics/**.c`                              |
| `quicken-renderer`   | StaticLib    | Fast       | `src/renderer/**.c`                             |
| `quicken-netcode`    | StaticLib    | Precise    | `src/netcode/**.c`                              |
| `quicken` (exe)      | ConsoleApp   | Precise    | `src/*.c`, `src/core/**`, `src/gameplay/**`, `src/ui/**` |

The executable links against all three static libraries plus SDL3 and
platform libraries (vulkan, ws2_32 on Windows, pthread on Linux).

Include directories are restricted per-target to enforce the dependency DAG:

- `quicken-physics`: `include` only (no SDL3, no Vulkan)
- `quicken-renderer`: `include`, `external/SDL3/include`, Vulkan SDK
- `quicken-netcode`: `include` only
- `quicken` (exe): `include`, `external/SDL3/include`

### 2.4 Phase 0 Verification

Phase 0 is complete when:

```
[ ] All headers listed in 2.1 exist with full type definitions and function signatures
[ ] All stub implementations listed in 2.2 exist and compile
[ ] Arena allocator (qk_arena.c) is fully implemented and tested
[ ] premake5.lua contains all four build targets per 2.3
[ ] `build.bat Release` succeeds on Windows (all targets compile and link)
[ ] `build.sh release` succeeds on Linux (all targets compile and link)
[ ] The quicken executable runs, prints version info, and exits cleanly
[ ] Everything is committed to main
```

---

## 3. Phase 1 -- Branch and Worktree Setup

After Phase 0 is committed to `main`, the Principal Engineer creates feature
branches and git worktrees for each agent. Each worktree is a fully
independent working directory with its own build output.

### 3.1 Directory Layout

```
H:\quicken\
    quicken-engine\            # main branch (Principal Engineer)
    quicken-engine-renderer\   # feat/renderer worktree
    quicken-engine-physics\    # feat/physics worktree
    quicken-engine-netcode\    # feat/netcode worktree
    quicken-engine-gameplay\   # feat/gameplay worktree
```

### 3.2 Setup Commands (run from the main repo)

```bash
cd H:\quicken\quicken-engine

# Create branches and worktrees
git branch feat/renderer
git worktree add ../quicken-engine-renderer feat/renderer

git branch feat/physics
git worktree add ../quicken-engine-physics feat/physics

git branch feat/netcode
git worktree add ../quicken-engine-netcode feat/netcode

git branch feat/gameplay
git worktree add ../quicken-engine-gameplay feat/gameplay
```

### 3.3 SDL3 Submodule Initialization

Each worktree needs the SDL3 submodule initialized. The build scripts
(`build.bat` / `build.sh`) handle this automatically by checking for
`external/SDL3/CMakeLists.txt` and running `git submodule update --init`
if missing. However, if you need to do it manually:

```bash
cd H:\quicken\quicken-engine-renderer
git submodule update --init
```

**Note**: Each worktree builds SDL3 independently into its own
`external/SDL3/build/` directory. This is by design -- worktrees must be
fully self-contained.

### 3.4 Verify Each Worktree Builds

Before any agent begins work, verify their worktree compiles:

```bash
# Windows (from each worktree root)
cd H:\quicken\quicken-engine-physics
build.bat Release

cd H:\quicken\quicken-engine-renderer
build.bat Release

cd H:\quicken\quicken-engine-netcode
build.bat Release

cd H:\quicken\quicken-engine-gameplay
build.bat Release
```

```bash
# Linux (from each worktree root)
cd /path/to/quicken-engine-physics
./build.sh release

cd /path/to/quicken-engine-renderer
./build.sh release

cd /path/to/quicken-engine-netcode
./build.sh release

cd /path/to/quicken-engine-gameplay
./build.sh release
```

If any worktree fails to build, Phase 0 was not completed correctly. Fix on
`main` and have all agents pull.

---

## 4. Phase 2 -- Parallel Development Rules

These rules are non-negotiable. Violating them will cause merge conflicts
and break other agents' builds.

### 4.1 File Ownership

Each agent owns specific directories and may ONLY modify files within them.

| Agent           | Worktree                        | CAN Modify                                         | CANNOT Modify                                  |
|-----------------|---------------------------------|----------------------------------------------------|-------------------------------------------------|
| Physics         | `quicken-engine-physics`        | `src/physics/*`                                    | Everything else                                 |
| Renderer        | `quicken-engine-renderer`       | `src/renderer/*`                                   | Everything else                                 |
| Netcode         | `quicken-engine-netcode`        | `src/netcode/*`                                    | Everything else                                 |
| Gameplay        | `quicken-engine-gameplay`       | `src/gameplay/*`, `src/ui/*`, `src/main.c`         | Everything else                                 |

**Explicit prohibitions** (applies to ALL agents):

1. **NEVER modify files in `include/`**. Those headers are the Principal
   Engineer's territory. They are the contracts all agents code against.
2. **NEVER modify `premake5.lua`**. The build configuration is owned by the
   Principal Engineer.
3. **NEVER modify `build.bat` or `build.sh`**. Build scripts are owned by
   the Principal Engineer.
4. **NEVER modify files in another module's `src/` directory**. The physics
   agent never touches `src/renderer/`. The renderer agent never touches
   `src/physics/`. No exceptions.
5. **NEVER modify `src/core/*`**. Core systems (platform, input, window,
   arena allocator) are owned by the Principal Engineer.

### 4.2 What Agents CAN Do

Within their owned directories, agents have full authority:

- **Create new `.c` files** for internal implementation.
- **Create internal `.h` files** within their `src/module/` directory for
  module-private declarations. Example: `src/physics/p_internal.h` is fine.
  These internal headers must NOT be placed in `include/`.
- **Replace stub implementations** with real code. The stub files become the
  real implementation files.
- **Add test code** within their own source directory or as temporary
  modifications to `src/main.c` on their branch (Gameplay agent only for
  `src/main.c`).

### 4.3 Build Integrity Rule

**Each branch MUST compile and link independently at every commit.** The stub
implementations for other modules ensure this. If you add a new `.c` file in
your module, it must compile against the existing headers without errors. If
you break the build on your branch, fix it before your next commit.

### 4.4 Include Discipline

Agents must respect the dependency DAG when including headers:

| Agent       | Allowed Includes from `include/`                                              |
|-------------|-------------------------------------------------------------------------------|
| Physics     | `quicken.h`, `qk_math.h`, `qk_types.h`, `qk_arena.h`, `physics/qk_physics.h` |
| Renderer    | `quicken.h`, `qk_math.h`, `qk_arena.h`, `renderer/qk_renderer.h`            |
| Netcode     | `quicken.h`, `qk_types.h`, `qk_arena.h`, `netcode/qk_netcode.h`, `netcode/n_types.h` |
| Gameplay    | `quicken.h`, `qk_math.h`, `qk_types.h`, `qk_arena.h`, `physics/qk_physics.h`, `netcode/n_types.h`, `gameplay/qk_gameplay.h`, `gameplay/g_local.h`, `ui/qk_ui.h`, `renderer/qk_renderer.h` |

**Violations the build system will catch**: The premake `includedirs` are
restricted per-target. If a physics source file tries to
`#include "renderer/qk_renderer.h"`, the build fails because the physics
static library's include path does not contain renderer headers.

**Violations the build system will NOT catch**: Including headers that are
on the include path but violate the logical dependency DAG (e.g., physics
including `qk_arena.h` is fine, but physics including `netcode/qk_netcode.h`
would be wrong even though `include/` is on its path). Agents must
self-enforce the logical DAG.

### 4.5 Naming Conventions Within Modules

All internal (non-public) functions and types within a module use the
module's short prefix:

| Module   | Internal Prefix | Example                    |
|----------|-----------------|----------------------------|
| Physics  | `p_`            | `p_accelerate()`, `p_trace_brush()` |
| Renderer | `r_`            | `r_vulkan_init()`, `r_create_pipeline()` |
| Netcode  | `n_`            | `n_bitpack_write()`, `n_server_send()` |
| Gameplay | `g_`            | `g_entity_alloc()`, `g_ca_tick()` |
| UI       | `ui_`           | `ui_draw_bar()`, `ui_crosshair_render()` |

Public functions (declared in `include/`) use the `qk_` prefix as defined
in the Integration Plan Section 2.2.

---

## 5. Phase 3 -- Testing in Isolation

Each agent can build and run their module against stub dependencies. The
stubs return zero/success/no-op, which means agents must construct synthetic
test data for their module's logic.

### 5.1 Physics Agent Testing

The physics module has no game-module dependencies. Test against hardcoded
brush geometry without the map loader.

**Approach**: Create a test harness in `src/physics/` that:
1. Constructs a `qk_collision_model_t` with 6 brushes forming a box room.
2. Creates a `qk_phys_world_t` from that collision model.
3. Runs `qk_physics_trace()` with known start/end positions and verifies
   `fraction`, `hit_normal`, and `end_pos`.
4. Constructs a `qk_player_state_t` and a sequence of `qk_usercmd_t`
   commands, then calls `qk_physics_move()` and verifies the resulting
   `origin` and `velocity`.
5. Validates strafejump speed gain matches Quake 3 expectations.

**Compile and run**: Build the full project. The `quicken` executable
links physics. Add temporary test calls in `src/main.c` (on the physics
branch, understanding that `src/main.c` modifications will be discarded at
merge time -- the gameplay agent owns `src/main.c`).

Alternatively, create a standalone test file within `src/physics/` that can
be called from a temporary `main` entry point on your branch.

### 5.2 Renderer Agent Testing

The renderer has no game-module dependencies. Display hardcoded geometry
without gameplay.

**Approach**: Create test code that:
1. Calls `qk_renderer_init()` with a config pointing to an SDL3 window.
2. Uploads a hardcoded cube or room using `qk_renderer_upload_world()`.
3. Builds a `qk_camera_t` from keyboard/mouse input (temporary camera
   controller).
4. Runs the render loop: `begin_frame`, `draw_world`, `push_ui_quad`,
   `end_frame`.
5. Verifies the Vulkan validation layer reports no errors.

### 5.3 Netcode Agent Testing

Netcode can loopback packets with synthetic entity states.

**Approach**: Create test code that:
1. Initializes a server with `qk_net_server_init()`.
2. Initializes a client and connects via loopback with `qk_net_client_connect_local()`.
3. Sends synthetic `qk_usercmd_t` inputs from client to server.
4. Sets synthetic `n_entity_state_t` on the server.
5. Runs server and client ticks.
6. Reads the interpolated state from `qk_net_client_get_interp_state()`.
7. Verifies that the entity positions match (within quantization tolerance).

### 5.4 Gameplay Agent Testing

Gameplay can simulate game logic with synthetic physics responses. Because
the physics stubs return zero/no-op, gameplay tests focus on the logic that
does NOT depend on real physics results.

**Approach**: Create test code that:
1. Calls `qk_game_init()` with a test config.
2. Connects test players with `qk_game_player_connect()`.
3. Sends synthetic commands with `qk_game_player_command()`.
4. Runs `qk_game_tick()` with a NULL or stub physics world.
5. Verifies the Clan Arena state machine transitions: WARMUP -> COUNTDOWN ->
   PLAYING -> ROUND_END -> WARMUP.
6. Verifies weapon fire, damage application, kill tracking.
7. Verifies entity packing with `qk_game_pack_entity()`.

**Note**: Full integration testing with real physics happens after the
Phase 4 merges. Gameplay tests during Phase 2 validate the game logic
layer in isolation.

---

## 6. Phase 4 -- Integration Merges

Merges happen on `main`, in dependency DAG order, performed by the
Principal Engineer.

### 6.1 Merge Order

```
1. feat/physics   --> main    (no game-module dependencies)
2. feat/renderer  --> main    (no game-module dependencies)
3. feat/netcode   --> main    (no game-module dependencies)
4. feat/gameplay  --> main    (depends on physics, netcode, renderer)
```

Steps 1-3 can happen in any order relative to each other since physics,
renderer, and netcode have no cross-dependencies. Step 4 MUST happen last.

### 6.2 Merge Procedure (for each branch)

```bash
cd H:\quicken\quicken-engine

# 1. Ensure main is up to date
git checkout main

# 2. Merge the feature branch
git merge feat/physics --no-ff -m "Merge feat/physics: real physics implementation"

# 3. Resolve any interface-requests.md items (see Section 8)
#    If header changes are needed, make them now on main.

# 4. Build and verify on Windows
build.bat Release

# 5. Build and verify on Linux (or in CI)
# ./build.sh release

# 6. Run the executable to verify no crashes
build\bin\Release-windows-x86_64\quicken.exe

# 7. Commit any interface-request resolutions
# 8. Only then proceed to the next merge
```

### 6.3 Conflict Resolution

Because agents own disjoint file sets, merge conflicts should be rare.
The only potential conflict areas are:

- **`src/main.c`**: The gameplay agent modifies this, and the physics
  agent may have added temporary test code. Resolution: the gameplay
  agent's version wins. Any temporary test code from other agents is
  discarded.

If unexpected conflicts arise in `include/` or `premake5.lua`, something
went wrong -- an agent modified files they do not own. The Principal
Engineer investigates and reverts the offending changes.

### 6.4 Post-Merge Integration

After all four branches are merged, the Principal Engineer:

1. Wires `src/main.c` to implement the full game loop as specified in
   Section 3.3 of the Integration Plan (local play with loopback).
2. Removes any remaining stub code that was replaced by real
   implementations.
3. Builds and runs the integrated system.
4. Tags the commit as `v0.1.0-vertical-slice` (or similar).

---

## 7. Mid-Development Header Updates

Sometimes the Principal Engineer needs to update a shared header during
Phase 2 (e.g., a bug in `qk_types.h`, a missing constant, or a field
type correction).

### 7.1 Procedure

1. The Principal Engineer commits the fix to `main`.
2. The Principal Engineer notifies all agents that `main` has been updated.
3. Each agent rebases their feature branch onto the updated `main`:

```bash
# Example: Physics agent rebases
cd H:\quicken\quicken-engine-physics
git fetch origin main
git rebase origin/main
```

If the rebase has conflicts (rare, since agents do not modify `include/`
files), resolve them by keeping the new `main` version of any shared header.

4. Each agent rebuilds to verify their code still compiles against the
   updated headers.

### 7.2 When NOT to Update Headers

Do NOT update headers mid-development for:
- New function signatures requested by an agent (use the interface request
  process in Section 8 instead)
- "Nice to have" additions that no agent currently needs
- Refactoring header organization

Header updates during Phase 2 are reserved for **bug fixes and corrections
only**. New API surface is deferred to the Phase 4 merge window.

---

## 8. Interface Change Request Process

If an agent discovers they need a change to a shared header (new function,
modified signature, new type, new constant), they MUST NOT edit the header
themselves. Instead, they file a request.

### 8.1 Request File Location

All requests go in: `docs/plans/interface-requests.md`

This file is created by the Principal Engineer during Phase 0 (initially
empty). Agents append to it on their branch. The Principal Engineer reviews
requests during Phase 4 merges.

### 8.2 Request Template

Each request is a markdown section appended to the file:

```markdown
---

### [REQUEST] <Short title>

**Requesting agent**: <Physics | Renderer | Netcode | Gameplay>
**Date**: <YYYY-MM-DD>
**Priority**: <Blocking | Nice-to-have>
**Header**: <e.g., include/qk_types.h>

**Current signature / type**:
```c
// Paste the current declaration, or "NEW" if requesting a new addition
```

**Proposed change**:
```c
// Paste the exact proposed declaration
```

**Rationale**: <Why this change is needed. What breaks without it.>

**Workaround**: <If Priority is Nice-to-have, describe how the agent is
working around it currently.>

**Resolution**: <Filled in by Principal Engineer: APPROVED / REJECTED / MODIFIED>
```

### 8.3 Blocking vs Nice-to-have

- **Blocking**: The agent cannot complete their implementation without this
  change. The Principal Engineer should evaluate immediately and may apply
  the change to `main` mid-development (see Section 7).
- **Nice-to-have**: The agent can work around it. The change is deferred to
  the Phase 4 merge window.

### 8.4 Agent Workaround While Waiting

If an agent needs a function that does not exist in the shared headers, they
can define it as a module-internal function in their `src/module/` directory.
During Phase 4, the Principal Engineer can promote it to the public API if
the request is approved.

Example: The physics agent needs a `vec3_lerp()` function not in `qk_math.h`.

```c
/* src/physics/p_internal.h */
static inline vec3_t p_vec3_lerp(vec3_t a, vec3_t b, f32 t) {
    return vec3_add(vec3_scale(a, 1.0f - t), vec3_scale(b, t));
}
```

This compiles, does not touch `include/`, and can be migrated to `qk_math.h`
later if the request is approved.

---

## 9. Agent Completion Checklist

Before an agent considers their work "done" on their feature branch, they
must verify ALL of the following:

### 9.1 Build Verification

```
[ ] `build.bat Release` succeeds with zero errors on Windows
[ ] `build.bat Debug` succeeds with zero errors on Windows
[ ] `build.sh release` succeeds with zero errors on Linux (if available)
[ ] `build.sh debug` succeeds with zero errors on Linux (if available)
[ ] Zero compiler warnings on the agent's owned source files
```

### 9.2 Code Quality

```
[ ] All public API functions from the module's header are implemented (no remaining stubs)
[ ] All public functions use the `qk_` prefix as declared in the shared header
[ ] All internal functions use the module short prefix (p_, r_, n_, g_, ui_)
[ ] No calls to malloc/free in hot paths (use arena allocator)
[ ] No compiler extensions or non-C11 code
[ ] No `#include` of headers outside the allowed set (Section 4.4)
```

### 9.3 File Ownership

```
[ ] No files in `include/` were modified
[ ] No files in `premake5.lua`, `build.bat`, or `build.sh` were modified
[ ] No files in another module's `src/` directory were modified
[ ] No files in `src/core/` were modified
[ ] All new files are within the agent's owned directories only
```

### 9.4 Testing

```
[ ] Module-specific tests pass (see Section 5)
[ ] The full `quicken` executable compiles and links (even though other modules are stubs)
[ ] The executable does not crash on startup/shutdown
```

### 9.5 Interface Requests

```
[ ] Any needed interface changes are documented in docs/plans/interface-requests.md
[ ] Blocking requests have been communicated to the Principal Engineer
[ ] Workarounds are in place for any unresolved requests
```

### 9.6 Git Hygiene

```
[ ] All changes are committed (no uncommitted files)
[ ] Commit messages are clear and describe what changed
[ ] No temporary debug prints left in committed code (or they are gated behind QUICKEN_DEBUG)
[ ] No temporary test hacks left in src/main.c (except for the Gameplay agent, who owns it)
```

---

## 10. Worktree Cleanup

After Phase 4 integration is complete and the vertical slice is working
on `main`, the Principal Engineer removes the worktrees and deletes the
feature branches.

### 10.1 Remove Worktrees

```bash
cd H:\quicken\quicken-engine

git worktree remove ../quicken-engine-renderer
git worktree remove ../quicken-engine-physics
git worktree remove ../quicken-engine-netcode
git worktree remove ../quicken-engine-gameplay
```

### 10.2 Delete Feature Branches

```bash
git branch -d feat/renderer
git branch -d feat/physics
git branch -d feat/netcode
git branch -d feat/gameplay
```

Use `-d` (not `-D`) to ensure git warns if the branches have unmerged
changes.

### 10.3 Verify Clean State

```bash
git worktree list
# Should show only the main working tree:
# H:\quicken\quicken-engine  <commit-hash> [main]

git branch
# Should show only:
# * main
```

---

## Appendix A: Quick Reference -- Agent File Ownership

```
Physics agent (H:\quicken\quicken-engine-physics):
    OWNS:    src/physics/*
    READS:   include/quicken.h
             include/qk_math.h
             include/qk_types.h
             include/qk_arena.h
             include/physics/qk_physics.h

Renderer agent (H:\quicken\quicken-engine-renderer):
    OWNS:    src/renderer/*
    READS:   include/quicken.h
             include/qk_math.h
             include/qk_arena.h
             include/renderer/qk_renderer.h

Netcode agent (H:\quicken\quicken-engine-netcode):
    OWNS:    src/netcode/*
    READS:   include/quicken.h
             include/qk_types.h
             include/qk_arena.h
             include/netcode/qk_netcode.h
             include/netcode/n_types.h

Gameplay agent (H:\quicken\quicken-engine-gameplay):
    OWNS:    src/gameplay/*
             src/ui/*
             src/main.c
    READS:   include/quicken.h
             include/qk_math.h
             include/qk_types.h
             include/qk_arena.h
             include/physics/qk_physics.h
             include/netcode/n_types.h
             include/gameplay/qk_gameplay.h
             include/gameplay/g_local.h
             include/ui/qk_ui.h
             include/renderer/qk_renderer.h
```

## Appendix B: Quick Reference -- Git Commands

**Agent rebasing onto updated main:**
```bash
cd <worktree-directory>
git fetch origin main
git rebase origin/main
# Resolve conflicts if any (keep main's version of include/ files)
# Rebuild to verify
```

**Agent checking what they have changed (before committing):**
```bash
cd <worktree-directory>
git status
git diff --stat
# Verify no files outside owned directories appear in the diff
```

**Principal Engineer checking all worktree states:**
```bash
cd H:\quicken\quicken-engine
git worktree list
# Then check each branch:
git log --oneline main..feat/physics
git log --oneline main..feat/renderer
git log --oneline main..feat/netcode
git log --oneline main..feat/gameplay
```

## Appendix C: Build Commands Reference

**Windows (from any worktree root):**
```batch
build.bat Release       REM Release build
build.bat Debug         REM Debug build with asserts and symbols
build.bat RelWithDebInfo  REM Release with debug symbols (for profiling)
```

**Linux (from any worktree root):**
```bash
./build.sh release        # Release build
./build.sh debug          # Debug build with asserts and symbols
./build.sh relwithdebinfo # Release with debug symbols
```

**Running the executable (Windows):**
```batch
build\bin\Release-windows-x86_64\quicken.exe
```

**Running the executable (Linux):**
```bash
./build/bin/release-linux-x86_64/quicken
```

---

*End of Workflow Document.*
