cmake_minimum_required(VERSION 3.21)

project(ExplosionSim
    VERSION 0.5.5
    DESCRIPTION "Physically based 3D explosion simulation for Windows x64"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ExplosionSim targets Windows only.")
endif()

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
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(EXPLOSION_BUILD_TESTS "Build the physics unit tests" ON)
option(EXPLOSION_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# ---------------------------------------------------------------------------
# Generated version metadata
# ---------------------------------------------------------------------------
configure_file(cmake/Version.h.in ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)
configure_file(cmake/ExplosionSim.rc.in ${CMAKE_BINARY_DIR}/generated/ExplosionSim.rc @ONLY)

# ---------------------------------------------------------------------------
# Third-party dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

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

# Jolt Physics (MIT) provides the rigid-body solver. The Build subdirectory
# contains the library project; when included through FetchContent it builds
# only the Jolt target and skips the samples/tests.
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(jolt
    URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.zip
    URL_HASH SHA256=0AF9BEEA51637EF805E624FE838EA2870F7B68CD48CBE1615C853BD9BCF4F1D7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(jolt)

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
    src/core/ThreadPool.cpp
    src/core/Random.cpp)

target_include_directories(explosion_core PUBLIC src)
target_compile_definitions(explosion_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
target_compile_options(explosion_core PRIVATE /W4 /permissive- /fp:fast /utf-8)
target_link_libraries(explosion_core PRIVATE Jolt)
if(EXPLOSION_WARNINGS_AS_ERRORS)
    target_compile_options(explosion_core PRIVATE /WX)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(ExplosionSim WIN32
    src/main.cpp
    src/app/App.cpp
    src/app/Settings.cpp
    src/gfx/GfxDevice.cpp
    src/gfx/Renderer.cpp
    src/gfx/Camera.cpp
    src/gfx/Shader.cpp
    src/gfx/ImageIO.cpp
    src/gfx/GpuEulerSolver.cpp
    src/gfx/SceneRenderer.cpp
    src/ui/Ui.cpp
    ${CMAKE_BINARY_DIR}/generated/ExplosionSim.rc)

target_include_directories(ExplosionSim PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(ExplosionSim PRIVATE
    explosion_core
    imgui
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    winmm
    windowscodecs
    ole32
    shell32)

target_compile_definitions(ExplosionSim PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _USE_MATH_DEFINES)
target_compile_options(ExplosionSim PRIVATE /W4 /permissive- /fp:fast /utf-8)
if(EXPLOSION_WARNINGS_AS_ERRORS)
    target_compile_options(ExplosionSim PRIVATE /WX)
endif()

# Shaders are loaded at runtime so they can be tweaked without a rebuild.
add_custom_command(TARGET ExplosionSim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:ExplosionSim>/assets
    COMMENT "Copying assets next to the executable")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(EXPLOSION_BUILD_TESTS)
    enable_testing()
    add_executable(explosion_tests
        tests/physics_tests.cpp
        src/app/Settings.cpp)
    target_link_libraries(explosion_tests PRIVATE explosion_core)
    target_compile_definitions(explosion_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
    target_compile_options(explosion_tests PRIVATE /W4 /permissive- /utf-8)
    if(EXPLOSION_WARNINGS_AS_ERRORS)
        target_compile_options(explosion_tests PRIVATE /WX)
    endif()
    add_test(NAME physics COMMAND explosion_tests)

    # GPU smoke test: creates its own D3D11 device and exercises the solver.
    add_executable(gpu_tests
        tests/gpu_tests.cpp
        src/gfx/GpuEulerSolver.cpp
        src/gfx/Shader.cpp)
    target_link_libraries(gpu_tests PRIVATE explosion_core d3d11 dxgi d3dcompiler)
    target_compile_definitions(gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
    target_compile_options(gpu_tests PRIVATE /W4 /permissive- /utf-8)
    if(EXPLOSION_WARNINGS_AS_ERRORS)
        target_compile_options(gpu_tests PRIVATE /WX)
    endif()
    add_custom_command(TARGET gpu_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/assets
                $<TARGET_FILE_DIR:gpu_tests>/assets
        COMMENT "Copying assets next to the GPU test")
    add_test(NAME gpu COMMAND gpu_tests)
endif()
