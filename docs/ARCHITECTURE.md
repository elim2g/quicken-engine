# QUICKEN Engine - Architecture

## Module Structure

The engine is separated into distinct modules with different compilation settings:

```
quicken-engine/
├── src/
│   ├── main.c              # Entry point
│   ├── core/               # Core systems (input, platform abstraction)
│   ├── physics/            # Physics simulation (PRECISE float)
│   └── renderer/           # Rendering (FAST float, aggressive opts)
├── include/
│   ├── quicken.h           # Main engine header
│   ├── core/               # Core system headers
│   ├── physics/            # Physics headers
│   └── renderer/           # Renderer headers
```

## Compilation Settings

### Physics Module (`quicken-physics`)
- **Float Mode**: Precise (`/fp:precise`, no `-ffast-math`)
- **Purpose**: Cross-platform determinism for networked gameplay
- **Contains**: Player movement, collision detection, game simulation
- **Performance**: Still highly optimized, just IEEE-754 compliant

### Renderer Module (`quicken-renderer`)
- **Float Mode**: Fast (`/fp:fast`, `-ffast-math`)
- **Purpose**: Maximum rendering performance
- **Contains**: Graphics pipeline, shader management, drawing
- **Performance**: Aggressive optimizations enabled

### Main Executable (`quicken`)
- **Float Mode**: Precise
- **Purpose**: Game logic, networking, main loop
- **Links**: Physics module + Renderer module + SDL3

## Why This Matters

### Networked Physics Consistency
With client prediction and server authority:
- Small floating-point differences can cause jitter
- Windows (MSVC) and Linux (GCC) need similar results
- Precise float mode keeps platforms in sync
- Server corrections happen less frequently

### Performance Reality
- Physics runs at **tick rate** (e.g., 125 Hz), not frame rate
- Rendering runs at **frame rate** (target: 1000+ fps)
- Most frame time is spent in rendering, not physics
- Aggressive opts where they matter, precise where needed

## Fixed-Point Consideration

Original QUAKE used fixed-point math for player movement. This is worth considering:

**Pros:**
- Perfect cross-platform determinism
- Fast on modern CPUs (integer ops)
- Proven approach (QUAKE, DOOM)

**Cons:**
- More implementation work
- Need to manage precision carefully

**Decision**: Start with precise floating-point, migrate critical paths to fixed-point if needed.

## Adding Code

- **Movement/collision/game rules** → `src/physics/`
- **Drawing/shaders/GPU** → `src/renderer/`
- **Input/window/platform** → `src/core/`
- **Game logic/networking** → `src/` (root level)

This keeps compilation boundaries clean and optimization flags correct.
