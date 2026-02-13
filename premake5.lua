-- QUICKEN Engine Build Configuration
--
-- Targets:
--   quicken-physics    StaticLib   (precise float, determinism)
--   quicken-renderer   StaticLib   (fast float, performance)
--   quicken-netcode    StaticLib   (precise float, determinism)
--   quicken            ConsoleApp  (client executable)
--   quicken-server     ConsoleApp  (headless dedicated server)
--
-- IMPORTANT: Different modules use different floating-point settings.
-- See docs/ARCHITECTURE.md and docs/plans/INTEGRATION.md Section 4.6.

workspace "QUICKEN"
    architecture "x86_64"
    configurations { "Debug", "Release", "RelWithDebInfo" }
    startproject "quicken"

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

    filter "system:linux"
        defines { "_POSIX_C_SOURCE=200809L" }

    filter {}

--------------------------------------------------------------
-- Physics (precise float, cross-platform determinism)
-- Include path: include/ only (no SDL3, no Vulkan)
--------------------------------------------------------------
project "quicken-physics"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/physics")

    files {
        "src/physics/**.c",
        "src/physics/**.h",
        "include/physics/**.h"
    }

    includedirs {
        "include"
    }

    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-std=c11",
            "-ffp-contract=off"     -- REQUIRED: prevent FMA for determinism
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}

--------------------------------------------------------------
-- Renderer (fast float, aggressive optimizations)
-- Include path: include/, SDL3, Vulkan SDK
--------------------------------------------------------------
project "quicken-renderer"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/renderer")

    files {
        "src/renderer/**.c",
        "src/renderer/**.h",
        "include/renderer/**.h"
    }

    includedirs {
        "include",
        "src/renderer",
        "external/SDL3/include"
    }

    filter "system:windows"
        includedirs { "$(VULKAN_SDK)/Include" }

    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-ffast-math",
            "-std=c11"
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:fast"
        }

    filter {}

--------------------------------------------------------------
-- Netcode (precise float, platform sockets)
-- Include path: include/ only (no SDL3, no Vulkan)
--------------------------------------------------------------
project "quicken-netcode"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/netcode")

    files {
        "src/netcode/**.c",
        "src/netcode/**.h",
        "include/netcode/**.h"
    }

    includedirs {
        "include"
    }

    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-std=c11",
            "-ffp-contract=off"
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}

--------------------------------------------------------------
-- Main executable (client: window + renderer + all modules)
-- Include path: include/, SDL3
--------------------------------------------------------------
project "quicken"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/main")

    files {
        "src/*.c",
        "src/core/**.c",
        "src/gameplay/**.c",
        "src/gameplay/**.h",
        "src/ui/**.c",
        "include/**.h"
    }

    removefiles {
        "src/server_main.c"
    }

    includedirs {
        "include",
        "src/gameplay",
        "external/SDL3/include"
    }

    links {
        "quicken-physics",
        "quicken-renderer",
        "quicken-netcode"
    }

    filter "system:windows"
        system "windows"
        libdirs {
            "external/SDL3/build/Release",
            "$(VULKAN_SDK)/Lib"
        }
        links { "SDL3", "vulkan-1", "ws2_32" }
        postbuildcommands {
            "{MKDIR} %{cfg.targetdir}",
            "{COPY} external/SDL3/build/Release/SDL3.dll %{cfg.targetdir}"
        }

    filter "system:linux"
        system "linux"
        links { "SDL3", "vulkan", "m", "pthread" }
        libdirs { "external/SDL3/build-linux" }
        runpathdirs { "external/SDL3/build-linux" }

    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-std=c11",
            "-ffp-contract=off"
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}

--------------------------------------------------------------
-- Dedicated server (headless: no renderer, no SDL3, no Vulkan)
--------------------------------------------------------------
project "quicken-server"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/server")

    defines { "QK_HEADLESS" }

    files {
        "src/server_main.c",
        "src/core/**.c",
        "src/gameplay/**.c",
        "src/gameplay/**.h",
        "include/**.h"
    }

    removefiles {
        "src/core/qk_window.c",
        "src/core/qk_input.c"
    }

    includedirs {
        "include",
        "src/gameplay"
    }

    links {
        "quicken-physics",
        "quicken-netcode"
    }

    filter "system:windows"
        system "windows"
        links { "ws2_32" }

    filter "system:linux"
        system "linux"
        links { "m", "pthread" }

    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-std=c11",
            "-ffp-contract=off"
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}

--------------------------------------------------------------
-- Netcode loopback test (exercises full loopback path)
--------------------------------------------------------------
project "test-netcode"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/test-netcode")

    files {
        "tests/test_netcode_loopback.c"
    }

    includedirs {
        "include"
    }

    links {
        "quicken-netcode"
    }

    filter "system:windows"
        system "windows"
        links { "ws2_32" }

    filter "system:linux"
        system "linux"
        links { "m", "pthread" }

    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-std=c11",
            "-ffp-contract=off"
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}
