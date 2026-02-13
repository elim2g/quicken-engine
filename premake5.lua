-- QUICKEN Engine Build Configuration
-- Targets: Windows (MSVC, MinGW) and Linux (GCC, Clang)
--
-- IMPORTANT: Physics and rendering use different floating-point settings
-- for cross-platform determinism while maintaining maximum performance

workspace "QUICKEN"
    architecture "x86_64"
    configurations { "Debug", "Release", "RelWithDebInfo" }
    startproject "quicken"

    -- Output directories
    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

    filter "configurations:Debug"
        defines { "QUICKEN_DEBUG" }
        runtime "Debug"
        symbols "On"
        optimize "Off"

    filter "configurations:Release"
        defines { "QUICKEN_RELEASE", "NDEBUG" }
        runtime "Release"
        symbols "Off"
        optimize "Speed"

    filter "configurations:RelWithDebInfo"
        defines { "QUICKEN_RELEASE", "NDEBUG" }
        runtime "Release"
        symbols "On"
        optimize "Speed"

    filter {}

-- Physics module (precise floating-point for cross-platform determinism)
project "quicken-physics"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/physics")

    files {
        "src/physics/**.c",
        "include/physics/**.h"
    }

    includedirs {
        "include",
        "external/SDL3/include"
    }

    -- Precise floating-point for determinism
    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall",
            "-Wextra",
            "-march=native",
            "-std=c11"
            -- NO -ffast-math here!
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"  -- Precise floating-point
        }

    filter {}

-- Netcode module (precise floating-point for deterministic state sync)
project "quicken-netcode"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/netcode")

    files {
        "src/netcode/**.c",
        "include/netcode/**.h"
    }

    includedirs {
        "include",
        "external/SDL3/include"
    }

    -- Precise floating-point: all state that crosses the network must
    -- produce identical results on all machines
    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall",
            "-Wextra",
            "-march=native",
            "-std=c11"
            -- NO -ffast-math — determinism required
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}

-- Renderer module (aggressive optimizations for maximum FPS)
project "quicken-renderer"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/renderer")

    files {
        "src/renderer/**.c",
        "include/renderer/**.h"
    }

    includedirs {
        "include",
        "external/SDL3/include"
    }

    -- Aggressive optimizations for rendering performance
    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall",
            "-Wextra",
            "-march=native",
            "-ffast-math",     -- FAST math for rendering
            "-std=c11"
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:fast"         -- FAST floating-point
        }

    filter {}

-- Main QUICKEN executable
project "quicken"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/main")

    files {
        "src/*.c",           -- Only root-level source files
        "src/core/**.c",     -- Core systems (input, platform, etc.)
        "include/**.h"
    }

    includedirs {
        "include",
        "external/SDL3/include"
    }

    -- Link against our physics, netcode, and renderer modules
    links {
        "quicken-physics",
        "quicken-netcode",
        "quicken-renderer"
    }

    -- Platform-specific settings
    filter "system:windows"
        system "windows"
        libdirs { "external/SDL3/build/Release" }
        links { "SDL3" }
        -- Copy SDL3.dll to output directory
        postbuildcommands {
            "{MKDIR} %{cfg.targetdir}",
            "{COPY} external/SDL3/build/Release/SDL3.dll %{cfg.targetdir}"
        }

    filter "system:linux"
        system "linux"
        links { "SDL3", "m", "pthread" }
        libdirs { "external/SDL3/build-linux" }
        runpathdirs { "external/SDL3/build-linux" }

    -- Base compiler flags (precise floating-point for game logic)
    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall",
            "-Wextra",
            "-march=native",
            "-std=c11"
            -- NO -ffast-math for game logic
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"      -- Precise for game logic
        }

    filter {}
