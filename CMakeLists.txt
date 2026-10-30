cmake_minimum_required(VERSION 3.21)

project(ExplosionSim
    VERSION 0.10.5
    DESCRIPTION "Physically based 3D explosion simulation"
    LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ExplosionSim requires a 64-bit toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Static CRT keeps the produced executable self-contained (no VC++ redistributable).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(EXPLOSION_BUILD_TESTS "Build the physics and core tests" ON)
option(EXPLOSION_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${MSVC})
option(EXPLOSION_BUILD_APP "Build the graphical application" ${WIN32})
option(EXPLOSION_BUILD_VULKAN_SMOKE "Build the Vulkan bring-up test" ON)
set(EXPLOSION_RHI "d3d11" CACHE STRING "Graphics backend (d3d11; vulkan is planned)")
set(EXPLOSION_PLATFORM "win32" CACHE STRING "Platform layer (win32; sdl3 is planned)")

if(EXPLOSION_BUILD_APP AND NOT WIN32)
    message(STATUS "The Win32/D3D11 application target is Windows-only; "
                   "building the native Linux shell as explosion_linux.")
    set(EXPLOSION_BUILD_APP OFF)
endif()

if(EXPLOSION_BUILD_APP)
    if(NOT EXPLOSION_RHI STREQUAL "d3d11")
        message(FATAL_ERROR "Only the d3d11 RHI backend is implemented on Windows.")
    endif()
    if(NOT EXPLOSION_PLATFORM STREQUAL "win32")
        message(FATAL_ERROR "Only the win32 platform layer is implemented.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
function(ex_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
    if(EXPLOSION_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Generated version metadata
# ---------------------------------------------------------------------------
configure_file(cmake/Version.h.in ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)
if(WIN32)
    configure_file(cmake/ExplosionSim.rc.in ${CMAKE_BINARY_DIR}/generated/ExplosionSim.rc @ONLY)
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# DirectXMath is vendored so the simulation core can use its types on every
# platform. A small SAL shim is required outside the Windows SDK.
add_library(directxmath INTERFACE)
target_include_directories(directxmath INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/directxmath/Inc)
if(NOT WIN32)
    target_include_directories(directxmath INTERFACE
        ${CMAKE_SOURCE_DIR}/third_party/directxmath/compat)
endif()
if(NOT MSVC)
    # DirectXMath uses type-punned SIMD stores that GCC flags under -Wextra.
    target_compile_options(directxmath INTERFACE -Wno-strict-aliasing)
endif()

if(EXPLOSION_BUILD_VULKAN_SMOKE)
    # Vulkan-Headers are fetched so the backend can be built without a local
    # Vulkan SDK. The loader itself comes from the SDK or the system package.
    FetchContent_Declare(vulkan_headers
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.275.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(vulkan_headers)

    if(WIN32 AND DEFINED ENV{VULKAN_SDK})
        find_library(VULKAN_LIBRARY NAMES vulkan-1 HINTS "$ENV{VULKAN_SDK}/Lib")
    else()
        find_library(VULKAN_LIBRARY NAMES vulkan)
        if(NOT VULKAN_LIBRARY)
            # Runtime-only installs (common in WSL) ship libvulkan.so.1 without
            # the development symlink.
            find_file(VULKAN_LIBRARY NAMES libvulkan.so.1
                PATHS /usr/lib/x86_64-linux-gnu /usr/lib /lib/x86_64-linux-gnu /lib)
        endif()
    endif()

    if(VULKAN_LIBRARY)
        add_library(explosion_vulkan INTERFACE)
        target_include_directories(explosion_vulkan INTERFACE
            ${vulkan_headers_SOURCE_DIR}/include)
        target_link_libraries(explosion_vulkan INTERFACE ${VULKAN_LIBRARY})

        # SDL3 supplies the window, input and Vulkan surface for the smoke app
        # and, from M3 on, for the Linux application.
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        if(NOT WIN32)
            find_package(X11 QUIET)
            if(NOT X11_FOUND)
                # Headless/CI environments without desktop development headers
                # build SDL in console mode; the offscreen Vulkan path still
                # works and the windowed path is exercised on desktop builds.
                set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
                set(SDL_X11 OFF CACHE BOOL "" FORCE)
                set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
                set(SDL_KMSDRM OFF CACHE BOOL "" FORCE)
            endif()
        endif()
        FetchContent_Declare(sdl3
            URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.10.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(sdl3)
        if(TARGET SDL3::SDL3-static)
            set(EXPLOSION_SDL3_TARGET SDL3::SDL3-static)
        else()
            set(EXPLOSION_SDL3_TARGET SDL3::SDL3)
        endif()
    else()
        message(STATUS "Vulkan loader not found; skipping vulkan_smoke.")
    endif()
endif()

# Shared Vulkan RHI and SDL3 platform libraries. The smoke test and the Linux
# application use the same objects so their device setup cannot drift apart.
if(TARGET explosion_vulkan)
    add_library(explosion_vulkan_rhi STATIC
        src/gfx/rhi/vulkan/VulkanRhi.cpp)
    target_include_directories(explosion_vulkan_rhi PUBLIC src)
    target_link_libraries(explosion_vulkan_rhi PUBLIC explosion_vulkan)
    target_link_libraries(explosion_vulkan_rhi PRIVATE explosion_core)
    ex_target_warnings(explosion_vulkan_rhi)

    add_library(explosion_platform_sdl3 STATIC
        src/platform/sdl3/Sdl3Window.cpp)
    target_include_directories(explosion_platform_sdl3 PUBLIC src)
    target_link_libraries(explosion_platform_sdl3 PUBLIC
        ${EXPLOSION_SDL3_TARGET}
        explosion_vulkan)
    ex_target_warnings(explosion_platform_sdl3)
endif()

if(EXPLOSION_BUILD_APP)
    # Dear ImGui ships without a CMakeLists.txt; SOURCE_SUBDIR points to a
    # non-existent folder so FetchContent only downloads it.
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.zip
        URL_HASH SHA256=15C60AD10E331378CDA381002418F42EA2E98B9C05758EB493B23181BB38CA45
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR cmake/no-subdirectory)
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

    target_include_directories(imgui SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)

    target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_compile_options(imgui PRIVATE /W3 /utf-8)
endif()

# Jolt Physics (MIT) provides the rigid-body solver. The Build subdirectory
# contains the library project; when included through FetchContent it builds
# only the Jolt target and skips the samples/tests.
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
# The simulation only uses Jolt's CPU rigid bodies; skip its Vulkan compute
# shaders so a plain Vulkan loader install does not require glslc.
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(jolt
    URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.zip
    URL_HASH SHA256=0AF9BEEA51637EF805E624FE838EA2870F7B68CD48CBE1615C853BD9BCF4F1D7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(jolt)

if(EXPLOSION_BUILD_APP)
    # nlohmann/json (MIT) parses the GitHub releases API response. It is
    # header-only and only used by the update checker.
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
        URL_HASH SHA256=04022B05D806EB5FF73023C280B68697D12B93E1B7267A0B22A1A39EC7578069
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Keep the project standard explicit after adding the third-party subproject.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Core simulation (platform independent, unit tested)
# ---------------------------------------------------------------------------
add_library(explosion_core STATIC
    src/sim/BlastModel.cpp
    src/sim/ParticleSystem.cpp
    src/sim/Simulation.cpp
    src/sim/VolumeBaker.cpp
    src/scene/Scene.cpp
    src/scene/VoronoiFracture.cpp
    src/scene/PhysicsWorld.cpp
    src/gfx/SceneMeshLibrary.cpp
    src/core/ThreadPool.cpp
    src/core/Random.cpp)

target_include_directories(explosion_core PUBLIC src)
target_link_libraries(explosion_core PUBLIC directxmath PRIVATE Jolt)
if(WIN32)
    target_compile_definitions(explosion_core PUBLIC
        NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
endif()
target_compile_options(explosion_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/fp:fast>)
ex_target_warnings(explosion_core)

# ---------------------------------------------------------------------------
# Vulkan bring-up test (M2)
# ---------------------------------------------------------------------------
if(EXPLOSION_BUILD_VULKAN_SMOKE AND TARGET explosion_vulkan)
    add_executable(vulkan_smoke
        tools/vulkan_smoke/main.cpp)
    target_include_directories(vulkan_smoke PRIVATE src)
    target_link_libraries(vulkan_smoke PRIVATE
        explosion_vulkan_rhi
        explosion_platform_sdl3
        explosion_core)
    ex_target_warnings(vulkan_smoke)

    # Build-time HLSL -> SPIR-V validation. The smoke currently embeds the
    # generated bytecode, but this target keeps the shader sources compiling
    # with the exact Vulkan flags used for the embedded copies.
    find_program(DXC_EXECUTABLE NAMES dxc
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    if(DXC_EXECUTABLE)
        set(EXPLOSION_VULKAN_SHADERS
            sky.hlsl:FullscreenVS:vs_6_0
            sky.hlsl:PSSky:ps_6_0
            ground.hlsl:FullscreenVS:vs_6_0
            ground.hlsl:PSGround:ps_6_0
            composite.hlsl:FullscreenVS:vs_6_0
            composite.hlsl:PSComposite:ps_6_0
            bloom.hlsl:FullscreenVS:vs_6_0
            bloom.hlsl:PSDownsample:ps_6_0
            bloom.hlsl:PSUpsample:ps_6_0
            scene.hlsl:VSScene:vs_6_0
            scene.hlsl:PSScene:ps_6_0
            scene.hlsl:VSShadow:vs_6_0
            scene.hlsl:VSChunk:vs_6_0
            scene.hlsl:VSShadowChunk:vs_6_0
            shock.hlsl:FullscreenVS:vs_6_0
            shock.hlsl:PSShock:ps_6_0
            volume.hlsl:FullscreenVS:vs_6_0
            volume.hlsl:PSVolume:ps_6_0
            particles.hlsl:VSParticle:vs_6_0
            particles.hlsl:PSAdditive:ps_6_0
            particles.hlsl:PSFire:ps_6_0
            particles.hlsl:PSAlpha:ps_6_0)
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/spirv")
        set(EXPLOSION_SPIRV_OUTPUTS)
        foreach(shader IN LISTS EXPLOSION_VULKAN_SHADERS)
            string(REPLACE ":" ";" shader_parts "${shader}")
            list(GET shader_parts 0 shader_file)
            list(GET shader_parts 1 shader_entry)
            list(GET shader_parts 2 shader_profile)
            get_filename_component(shader_stem "${shader_file}" NAME_WE)
            set(shader_output
                "${CMAKE_BINARY_DIR}/spirv/${shader_stem}_${shader_entry}.spv")
            add_custom_command(OUTPUT "${shader_output}"
                COMMAND "${DXC_EXECUTABLE}"
                    -T "${shader_profile}"
                    -E "${shader_entry}"
                    -spirv -fvk-use-dx-layout -DEXPLOSION_VULKAN
                    -I "${CMAKE_SOURCE_DIR}/assets/shaders"
                    -Fo "${shader_output}"
                    "${CMAKE_SOURCE_DIR}/assets/shaders/${shader_file}"
                DEPENDS
                    "${CMAKE_SOURCE_DIR}/assets/shaders/${shader_file}"
                    "${CMAKE_SOURCE_DIR}/assets/shaders/common.hlsli"
                COMMENT "Compiling ${shader_file}:${shader_entry} to SPIR-V"
                VERBATIM)
            list(APPEND EXPLOSION_SPIRV_OUTPUTS "${shader_output}")
        endforeach()
        add_custom_target(vulkan_shaders DEPENDS ${EXPLOSION_SPIRV_OUTPUTS})
    else()
        message(STATUS "DXC not found; the vulkan_shaders target is unavailable.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Native Linux application shell (M2)
# ---------------------------------------------------------------------------
if(NOT WIN32 AND TARGET explosion_vulkan_rhi)
    add_executable(explosion_linux
        src/app/LinuxApp.cpp)
    target_include_directories(explosion_linux PRIVATE
        src
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(explosion_linux PRIVATE
        explosion_vulkan_rhi
        explosion_platform_sdl3
        explosion_core)
    ex_target_warnings(explosion_linux)

    add_custom_command(TARGET explosion_linux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/assets
                $<TARGET_FILE_DIR:explosion_linux>/assets
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/CHANGELOG.md
                $<TARGET_FILE_DIR:explosion_linux>/CHANGELOG.md
        COMMENT "Copying assets next to the Linux application")
endif()

# ---------------------------------------------------------------------------
# Update helper (standalone Win32 executable)
# ---------------------------------------------------------------------------
if(EXPLOSION_BUILD_APP)
    add_library(miniz STATIC
        third_party/miniz/miniz.c
        third_party/miniz/miniz_tinfl.c
        third_party/miniz/miniz_zip.c)
    target_include_directories(miniz PUBLIC third_party/miniz)
    target_compile_definitions(miniz PRIVATE MINIZ_NO_DEFLATE_APIS MINIZ_NO_STDIO)
    target_compile_options(miniz PRIVATE /W3 /O1 /GL)

    add_executable(ExplosionSimUpdater WIN32
        tools/updater/main.cpp
        src/update/Sha256.cpp)
    target_include_directories(ExplosionSimUpdater PRIVATE src)
    target_link_libraries(ExplosionSimUpdater PRIVATE miniz shell32 bcrypt wintrust crypt32)
    target_compile_definitions(ExplosionSimUpdater PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_options(ExplosionSimUpdater PRIVATE /W4 /permissive- /utf-8 /O1 /Gy /GL)
    target_link_options(ExplosionSimUpdater PRIVATE /LTCG /OPT:REF /OPT:ICF)
    if(EXPLOSION_WARNINGS_AS_ERRORS)
        target_compile_options(ExplosionSimUpdater PRIVATE /WX)
    endif()
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
if(EXPLOSION_BUILD_APP)
    add_executable(ExplosionSim WIN32
        src/main.cpp
        src/app/App.cpp
        src/app/Changelog.cpp
        src/app/Settings.cpp
        src/app/Splash.cpp
        src/gfx/GfxDevice.cpp
        src/gfx/rhi/d3d11/D3D11Rhi.cpp
        src/gfx/Renderer.cpp
        src/gfx/Camera.cpp
        src/gfx/Shader.cpp
        src/gfx/ImageIO.cpp
        src/gfx/GpuEulerSolver.cpp
        src/gfx/SceneRenderer.cpp
        src/ui/Ui.cpp
        src/update/Sha256.cpp
        src/update/UpdateChecker.cpp
        src/update/Version.cpp
        ${CMAKE_BINARY_DIR}/generated/ExplosionSim.rc)

    target_include_directories(ExplosionSim PRIVATE src ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(ExplosionSim PRIVATE
        explosion_core
        imgui
        nlohmann_json::nlohmann_json
        bcrypt
        d3d11
        dxgi
        d3dcompiler
        dwmapi
        winmm
        winhttp
        windowscodecs
        msimg32
        ole32
        shell32)

    target_compile_definitions(ExplosionSim PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _USE_MATH_DEFINES)
    target_compile_options(ExplosionSim PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/fp:fast>)
    ex_target_warnings(ExplosionSim)

    add_dependencies(ExplosionSim ExplosionSimUpdater)

    # Shaders are loaded at runtime so they can be tweaked without a rebuild.
    add_custom_command(TARGET ExplosionSim POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/assets
                $<TARGET_FILE_DIR:ExplosionSim>/assets
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/CHANGELOG.md
                $<TARGET_FILE_DIR:ExplosionSim>/CHANGELOG.md
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:ExplosionSimUpdater>
                $<TARGET_FILE_DIR:ExplosionSim>/ExplosionSimUpdater.exe
        COMMENT "Copying assets, changelog and updater next to the executable")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(EXPLOSION_BUILD_TESTS)
    enable_testing()

    # Portable core smoke tests (Windows and Linux).
    add_executable(core_tests
        tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE explosion_core)
    ex_target_warnings(core_tests)
    add_test(NAME core COMMAND core_tests)

    if(EXPLOSION_BUILD_APP)
        add_executable(explosion_tests
            tests/physics_tests.cpp
            src/app/Changelog.cpp
            src/app/Settings.cpp
            src/update/Sha256.cpp
            src/update/Version.cpp)
        target_link_libraries(explosion_tests PRIVATE explosion_core bcrypt)
        target_compile_definitions(explosion_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
        ex_target_warnings(explosion_tests)
        add_test(NAME physics COMMAND explosion_tests)

        # GPU smoke test: creates its own D3D11 device and exercises the solver.
        add_executable(gpu_tests
            tests/gpu_tests.cpp
            src/gfx/GpuEulerSolver.cpp
            src/gfx/Shader.cpp)
        target_link_libraries(gpu_tests PRIVATE explosion_core d3d11 dxgi d3dcompiler)
        target_compile_definitions(gpu_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
        ex_target_warnings(gpu_tests)
        add_custom_command(TARGET gpu_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_SOURCE_DIR}/assets
                    $<TARGET_FILE_DIR:gpu_tests>/assets
            COMMENT "Copying assets next to the GPU test")
        add_test(NAME gpu COMMAND gpu_tests)
    endif()
endif()
